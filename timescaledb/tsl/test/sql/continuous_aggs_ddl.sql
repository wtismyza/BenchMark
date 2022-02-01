-- This file and its contents are licensed under the Timescale License.
-- Please see the included NOTICE for copyright information and
-- LICENSE-TIMESCALE for a copy of the license.

-- Set this variable to avoid using a hard-coded path each time query
-- results are compared
\set QUERY_RESULT_TEST_EQUAL_RELPATH '../../../test/sql/include/query_result_test_equal.sql'

\set ON_ERROR_STOP 0

--DDL commands on continuous aggregates

CREATE TABLE conditions (
      timec        TIMESTAMPTZ       NOT NULL,
      location    TEXT              NOT NULL,
      temperature integer  NULL,
      humidity    DOUBLE PRECISION  NULL,
      timemeasure TIMESTAMPTZ,
      timeinterval INTERVAL
);

select table_name from create_hypertable('conditions', 'timec');

-- schema tests

\c :TEST_DBNAME :ROLE_SUPERUSER
CREATE TABLESPACE tablespace1 OWNER :ROLE_DEFAULT_PERM_USER LOCATION :TEST_TABLESPACE1_PATH;
CREATE TABLESPACE tablespace2 OWNER :ROLE_DEFAULT_PERM_USER LOCATION :TEST_TABLESPACE2_PATH;

CREATE SCHEMA rename_schema;
GRANT ALL ON SCHEMA rename_schema TO :ROLE_DEFAULT_PERM_USER;

SET ROLE :ROLE_DEFAULT_PERM_USER;

CREATE TABLE foo(time TIMESTAMPTZ, data INTEGER);
SELECT create_hypertable('foo', 'time');

CREATE MATERIALIZED VIEW rename_test
  WITH ( timescaledb.continuous, timescaledb.materialized_only=true)
AS SELECT time_bucket('1week', time), COUNT(data)
    FROM foo
    GROUP BY 1 WITH NO DATA;

SELECT user_view_schema, user_view_name, partial_view_schema, partial_view_name
      FROM _timescaledb_catalog.continuous_agg;

ALTER MATERIALIZED VIEW rename_test SET SCHEMA rename_schema;

SELECT user_view_schema, user_view_name, partial_view_schema, partial_view_name
      FROM _timescaledb_catalog.continuous_agg;

SELECT ca.raw_hypertable_id as "RAW_HYPERTABLE_ID",
       h.schema_name AS "MAT_SCHEMA_NAME",
       h.table_name AS "MAT_TABLE_NAME",
       partial_view_name as "PART_VIEW_NAME",
       partial_view_schema as "PART_VIEW_SCHEMA",
       direct_view_name as "DIR_VIEW_NAME",
       direct_view_schema as "DIR_VIEW_SCHEMA"
FROM _timescaledb_catalog.continuous_agg ca
INNER JOIN _timescaledb_catalog.hypertable h ON(h.id = ca.mat_hypertable_id)
WHERE user_view_name = 'rename_test'
\gset

RESET ROLE;
SELECT current_user;

ALTER VIEW :"PART_VIEW_SCHEMA".:"PART_VIEW_NAME" SET SCHEMA public;
SET ROLE :ROLE_DEFAULT_PERM_USER;

SELECT user_view_schema, user_view_name, partial_view_schema, partial_view_name
      FROM _timescaledb_catalog.continuous_agg;

--alter direct view schema
SELECT user_view_schema, user_view_name, direct_view_schema, direct_view_name
      FROM _timescaledb_catalog.continuous_agg;

RESET ROLE;
SELECT current_user;
ALTER VIEW :"DIR_VIEW_SCHEMA".:"DIR_VIEW_NAME" SET SCHEMA public;
SET ROLE :ROLE_DEFAULT_PERM_USER;

SELECT user_view_schema, user_view_name, partial_view_schema, partial_view_name,
      direct_view_schema, direct_view_name
      FROM _timescaledb_catalog.continuous_agg;

