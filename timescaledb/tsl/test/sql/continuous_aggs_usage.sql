-- This file and its contents are licensed under the Timescale License.
-- Please see the included NOTICE for copyright information and
-- LICENSE-TIMESCALE for a copy of the license.

-- TEST SETUP --
\set ON_ERROR_STOP 0
SET client_min_messages TO LOG;

-- START OF USAGE TEST --

--First create your hypertable
CREATE TABLE device_readings (
      observation_time  TIMESTAMPTZ       NOT NULL,
      device_id         TEXT              NOT NULL,
      metric            DOUBLE PRECISION  NOT NULL,
      PRIMARY KEY(observation_time, device_id)
);
SELECT table_name FROM create_hypertable('device_readings', 'observation_time');

--Next, create your continuous aggregate view
CREATE MATERIALIZED VIEW device_summary
WITH (timescaledb.continuous, timescaledb.materialized_only=true) --This flag is what makes the view continuous
AS
SELECT
  time_bucket('1 hour', observation_time) as bucket, --time_bucket is required
  device_id,
  avg(metric) as metric_avg, --We can use regular aggregates
  max(metric)-min(metric) as metric_spread --We can also use expressions on aggregates and constants
FROM
  device_readings
GROUP BY bucket, device_id WITH NO DATA; --We have to group by the bucket column, but can also add other group-by columns

SELECT add_continuous_aggregate_policy('device_summary', NULL, '2 h'::interval, '2 h'::interval);
--Next, insert some data into the raw hypertable
INSERT INTO device_readings
SELECT ts, 'device_1', (EXTRACT(EPOCH FROM ts)) from generate_series('2018-12-01 00:00'::timestamp, '2018-12-31 00:00'::timestamp, '30 minutes') ts;
INSERT INTO device_readings
SELECT ts, 'device_2', (EXTRACT(EPOCH FROM ts)) from generate_series('2018-12-01 00:00'::timestamp, '2018-12-31 00:00'::timestamp, '30 minutes') ts;

--Initially, it will be empty.
SELECT * FROM device_summary;

-- Simulate a policy that refreshes with lag, i.e., it doesn't refresh
-- the entire data set. In this case up to the given date.
CALL refresh_continuous_aggregate('device_summary', NULL, '2018-12-30 22:00');

--Now you can run selects over your view as normal
SELECT * FROM device_summary WHERE metric_spread = 1800 ORDER BY bucket DESC, device_id LIMIT 10;

--You can view informaton about your continuous aggregates. The meaning of these fields will be explained further down.
\x
SELECT * FROM timescaledb_information.continuous_aggregates;

\x

-- Refresh interval
--
-- The refresh interval determines how often the background worker
-- for automatic materialization will run. The default is (2 x bucket_width)
SELECT schedule_interval FROM _timescaledb_config.bgw_job WHERE id = 1000;

-- You can change this setting with ALTER VIEW (equivalently, specify in WITH clause of CREATE VIEW)
SELECT alter_job(1000, schedule_interval := '1h');

SELECT schedule_interval FROM _timescaledb_config.bgw_job WHERE id = 1000;

--
-- Refresh with lag
--
-- It is possible to use a policy or manual refresh with a lag, which
-- means the materialization will not contain the most up-to-date
-- data.
SELECT max(observation_time) FROM device_readings;
SELECT max(bucket) FROM device_summary;
CALL refresh_continuous_aggregate('device_summary', NULL, '2018-12-31 01:00');
SELECT max(observation_time) FROM device_readings;
SELECT max(bucket) FROM device_summary;

--
-- Invalidations
--

--Changes to the raw table, for values that have already been materialized are propagated asynchronously, after the materialization next runs.
--Before update:
SELECT * FROM device_summary WHERE device_id = 'device_1' and bucket = 'Sun Dec 30 13:00:00 2018 PST';

INSERT INTO device_readings VALUES ('Sun Dec 30 13:01:00 2018 PST', 'device_1', 1.0);

--Change not reflected before materializer runs.
SELECT * FROM device_summary WHERE device_id = 'device_1' and bucket = 'Sun Dec 30 13:00:00 2018 PST';
CALL refresh_continuous_aggregate('device_summary', NULL, NULL);
--But is reflected after.
SELECT * FROM device_summary WHERE device_id = 'device_1' and bucket = 'Sun Dec 30 13:00:00 2018 PST';

--
-- Dealing with timezones
--

-- You cannot use any functions that depend on the local timezone setting inside a continuous aggregate.
-- For example you cannot cast to the local time. This is because
-- a timezone setting can alter from user-to-user and thus
-- cannot be materialized.

DROP MATERIALIZED VIEW device_summary;
CREATE MATERIALIZED VIEW device_summary
WITH (timescaledb.continuous, timescaledb.materialized_only=true)
AS
SELECT
  time_bucket('1 hour', observation_time) as bucket,
  min(observation_time::timestamp) as min_time, --note the cast to localtime
  device_id,
  avg(metric) as metric_avg,
  max(metric)-min(metric) as metric_spread
FROM
  device_readings
GROUP BY bucket, device_id WITH NO DATA;
--note the error.

-- You have two options:
-- Option 1: be explicit in your timezone:

DROP MATERIALIZED VIEW device_summary;
CREATE MATERIALIZED VIEW device_summary
WITH (timescaledb.continuous, timescaledb.materialized_only=true)
AS
SELECT
  time_bucket('1 hour', observation_time) as bucket,
  min(observation_time AT TIME ZONE 'EST') as min_time, --note the explict timezone
  device_id,
  avg(metric) as metric_avg,
  max(metric)-min(metric) as metric_spread
