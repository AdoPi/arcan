/*
 * Copyright 2017, Björn Ståhl
 * License: 3-Clause BSD, see COPYING file in arcan source repository
 * Reference: http://arcan-fe.com, README.MD
 * Description: fork()+sandbox asynch- image loading wrapper around stbi
 * Bad assumption with a 4 bpp fixed output format (needs revision for
 * float,...) and needs a wider range of sandbox- format supports. If/ when
 * good enough, this can be re-worked into main arcan to replace the _img
 * functions in the main pipeline.
 */
#include <arcan_shmif.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>

#ifdef ENABLE_SECCOMP
	#include <seccomp.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "imgload.h"

bool imgload_spawn(struct arcan_shmif_cont* con, struct img_state* tgt)
{
/* pre-alloc the upper limit for the return- image, we'll munmap when
 * finished to look less memory hungry */
	if (tgt->out)
		munmap((void*)tgt->out, tgt->buf_lim);
	tgt->buf_lim = image_size_limit_mb * 1024 * 1024;
	tgt->out = mmap(NULL, tgt->buf_lim,
		PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (!tgt->out)
		return false;
	memset((void*)tgt->out, '\0', sizeof(struct img_data));

/* spawn our worker process */
	tgt->proc = fork();
	if (-1 == tgt->proc){
		munmap((void*)tgt->out, tgt->buf_lim);
		tgt->out = NULL;
		return false;
	}
/* parent */
	else if (0 != tgt->proc)
		return true;

/* start with the most risk- prone, original fopen */
	FILE* inf = (tgt->is_stdin? stdin : fopen(tgt->fname, "r"));
	if (!inf)
		exit(EXIT_FAILURE);

/* close the parent pipes in the safest way possible, if that fails, accept UB
 * and kill the streams (other option would be replacing with memstreams) */
	int nfd = open("/dev/null", O_RDWR);
	if (-1 != nfd){
		if (inf != stdin){
			dup2(nfd, STDIN_FILENO);
		}
		dup2(nfd, STDOUT_FILENO);
		dup2(nfd, STDERR_FILENO);
		close(nfd);
	}
	else{
		if (inf != stdin)
			fclose(stdin);
		fclose(stderr);
		fclose(stdout);
	}

/* drop shm-con so that it's not around anymore - owning the parser won't grant
 * us direct access to the shm- connection and with the syscalls eliminated, it
 * can't be re-opened */
	if (con){
		munmap(con->addr, con->shmsize);
		close(con->epipe);
		close(con->shmh);
		memset(con, '\0', sizeof(struct arcan_shmif_cont));
	}

/* someone might've needed to be careless and run as root, if they fail, well
 * they fail, it's added safety - not a guarantee */
	setgid(65534);
	setuid(65534);


/* set some limits that will make things worse even if we don't have seccmp */
	setrlimit(RLIMIT_CORE, &(struct rlimit){});
	setrlimit(RLIMIT_FSIZE, &(struct rlimit){});
	setrlimit(RLIMIT_NOFILE, &(struct rlimit){});
	setrlimit(RLIMIT_NPROC, &(struct rlimit){});

#ifdef ENABLE_SECCOMP
	if (!disable_syscall_flt){
		prctl(PR_SET_NO_NEW_PRIVS, 1);
		prctl(PR_SET_DUMPABLE, 0);
		scmp_filter_ctx flt = seccomp_init(SCMP_ACT_KILL);
		seccomp_rule_add(flt, SCMP_ACT_ALLOW, SCMP_SYS(mmap), 0);
		seccomp_rule_add(flt, SCMP_ACT_ALLOW, SCMP_SYS(brk), 0);
		seccomp_rule_add(flt, SCMP_ACT_ALLOW, SCMP_SYS(exit), 0);
		seccomp_rule_add(flt, SCMP_ACT_ALLOW, SCMP_SYS(fstat), 0);
		seccomp_rule_add(flt, SCMP_ACT_ALLOW, SCMP_SYS(read), 0);
		seccomp_rule_add(flt, SCMP_ACT_ALLOW, SCMP_SYS(munmap), 0);
		seccomp_rule_add(flt, SCMP_ACT_ALLOW, SCMP_SYS(lseek), 0);
		seccomp_rule_add(flt, SCMP_ACT_ALLOW, SCMP_SYS(exit_group), 0);
//	seccomp_rule_add(flt, SCMP_ACT_ALLOW, SCMP_SYS(rt_sigreturn), 0);
		seccomp_load(flt);
	}
#endif

/* now we're in the dangerous part, stbi_load_from_file - though the source is
 * just a filestream, we need to return the decoded buffer somehow. STBI may
 * need intermediate allocations and we can't really tell, so unfortunately we
 * waste an extra memcpy.
 *
 * Decent optimizations needed here and not in place now:
 * 1. custom 'upper limit' malloc so we can drop the mmap/brk/munmap syscalls
 * 2. patch stbi- to use a separate allocator for our output buffer so
 *    that the decode writes directly to tgt->out->buf, saving us a
 *    memcpy.
 * 3. pre-mmaping file and using stbi-load-from-memory could drop the
 *    fstat/read/lseek syscalls as well.
 *
 * The custom allocator will likely also be needed for this to work in a
 * multithreaded setting.
 */
	int dw, dh;
	uint8_t* buf = stbi_load_from_file(inf, &dw, &dh, NULL, 4);
	if (buf){
		memcpy((void*) tgt->out->buf, buf, dw * dh * 4);
		tgt->out->w = dw;
		tgt->out->h = dh;
		tgt->out->buf_sz = dw * dh * 4;
		tgt->out->ready = true;
		exit(EXIT_SUCCESS);
	}
	exit(EXIT_FAILURE);
}

bool imgload_poll(struct img_state* tgt)
{
	if (!tgt->proc || !tgt->out)
		return true;

	int sc = 0;
	int rc;
	while ((rc = waitpid(tgt->proc, &sc, WNOHANG)) == -1 && errno == EINTR){}
/* failed */
	if (rc == -1){
		tgt->broken = true;
		return true;
	}

/* nothing new */
	if (rc == 0)
		return false;

/* client tries to exceed buffer, signs of a troublemaker */
	size_t out_sz = tgt->out->buf_sz;
	if (out_sz > tgt->buf_lim){
		munmap((void*)tgt->out, tgt->buf_lim);
		tgt->proc = 0;
		tgt->out = NULL;
		tgt->broken = true;
		return true;
	}

	printf("poll to broken: %d\n", sc);
	tgt->broken = sc != EXIT_SUCCESS;
	tgt->proc = 0;
	tgt->out->x = tgt->out->y = 0;

/* unmap extra data, align with page size */
	uintptr_t system_page_size = sysconf(_SC_PAGE_SIZE);
	uintptr_t base = (uintptr_t) tgt->out;
	uintptr_t end = base + sizeof(struct img_data) + out_sz;
	if (end % system_page_size != 0)
		end += system_page_size - end % system_page_size;

/* if fork/exec/... are not closed off, there's the sigbus possiblity left,
 * though DOS isn't much of a threat model here, our real worry is code-exec */
	size_t ntr = tgt->buf_lim - (end - base);
/* Odd case where we we couldn't poke hole in the mapping, simply skip the
 * truncate step. This affects accounting in playlists slightly */
	if (0 == munmap((void*) end, ntr))
		tgt->buf_lim -= ntr;

	return true;
}

/*
 * reset the contents of imgload so that it can be used for a new imgload_spawn
 * only run after imgload_poll has returned true once.
 */
void imgload_reset(struct img_state* tgt)
{
	if (tgt->proc){
		kill(tgt->proc, SIGKILL);
		while (-1 == waitpid(tgt->proc, NULL, 0) && errno == EINTR){}
		tgt->proc = 0;
	}

	tgt->broken = false;
	if (tgt->out){
		munmap((void*)tgt->out, tgt->buf_lim);
		tgt->out = NULL;
		tgt->buf_lim = 0;
	}
}