RESET ROLE;
SELECT current_user;
ALTER SCHEMA rename_schema RENAME TO new_name_schema;
SET ROLE :ROLE_DEFAULT_PERM_USER;

SELECT user_view_schema, user_view_name, partial_view_schema, partial_view_name
      FROM _timescaledb_catalog.continuous_agg;

ALTER VIEW :"PART_VIEW_NAME" SET SCHEMA new_name_schema;

SELECT user_view_schema, user_view_name, partial_view_schema, partial_view_name
      FROM _timescaledb_catalog.continuous_agg;

RESET ROLE;
SELECT current_user;
ALTER SCHEMA new_name_schema RENAME TO foo_name_schema;
SET ROLE :ROLE_DEFAULT_PERM_USER;

SELECT user_view_schema, user_view_name, partial_view_schema, partial_view_name
      FROM _timescaledb_catalog.continuous_agg;

ALTER MATERIALIZED VIEW foo_name_schema.rename_test SET SCHEMA public;

SELECT user_view_schema, user_view_name, partial_view_schema, partial_view_name
      FROM _timescaledb_catalog.continuous_agg;

RESET ROLE;
SELECT current_user;
ALTER SCHEMA foo_name_schema RENAME TO rename_schema;
SET ROLE :ROLE_DEFAULT_PERM_USER;

SET client_min_messages TO LOG;

SELECT user_view_schema, user_view_name, partial_view_schema, partial_view_name
      FROM _timescaledb_catalog.continuous_agg;

ALTER MATERIALIZED VIEW rename_test RENAME TO rename_c_aggregate;

SELECT user_view_schema, user_view_name, partial_view_schema, partial_view_name
      FROM _timescaledb_catalog.continuous_agg;

SELECT * FROM rename_c_aggregate;

ALTER VIEW rename_schema.:"PART_VIEW_NAME" RENAME TO partial_view;

SELECT user_view_schema, user_view_name, partial_view_schema, partial_view_name,
      direct_view_schema, direct_view_name
      FROM _timescaledb_catalog.continuous_agg;

--rename direct view
ALTER VIEW :"DIR_VIEW_NAME" RENAME TO direct_view;
SELECT user_view_schema, user_view_name, partial_view_schema, partial_view_name,
      direct_view_schema, direct_view_name
      FROM _timescaledb_catalog.continuous_agg;

-- drop_chunks tests
DROP TABLE conditions CASCADE;
DROP TABLE foo CASCADE;

CREATE TABLE drop_chunks_table(time BIGINT, data INTEGER);
SELECT hypertable_id AS drop_chunks_table_id
    FROM create_hypertable('drop_chunks_table', 'time', chunk_time_interval => 10) \gset

CREATE OR REPLACE FUNCTION integer_now_test() returns bigint LANGUAGE SQL STABLE as $$ SELECT coalesce(max(time), bigint '0') FROM drop_chunks_table $$;
SELECT set_integer_now_func('drop_chunks_table', 'integer_now_test');

CREATE MATERIALIZED VIEW drop_chunks_view
  WITH (
    timescaledb.continuous,
    timescaledb.materialized_only=true
  )
AS SELECT time_bucket('5', time), COUNT(data)
    FROM drop_chunks_table
    GROUP BY 1 WITH NO DATA;

SELECT format('%I.%I', schema_name, table_name) AS drop_chunks_mat_table,
        schema_name AS drop_chunks_mat_schema,
        table_name AS drop_chunks_mat_table_name
    FROM _timescaledb_catalog.hypertable, _timescaledb_catalog.continuous_agg
    WHERE _timescaledb_catalog.continuous_agg.raw_hypertable_id = :drop_chunks_table_id
        AND _timescaledb_catalog.hypertable.id = _timescaledb_catalog.continuous_agg.mat_hypertable_id \gset

-- create 3 chunks, with 3 time bucket
INSERT INTO drop_chunks_table SELECT i, i FROM generate_series(0, 29) AS i;
-- Only refresh up to bucket 15 initially. Matches the old refresh
-- behavior that didn't materialize everything
CALL refresh_continuous_aggregate('drop_chunks_view', 0, 15);
SELECT count(c) FROM show_chunks('drop_chunks_table') AS c;
SELECT count(c) FROM show_chunks('drop_chunks_view') AS c;