FROM
  device_readings
GROUP BY bucket, device_id WITH NO DATA;
DROP MATERIALIZED VIEW device_summary;

-- Option 2: Keep things as TIMESTAMPTZ in the view and convert to local time when
-- querying from the view

DROP MATERIALIZED VIEW device_summary;
CREATE MATERIALIZED VIEW device_summary
WITH (timescaledb.continuous, timescaledb.materialized_only=true)
AS
SELECT
  time_bucket('1 hour', observation_time) as bucket,
  min(observation_time) as min_time, --this is a TIMESTAMPTZ
  device_id,
  avg(metric) as metric_avg,
  max(metric)-min(metric) as metric_spread
FROM
  device_readings
GROUP BY bucket, device_id WITH DATA;

SELECT min(min_time)::timestamp FROM device_summary;

--
-- test just in time aggregate / materialization only view
--

-- hardcoding now to 50 will lead to 30 watermark
CREATE OR REPLACE FUNCTION device_readings_int_now()
  RETURNS INT LANGUAGE SQL STABLE AS
$BODY$
  SELECT 50;
$BODY$;

CREATE TABLE device_readings_int(time int, value float);
SELECT create_hypertable('device_readings_int','time',chunk_time_interval:=10);

SELECT set_integer_now_func('device_readings_int','device_readings_int_now');

CREATE MATERIALIZED VIEW device_readings_mat_only
  WITH (timescaledb.continuous, timescaledb.materialized_only=true)
AS
  SELECT time_bucket(10,time), avg(value) FROM device_readings_int GROUP BY 1 WITH NO DATA;

CREATE MATERIALIZED VIEW device_readings_jit
  WITH (timescaledb.continuous, timescaledb.materialized_only=false)
AS
  SELECT time_bucket(10,time), avg(value) FROM device_readings_int GROUP BY 1 WITH NO DATA;

INSERT INTO device_readings_int SELECT i, i*10 FROM generate_series(10,40,10) AS g(i);

-- materialization only should have 0 rows
SELECT * FROM device_readings_mat_only ORDER BY time_bucket;

-- jit aggregate should have 4 rows
SELECT * FROM device_readings_jit ORDER BY time_bucket;

-- simulate a refresh policy with lag, i.e., one that doesn't refresh
-- up to the latest data. Max value is 40.
CALL refresh_continuous_aggregate('device_readings_mat_only', NULL, 30);
CALL refresh_continuous_aggregate('device_readings_jit', NULL, 30);

-- materialization only should have 2 rows
SELECT * FROM device_readings_mat_only ORDER BY time_bucket;
-- jit aggregate should have 4 rows
SELECT * FROM device_readings_jit ORDER BY time_bucket;

-- add 2 more rows
INSERT INTO device_readings_int SELECT i, i*10 FROM generate_series(50,60,10) AS g(i);

-- materialization only should have 2 rows
SELECT * FROM device_readings_mat_only ORDER BY time_bucket;
-- jit aggregate should have 6 rows
SELECT * FROM device_readings_jit ORDER BY time_bucket;

-- hardcoding now to 100 will lead to 80 watermark
CREATE OR REPLACE FUNCTION device_readings_int_now()
  RETURNS INT LANGUAGE SQL STABLE AS
$BODY$
  SELECT 100;
$BODY$;

-- refresh should materialize all now
CALL refresh_continuous_aggregate('device_readings_mat_only', NULL, NULL);
CALL refresh_continuous_aggregate('device_readings_jit', NULL, NULL);

-- materialization only should have 6 rows
SELECT * FROM device_readings_mat_only ORDER BY time_bucket;
-- jit aggregate should have 6 rows
SELECT * FROM device_readings_jit ORDER BY time_bucket;

-- START OF BASIC USAGE TESTS --

-- Check that continuous aggregate and materialized table is dropped
-- together.

CREATE TABLE whatever(time TIMESTAMPTZ NOT NULL, metric INTEGER);
SELECT * FROM create_hypertable('whatever', 'time');
CREATE MATERIALIZED VIEW whatever_summary WITH (timescaledb.continuous) AS
SELECT time_bucket('1 hour', time) AS bucket, avg(metric)
  FROM whatever GROUP BY bucket WITH NO DATA;

SELECT (SELECT format('%1$I.%2$I', schema_name, table_name)::regclass::oid
          FROM _timescaledb_catalog.hypertable
     WHERE id = raw_hypertable_id) AS raw_table
     , (SELECT format('%1$I.%2$I', schema_name, table_name)::regclass::oid
          FROM _timescaledb_catalog.hypertable
     WHERE id = mat_hypertable_id) AS mat_table
FROM _timescaledb_catalog.continuous_agg
WHERE user_view_name = 'whatever_summary' \gset
SELECT relname FROM pg_class WHERE oid = :mat_table;

----------------------------------------------------------------
-- Should generate an error since the cagg is dependent on the table.
DROP TABLE whatever;

----------------------------------------------------------------
-- Checking that a cagg cannot be dropped if there is a dependent
-- object on it.
CREATE VIEW whatever_summary_dependency AS SELECT * FROM whatever_summary;

-- Should generate an error
DROP MATERIALIZED VIEW whatever_summary;

-- Dropping the dependent view so that we can do a proper drop below.
DROP VIEW whatever_summary_dependency;

----------------------------------------------------------------
-- Dropping the cagg should also remove the materialized table
DROP MATERIALIZED VIEW whatever_summary;
SELECT relname FROM pg_class WHERE oid = :mat_table;

----------------------------------------------------------------
-- Cleanup
DROP TABLE whatever;

-- END OF BASIC USAGE TESTS --
