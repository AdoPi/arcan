-- benchmark_timestamp
-- @short: Sample a timekeeping source.
-- @inargs: number:stratum=0
-- @outargs: number:timestamp
-- @longdescr: This function queries some system specific timestamp source
-- for a value usable for tracing and benchmarking purposes. The accuracy
-- and precision may vary depending on if benchmark trace data collection is
-- enabled or not, as well as the security level of the application running.
-- The optional *stratum* argument specifies what kind of timekeeping source
-- should be used, where the default value (0) will be some kind of monotonic
-- system clock in millisecond resolution.
-- Other defined stratum values are:
--  1: seconds since epoch (1970-01-01), non-monotonic.
-- -1: microseconds in system clock.
-- Selecting an invalid stratum is a terminal state transition.
-- @note: As the name implies, this is primarily intended for benchmarking
-- purposes. Real-world timekeeping cases should be avoided if possible as
-- the API does not contain sufficient functions for handling the usecases
-- that appear when dealing with locales, adjustments etc.
-- @group: system
-- @cfunction: timestamp