SELECT * FROM drop_chunks_view ORDER BY 1;

-- cannot drop directly from the materialization table without specifying
-- cont. aggregate view name explicitly

\set ON_ERROR_STOP 0
SELECT drop_chunks(:'drop_chunks_mat_table',
    newer_than => -20,
    verbose => true);
\set ON_ERROR_STOP 1

SELECT count(c) FROM show_chunks('drop_chunks_table') AS c;
SELECT count(c) FROM show_chunks('drop_chunks_view') AS c;

SELECT * FROM drop_chunks_view ORDER BY 1;

-- drop chunks when the chunksize and time_bucket aren't aligned
DROP TABLE drop_chunks_table CASCADE;
CREATE TABLE drop_chunks_table_u(time BIGINT, data INTEGER);
SELECT hypertable_id AS drop_chunks_table_u_id
    FROM create_hypertable('drop_chunks_table_u', 'time', chunk_time_interval => 7) \gset

CREATE OR REPLACE FUNCTION integer_now_test1() returns bigint LANGUAGE SQL STABLE as $$ SELECT coalesce(max(time), bigint '0') FROM drop_chunks_table_u $$;
SELECT set_integer_now_func('drop_chunks_table_u', 'integer_now_test1');

CREATE MATERIALIZED VIEW drop_chunks_view
  WITH (
    timescaledb.continuous,
    timescaledb.materialized_only=true
  )
AS SELECT time_bucket('3', time), COUNT(data)
    FROM drop_chunks_table_u
    GROUP BY 1 WITH NO DATA;

SELECT format('%I.%I', schema_name, table_name) AS drop_chunks_mat_table_u,
        schema_name AS drop_chunks_mat_schema,
        table_name AS drop_chunks_mat_table_u_name
    FROM _timescaledb_catalog.hypertable, _timescaledb_catalog.continuous_agg
    WHERE _timescaledb_catalog.continuous_agg.raw_hypertable_id = :drop_chunks_table_u_id
        AND _timescaledb_catalog.hypertable.id = _timescaledb_catalog.continuous_agg.mat_hypertable_id \gset

-- create 3 chunks, with 3 time bucket
INSERT INTO drop_chunks_table_u SELECT i, i FROM generate_series(0, 21) AS i;
-- Refresh up to bucket 15 to match old materializer behavior
CALL refresh_continuous_aggregate('drop_chunks_view', 0, 15);
SELECT count(c) FROM show_chunks('drop_chunks_table_u') AS c;
SELECT count(c) FROM show_chunks('drop_chunks_view') AS c;

SELECT * FROM drop_chunks_view ORDER BY 1;

-- TRUNCATE test
-- Can truncate regular hypertables that have caggs
TRUNCATE drop_chunks_table_u;
\set ON_ERROR_STOP 0
-- Can't truncate materialized hypertables directly
TRUNCATE :drop_chunks_mat_table_u;
\set ON_ERROR_STOP 1

-- Check that we don't interfere with TRUNCATE of normal table and
-- partitioned table
CREATE TABLE truncate (value int);
INSERT INTO truncate VALUES (1), (2);
TRUNCATE truncate;
SELECT * FROM truncate;
CREATE TABLE truncate_partitioned (value int)
  PARTITION BY RANGE(value);
CREATE TABLE truncate_p1 PARTITION OF truncate_partitioned
  FOR VALUES FROM (1) TO (3);
INSERT INTO truncate_partitioned VALUES (1), (2);
TRUNCATE truncate_partitioned;
SELECT * FROM truncate_partitioned;

-- ALTER TABLE tests
\set ON_ERROR_STOP 0
-- test a variety of ALTER TABLE statements
ALTER TABLE :drop_chunks_mat_table_u RENAME chunk_id TO bad_name;
ALTER TABLE :drop_chunks_mat_table_u ADD UNIQUE(chunk_id);
ALTER TABLE :drop_chunks_mat_table_u SET UNLOGGED;
ALTER TABLE :drop_chunks_mat_table_u ENABLE ROW LEVEL SECURITY;
ALTER TABLE :drop_chunks_mat_table_u ADD COLUMN fizzle INTEGER;
ALTER TABLE :drop_chunks_mat_table_u DROP COLUMN chunk_id;
ALTER TABLE :drop_chunks_mat_table_u ALTER COLUMN chunk_id DROP NOT NULL;
ALTER TABLE :drop_chunks_mat_table_u ALTER COLUMN chunk_id SET DEFAULT 1;
ALTER TABLE :drop_chunks_mat_table_u ALTER COLUMN chunk_id SET STORAGE EXTERNAL;
ALTER TABLE :drop_chunks_mat_table_u DISABLE TRIGGER ALL;
ALTER TABLE :drop_chunks_mat_table_u SET TABLESPACE foo;
ALTER TABLE :drop_chunks_mat_table_u NOT OF;
ALTER TABLE :drop_chunks_mat_table_u OWNER TO CURRENT_USER;
\set ON_ERROR_STOP 1

ALTER TABLE :drop_chunks_mat_table_u SET SCHEMA public;
ALTER TABLE :drop_chunks_mat_table_u_name RENAME TO new_name;

SET ROLE :ROLE_DEFAULT_PERM_USER;
SET client_min_messages TO LOG;

CREATE INDEX new_name_idx ON new_name(chunk_id);

SELECT * FROM new_name;

SELECT * FROM drop_chunks_view ORDER BY 1;

\set ON_ERROR_STOP 0

-- no continuous aggregates on a continuous aggregate materialization table
CREATE MATERIALIZED VIEW new_name_view
  WITH (
    timescaledb.continuous,
    timescaledb.materialized_only=true
  )
AS SELECT time_bucket('6', time_bucket), COUNT(agg_2_2)
    FROM new_name
    GROUP BY 1 WITH NO DATA;

-- cannot create a continuous aggregate on a continuous aggregate view
CREATE MATERIALIZED VIEW drop_chunks_view_view
  WITH (
    timescaledb.continuous,
    timescaledb.materialized_only=true
  )
AS SELECT time_bucket('6', time_bucket), SUM(count)
    FROM drop_chunks_view
    GROUP BY 1 WITH NO DATA;
\set ON_ERROR_STOP 1

DROP INDEX new_name_idx;

CREATE TABLE metrics(time timestamptz, device_id int, v1 float, v2 float);
SELECT create_hypertable('metrics','time');

INSERT INTO metrics SELECT generate_series('2000-01-01'::timestamptz,'2000-01-10','1m'),1,0.25,0.75;

-- check expressions in view definition
CREATE MATERIALIZED VIEW cagg_expr
  WITH (timescaledb.continuous, timescaledb.materialized_only=true)
AS
SELECT
  time_bucket('1d', time) AS time,
  'Const'::text AS Const,
  4.3::numeric AS "numeric",
  first(metrics,time),
  CASE WHEN true THEN 'foo' ELSE 'bar' END,
  COALESCE(NULL,'coalesce'),
  avg(v1) + avg(v2) AS avg1,
  avg(v1+v2) AS avg2
FROM metrics
GROUP BY 1 WITH NO DATA;

CALL refresh_continuous_aggregate('cagg_expr', NULL, NULL);
SELECT * FROM cagg_expr ORDER BY time LIMIT 5;



--test materialization of invalidation before drop
DROP TABLE IF EXISTS drop_chunks_table CASCADE;
DROP TABLE IF EXISTS drop_chunks_table_u CASCADE;
CREATE TABLE drop_chunks_table(time BIGINT, data INTEGER);
SELECT hypertable_id AS drop_chunks_table_nid
    FROM create_hypertable('drop_chunks_table', 'time', chunk_time_interval => 10) \gset

CREATE OR REPLACE FUNCTION integer_now_test2() returns bigint LANGUAGE SQL STABLE as $$ SELECT coalesce(max(time), bigint '0') FROM drop_chunks_table $$;
SELECT set_integer_now_func('drop_chunks_table', 'integer_now_test2');

CREATE MATERIALIZED VIEW drop_chunks_view
  WITH (
    timescaledb.continuous,
    timescaledb.materialized_only=true
  )
AS SELECT time_bucket('5', time), max(data)
    FROM drop_chunks_table
    GROUP BY 1 WITH NO DATA;

INSERT INTO drop_chunks_table SELECT i, i FROM generate_series(0, 20) AS i;
--dropping chunks will process the invalidations
SELECT drop_chunks('drop_chunks_table', older_than => (integer_now_test2()-9));
SELECT * FROM drop_chunks_table ORDER BY time ASC limit 1;

INSERT INTO drop_chunks_table SELECT i, i FROM generate_series(20, 35) AS i;
CALL refresh_continuous_aggregate('drop_chunks_view', 10, 40);

--this will be seen after the drop its within the invalidation window and will be dropped
INSERT INTO drop_chunks_table VALUES (26, 100);
--this will not be processed by the drop since chunk 30-39 is not dropped but will be seen after refresh
--shows that the drop doesn't do more work than necessary
INSERT INTO drop_chunks_table VALUES (31, 200);
--move the time up to 39
INSERT INTO drop_chunks_table SELECT i, i FROM generate_series(35, 39) AS i;

--the chunks and ranges we have thus far
SELECT chunk_name, range_start_integer, range_end_integer
FROM timescaledb_information.chunks
WHERE hypertable_name = 'drop_chunks_table';

--the invalidation on 25 not yet seen
SELECT * FROM drop_chunks_view ORDER BY time_bucket DESC;

--dropping tables will cause the invalidation to be processed
SELECT drop_chunks('drop_chunks_table', older_than => (integer_now_test2()-9));

--new values on 25 now seen in view
SELECT * FROM drop_chunks_view ORDER BY time_bucket DESC;
--earliest datapoint now in table
SELECT * FROM drop_chunks_table ORDER BY time ASC limit 1;
--we see the chunks row with the dropped flags set;
SELECT * FROM _timescaledb_catalog.chunk where dropped;
--still see data in the view
SELECT * FROM drop_chunks_view WHERE time_bucket < (integer_now_test2()-9) ORDER BY time_bucket DESC;
--no data but covers dropped chunks
SELECT * FROM drop_chunks_table WHERE time < (integer_now_test2()-9) ORDER BY time DESC;
--recreate the dropped chunk
INSERT INTO drop_chunks_table SELECT i, i FROM generate_series(0, 20) AS i;
--see data from recreated region
SELECT * FROM drop_chunks_table WHERE time < (integer_now_test2()-9) ORDER BY time DESC;

--should show chunk with old name and old ranges
SELECT chunk_name, range_start_integer, range_end_integer
FROM timescaledb_information.chunks
WHERE hypertable_name = 'drop_chunks_table'
ORDER BY range_start_integer;

--We dropped everything up to the bucket starting at 30 and then
--inserted new data up to and including time 20. Therefore, the
--dropped data should stay the same as long as we only refresh
--buckets that have non-dropped data.
CALL refresh_continuous_aggregate('drop_chunks_view', 30, 40);
SELECT * FROM drop_chunks_view ORDER BY time_bucket DESC;


SELECT format('%I.%I', schema_name, table_name) AS drop_chunks_mat_tablen,
        schema_name AS drop_chunks_mat_schema,
        table_name AS drop_chunks_mat_table_name
    FROM _timescaledb_catalog.hypertable, _timescaledb_catalog.continuous_agg
    WHERE _timescaledb_catalog.continuous_agg.raw_hypertable_id = :drop_chunks_table_nid
        AND _timescaledb_catalog.hypertable.id = _timescaledb_catalog.continuous_agg.mat_hypertable_id \gset

-- TEST drop chunks from continuous aggregates by specifying view name
SELECT drop_chunks('drop_chunks_view',
    newer_than => -20,
    verbose => true);

-- Test that we cannot drop chunks when specifying materialized
-- hypertable
INSERT INTO drop_chunks_table SELECT generate_series(45, 55), 500;
CALL refresh_continuous_aggregate('drop_chunks_view', 45, 55);

SELECT chunk_name, range_start_integer, range_end_integer
FROM timescaledb_information.chunks
WHERE hypertable_name = :'drop_chunks_mat_table_name' ORDER BY range_start_integer;
\set ON_ERROR_STOP 0
\set VERBOSITY default
SELECT drop_chunks(:'drop_chunks_mat_tablen', older_than => 60);
\set VERBOSITY terse
\set ON_ERROR_STOP 1

-----------------------------------------------------------------
-- Test that drop_chunks will "refresh-on-drop", but only in the
-- regions covered by the dropped chunks.
-----------------------------------------------------------------
SELECT chunk_name, range_start_integer, range_end_integer
FROM timescaledb_information.chunks
WHERE hypertable_name = 'drop_chunks_table'
ORDER BY 2,3;

-- Pick the second chunk as the one to drop
WITH numbered_chunks AS (
     SELECT row_number() OVER (ORDER BY range_start_integer), chunk_schema, chunk_name, range_start_integer, range_end_integer
     FROM timescaledb_information.chunks
     WHERE hypertable_name = 'drop_chunks_table'
     ORDER BY 1
)
SELECT format('%I.%I', chunk_schema, chunk_name)::regclass AS chunk_to_drop, range_start_integer, range_end_integer
FROM numbered_chunks
WHERE row_number = 2 \gset

-- There's data in the table for the chunk/range we will drop
SELECT * FROM drop_chunks_table
WHERE time >= :range_start_integer
AND time < :range_end_integer
ORDER BY 1;

-- Make sure there is also data in the continuous aggregate
CALL refresh_continuous_aggregate('drop_chunks_view', 0, 50);

SELECT * FROM drop_chunks_view
ORDER BY 1;

-- Drop the second chunk, to leave a gap in the data
DROP TABLE :chunk_to_drop;

-- Verify that the second chunk is dropped
SELECT chunk_name, range_start_integer, range_end_integer
FROM timescaledb_information.chunks
WHERE hypertable_name = 'drop_chunks_table'
ORDER BY 2,3;

-- Data is no longer in the table but still in the view
SELECT * FROM drop_chunks_table
WHERE time >= :range_start_integer
AND time < :range_end_integer
ORDER BY 1;

SELECT * FROM drop_chunks_view
WHERE time_bucket >= :range_start_integer
AND time_bucket < :range_end_integer
ORDER BY 1;

-- Insert a large value in one of the chunks that will be dropped
INSERT INTO drop_chunks_table VALUES (:range_start_integer-1, 100);
-- Now drop the two adjecent chunks
SELECT drop_chunks('drop_chunks_table', older_than=>30);

-- Verify that the chunks are dropped
SELECT chunk_name, range_start_integer, range_end_integer
FROM timescaledb_information.chunks
WHERE hypertable_name = 'drop_chunks_table'
ORDER BY 2,3;

-- The continuous aggregate should be refreshed in the regions covered
-- by the dropped chunks, but not in the "gap" region, i.e., the
-- region of the chunk that was dropped via DROP TABLE.
SELECT * FROM drop_chunks_view
ORDER BY 1;

-- Now refresh in the region of the first two dropped chunks
CALL refresh_continuous_aggregate('drop_chunks_view', 0, :range_end_integer);

-- Aggregate data in the refreshed range should no longer exist since
-- the underlying data was dropped.
SELECT * FROM drop_chunks_view
ORDER BY 1;

--------------------------------------------------------------------
-- Check that we can create a materialized table in a tablespace. We
-- create one with tablespace and one without and compare them.

CREATE VIEW cagg_info AS
WITH
  caggs AS (
    SELECT format('%I.%I', user_view_schema, user_view_name)::regclass AS user_view,
           format('%I.%I', direct_view_schema, direct_view_name)::regclass AS direct_view,
           format('%I.%I', partial_view_schema, partial_view_name)::regclass AS partial_view,
           format('%I.%I', ht.schema_name, ht.table_name)::regclass AS mat_relid
      FROM _timescaledb_catalog.hypertable ht,
           _timescaledb_catalog.continuous_agg cagg
     WHERE ht.id = cagg.mat_hypertable_id
  )
SELECT user_view,
       pg_get_userbyid(relowner) AS user_view_owner,
       relname AS mat_table,
       (SELECT pg_get_userbyid(relowner) FROM pg_class WHERE oid = mat_relid) AS mat_table_owner,
       direct_view,
       (SELECT pg_get_userbyid(relowner) FROM pg_class WHERE oid = direct_view) AS direct_view_owner,
       partial_view,
       (SELECT pg_get_userbyid(relowner) FROM pg_class WHERE oid = partial_view) AS partial_view_owner,
       (SELECT spcname FROM pg_tablespace WHERE oid = reltablespace) AS tablespace
  FROM pg_class JOIN caggs ON pg_class.oid = caggs.mat_relid;
GRANT SELECT ON cagg_info TO PUBLIC;

CREATE VIEW chunk_info AS
SELECT ht.schema_name, ht.table_name, relname AS chunk_name,
       (SELECT spcname FROM pg_tablespace WHERE oid = reltablespace) AS tablespace
  FROM pg_class c,
       _timescaledb_catalog.hypertable ht,
       _timescaledb_catalog.chunk ch
 WHERE ch.table_name = c.relname AND ht.id = ch.hypertable_id;

CREATE TABLE whatever(time BIGINT NOT NULL, data INTEGER);

SELECT hypertable_id AS whatever_nid
  FROM create_hypertable('whatever', 'time', chunk_time_interval => 10)
\gset

SELECT set_integer_now_func('whatever', 'integer_now_test');

CREATE MATERIALIZED VIEW whatever_view_1
WITH (timescaledb.continuous, timescaledb.materialized_only=true) AS
SELECT time_bucket('5', time), COUNT(data)
  FROM whatever GROUP BY 1 WITH NO DATA;

CREATE MATERIALIZED VIEW whatever_view_2
WITH (timescaledb.continuous, timescaledb.materialized_only=true)
TABLESPACE tablespace1 AS
SELECT time_bucket('5', time), COUNT(data)
  FROM whatever GROUP BY 1 WITH NO DATA;

INSERT INTO whatever SELECT i, i FROM generate_series(0, 29) AS i;
CALL refresh_continuous_aggregate('whatever_view_1', NULL, NULL);
CALL refresh_continuous_aggregate('whatever_view_2', NULL, NULL);

SELECT user_view,
       mat_table,
       cagg_info.tablespace AS mat_tablespace,
       chunk_name,
       chunk_info.tablespace AS chunk_tablespace
  FROM cagg_info, chunk_info
 WHERE mat_table::text = table_name
   AND user_view::text LIKE 'whatever_view%';

ALTER MATERIALIZED VIEW whatever_view_1 SET TABLESPACE tablespace2;

SELECT user_view,
       mat_table,
       cagg_info.tablespace AS mat_tablespace,
       chunk_name,
       chunk_info.tablespace AS chunk_tablespace
  FROM cagg_info, chunk_info
 WHERE mat_table::text = table_name
   AND user_view::text LIKE 'whatever_view%';

DROP MATERIALIZED VIEW whatever_view_1;
DROP MATERIALIZED VIEW whatever_view_2;

-- test bucket width expressions on integer hypertables
CREATE TABLE metrics_int2 (
  time int2 NOT NULL,
  device_id int,
  v1 float,
  v2 float
);

CREATE TABLE metrics_int4 (
  time int4 NOT NULL,
  device_id int,
  v1 float,
  v2 float
);

CREATE TABLE metrics_int8 (
  time int8 NOT NULL,
  device_id int,
  v1 float,
  v2 float
);

SELECT create_hypertable (('metrics_' || dt)::regclass, 'time', chunk_time_interval => 10)
FROM (
  VALUES ('int2'),
    ('int4'),
    ('int8')) v (dt);

CREATE OR REPLACE FUNCTION int2_now ()
  RETURNS int2
  LANGUAGE SQL
  STABLE
  AS $$
  SELECT 10::int2
$$;

CREATE OR REPLACE FUNCTION int4_now ()
  RETURNS int4
  LANGUAGE SQL
  STABLE
  AS $$
  SELECT 10::int4
$$;

CREATE OR REPLACE FUNCTION int8_now ()
  RETURNS int8
  LANGUAGE SQL
  STABLE
  AS $$
  SELECT 10::int8
$$;

SELECT set_integer_now_func (('metrics_' || dt)::regclass, (dt || '_now')::regproc)
FROM (
  VALUES ('int2'),
    ('int4'),
    ('int8')) v (dt);

-- width expression for int2 hypertables
CREATE MATERIALIZED VIEW width_expr WITH (timescaledb.continuous) AS
SELECT time_bucket(1::smallint, time)
FROM metrics_int2
GROUP BY 1;

DROP MATERIALIZED VIEW width_expr;

CREATE MATERIALIZED VIEW width_expr WITH (timescaledb.continuous) AS
SELECT time_bucket(1::smallint + 2::smallint, time)
FROM metrics_int2
GROUP BY 1;

DROP MATERIALIZED VIEW width_expr;

-- width expression for int4 hypertables
CREATE MATERIALIZED VIEW width_expr WITH (timescaledb.continuous) AS
SELECT time_bucket(1, time)
FROM metrics_int4
GROUP BY 1;

DROP MATERIALIZED VIEW width_expr;

CREATE MATERIALIZED VIEW width_expr WITH (timescaledb.continuous) AS
SELECT time_bucket(1 + 2, time)
FROM metrics_int4
GROUP BY 1;

DROP MATERIALIZED VIEW width_expr;

-- width expression for int8 hypertables
CREATE MATERIALIZED VIEW width_expr WITH (timescaledb.continuous) AS
SELECT time_bucket(1, time)
FROM metrics_int8
GROUP BY 1;

DROP MATERIALIZED VIEW width_expr;

CREATE MATERIALIZED VIEW width_expr WITH (timescaledb.continuous) AS
SELECT time_bucket(1 + 2, time)
FROM metrics_int8
GROUP BY 1;

DROP MATERIALIZED VIEW width_expr;

\set ON_ERROR_STOP 0
-- non-immutable expresions should be rejected
CREATE MATERIALIZED VIEW width_expr WITH (timescaledb.continuous) AS
SELECT time_bucket(extract(year FROM now())::smallint, time)
FROM metrics_int2
GROUP BY 1;

CREATE MATERIALIZED VIEW width_expr WITH (timescaledb.continuous) AS
SELECT time_bucket(extract(year FROM now())::int, time)
FROM metrics_int4
GROUP BY 1;

CREATE MATERIALIZED VIEW width_expr WITH (timescaledb.continuous) AS
SELECT time_bucket(extract(year FROM now())::int, time)
FROM metrics_int8
GROUP BY 1;
\set ON_ERROR_STOP 1

-- Test various ALTER MATERIALIZED VIEW statements.

SET ROLE :ROLE_DEFAULT_PERM_USER;

CREATE MATERIALIZED VIEW owner_check WITH (timescaledb.continuous) AS
SELECT time_bucket(1 + 2, time)
FROM metrics_int8
GROUP BY 1
WITH NO DATA;

\x on
SELECT * FROM cagg_info WHERE user_view::text = 'owner_check';
\x off

-- This should not work since the target user has the wrong role, but
-- we test that the normal checks are done when changing the owner.
\set ON_ERROR_STOP 0
ALTER MATERIALIZED VIEW owner_check OWNER TO :ROLE_1;
\set ON_ERROR_STOP 1

-- Superuser can always change owner 
SET ROLE :ROLE_SUPERUSER;
ALTER MATERIALIZED VIEW owner_check OWNER TO :ROLE_1;

\x on
SELECT * FROM cagg_info WHERE user_view::text = 'owner_check';
\x off

DROP TABLESPACE tablespace1;
DROP TABLESPACE tablespace2;
