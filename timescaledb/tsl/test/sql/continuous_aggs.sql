-- This file and its contents are licensed under the Timescale License.
-- Please see the included NOTICE for copyright information and
-- LICENSE-TIMESCALE for a copy of the license.

-- initialize the bgw mock state to prevent the materialization workers from running
\c :TEST_DBNAME :ROLE_SUPERUSER

CREATE OR REPLACE FUNCTION ts_bgw_params_create() RETURNS VOID
AS :MODULE_PATHNAME LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION test.continuous_aggs_find_view(cagg REGCLASS) RETURNS VOID
AS :TSL_MODULE_PATHNAME, 'ts_test_continuous_agg_find_by_view_name' LANGUAGE C;

\set WAIT_ON_JOB 0
\set IMMEDIATELY_SET_UNTIL 1
\set WAIT_FOR_OTHER_TO_ADVANCE 2

-- remove any default jobs, e.g., telemetry so bgw_job isn't polluted
DELETE FROM _timescaledb_config.bgw_job WHERE TRUE;

SET ROLE :ROLE_DEFAULT_PERM_USER;

SELECT * FROM _timescaledb_config.bgw_job;

--TEST1 ---
--basic test with count
create table foo (a integer, b integer, c integer);
select table_name from create_hypertable('foo', 'a', chunk_time_interval=> 10);

insert into foo values( 3 , 16 , 20);
insert into foo values( 1 , 10 , 20);
insert into foo values( 1 , 11 , 20);
insert into foo values( 1 , 12 , 20);
insert into foo values( 1 , 13 , 20);
insert into foo values( 1 , 14 , 20);
insert into foo values( 2 , 14 , 20);
insert into foo values( 2 , 15 , 20);
insert into foo values( 2 , 16 , 20);

CREATE OR REPLACE FUNCTION integer_now_foo() returns int LANGUAGE SQL STABLE as $$ SELECT coalesce(max(a), 0) FROM foo $$;
SELECT set_integer_now_func('foo', 'integer_now_foo');


CREATE MATERIALIZED VIEW mat_m1(a, countb)
WITH (timescaledb.continuous, timescaledb.materialized_only=true)
as
select a, count(b)
from foo
group by time_bucket(1, a), a WITH NO DATA;

SELECT add_continuous_aggregate_policy('mat_m1', NULL, 2::integer, '12 h'::interval);
SELECT * FROM _timescaledb_config.bgw_job;

SELECT ca.raw_hypertable_id as "RAW_HYPERTABLE_ID",
       h.schema_name AS "MAT_SCHEMA_NAME",
       h.table_name AS "MAT_TABLE_NAME",
       partial_view_name as "PART_VIEW_NAME",
       partial_view_schema as "PART_VIEW_SCHEMA"
FROM _timescaledb_catalog.continuous_agg ca
INNER JOIN _timescaledb_catalog.hypertable h ON(h.id = ca.mat_hypertable_id)
WHERE user_view_name = 'mat_m1'
\gset

insert into :"MAT_SCHEMA_NAME".:"MAT_TABLE_NAME"
select a, _timescaledb_internal.partialize_agg(count(b)),
time_bucket(1, a)
,1
from foo
group by time_bucket(1, a) , a ;

select * from mat_m1 order by a ;

--check triggers on user hypertable --
SET ROLE :ROLE_SUPERUSER;
select tgname, tgtype, tgenabled , relname from pg_trigger, pg_class
where tgrelid = pg_class.oid and pg_class.relname like 'foo'
order by tgname;

SET ROLE :ROLE_DEFAULT_PERM_USER;

-- TEST2 ---
DROP MATERIALIZED VIEW mat_m1;

SELECT * FROM _timescaledb_config.bgw_job;

CREATE TABLE conditions (
      timec        TIMESTAMPTZ       NOT NULL,
      location    TEXT              NOT NULL,
      temperature DOUBLE PRECISION  NULL,
      humidity    DOUBLE PRECISION  NULL
    );

select table_name from create_hypertable( 'conditions', 'timec');

insert into conditions values ( '2010-01-01 09:00:00-08', 'SFO', 55, 45);
insert into conditions values ( '2010-01-02 09:00:00-08', 'por', 100, 100);
insert into conditions values ( '2010-01-02 09:00:00-08', 'SFO', 65, 45);
insert into conditions values ( '2010-01-02 09:00:00-08', 'NYC', 65, 45);
insert into conditions values ( '2018-11-01 09:00:00-08', 'NYC', 45, 35);
insert into conditions values ( '2018-11-02 09:00:00-08', 'NYC', 35, 15);


CREATE MATERIALIZED VIEW mat_m1( timec, minl, sumt , sumh)
WITH (timescaledb.continuous, timescaledb.materialized_only=true)
as
select time_bucket('1day', timec), min(location), sum(temperature),sum(humidity)
from conditions
group by time_bucket('1day', timec) WITH NO DATA;

SELECT ca.raw_hypertable_id as "RAW_HYPERTABLE_ID",
       h.schema_name AS "MAT_SCHEMA_NAME",
       h.table_name AS "MAT_TABLE_NAME",
       partial_view_name as "PART_VIEW_NAME",
       partial_view_schema as "PART_VIEW_SCHEMA"
FROM _timescaledb_catalog.continuous_agg ca
INNER JOIN _timescaledb_catalog.hypertable h ON(h.id = ca.mat_hypertable_id)
WHERE user_view_name = 'mat_m1'
\gset

-- Materialized hypertable for mat_m1 should not be visible in the
-- hypertables view:
SELECT hypertable_schema, hypertable_name
FROM timescaledb_information.hypertables ORDER BY 1,2;

SET ROLE :ROLE_SUPERUSER;
insert into  :"MAT_SCHEMA_NAME".:"MAT_TABLE_NAME"
select
 time_bucket('1day', timec), _timescaledb_internal.partialize_agg( min(location)), _timescaledb_internal.partialize_agg( sum(temperature)) , _timescaledb_internal.partialize_agg( sum(humidity))
,1
from conditions
group by time_bucket('1day', timec) ;

SET ROLE :ROLE_DEFAULT_PERM_USER;
--should have same results --
select timec, minl, sumt, sumh
from mat_m1
order by timec;

select time_bucket('1day', timec), min(location), sum(temperature), sum(humidity)
from conditions
group by time_bucket('1day', timec)
order by 1;

-- TEST3 --
-- drop on table conditions should cascade to materialized mat_v1

drop table conditions cascade;

CREATE TABLE conditions (
      timec        TIMESTAMPTZ       NOT NULL,
      location    TEXT              NOT NULL,
      temperature DOUBLE PRECISION  NULL,
      humidity    DOUBLE PRECISION  NULL
    );

select table_name from create_hypertable( 'conditions', 'timec');

insert into conditions values ( '2010-01-01 09:00:00-08', 'SFO', 55, 45);
insert into conditions values ( '2010-01-02 09:00:00-08', 'por', 100, 100);
insert into conditions values ( '2010-01-02 09:00:00-08', 'NYC', 65, 45);
insert into conditions values ( '2010-01-02 09:00:00-08', 'SFO', 65, 45);
insert into conditions values ( '2010-01-03 09:00:00-08', 'NYC', 45, 55);
insert into conditions values ( '2010-01-05 09:00:00-08', 'SFO', 75, 100);
insert into conditions values ( '2018-11-01 09:00:00-08', 'NYC', 45, 35);
insert into conditions values ( '2018-11-02 09:00:00-08', 'NYC', 35, 15);
insert into conditions values ( '2018-11-03 09:00:00-08', 'NYC', 35, 25);


CREATE MATERIALIZED VIEW mat_m1( timec, minl, sumth, stddevh)
WITH (timescaledb.continuous, timescaledb.materialized_only=true)
as
select time_bucket('1week', timec) ,
min(location), sum(temperature)+sum(humidity), stddev(humidity)
from conditions
group by time_bucket('1week', timec)  WITH NO DATA;

SELECT ca.raw_hypertable_id as "RAW_HYPERTABLE_ID",
       h.schema_name AS "MAT_SCHEMA_NAME",
       h.table_name AS "MAT_TABLE_NAME",
       partial_view_name as "PART_VIEW_NAME",
       partial_view_schema as "PART_VIEW_SCHEMA"
FROM _timescaledb_catalog.continuous_agg ca
INNER JOIN _timescaledb_catalog.hypertable h ON(h.id = ca.mat_hypertable_id)
WHERE user_view_name = 'mat_m1'
\gset

SET ROLE :ROLE_SUPERUSER;
insert into  :"MAT_SCHEMA_NAME".:"MAT_TABLE_NAME"
select
 time_bucket('1week', timec),  _timescaledb_internal.partialize_agg( min(location)), _timescaledb_internal.partialize_agg( sum(temperature)) , _timescaledb_internal.partialize_agg( sum(humidity)), _timescaledb_internal.partialize_agg(stddev(humidity))
,1
from conditions
group by time_bucket('1week', timec) ;
SET ROLE :ROLE_DEFAULT_PERM_USER;

--should have same results --
select timec, minl, sumth, stddevh
from mat_m1
order by timec;

select time_bucket('1week', timec) ,
min(location), sum(temperature)+ sum(humidity), stddev(humidity)
from conditions
group by time_bucket('1week', timec)
order by time_bucket('1week', timec);

-- TEST4 --
--materialized view with group by clause + expression in SELECT
-- use previous data from conditions
--drop only the view.

-- apply where clause on result of mat_m1 --
DROP MATERIALIZED VIEW mat_m1;
CREATE MATERIALIZED VIEW mat_m1( timec, minl, sumth, stddevh)
WITH (timescaledb.continuous, timescaledb.materialized_only=true)
as
select time_bucket('1week', timec) ,
min(location), sum(temperature)+sum(humidity), stddev(humidity)
from conditions
where location = 'NYC'
group by time_bucket('1week', timec)
 WITH NO DATA;

SELECT ca.raw_hypertable_id as "RAW_HYPERTABLE_ID",
       h.schema_name AS "MAT_SCHEMA_NAME",
       h.table_name AS "MAT_TABLE_NAME",
       partial_view_name as "PART_VIEW_NAME",
       partial_view_schema as "PART_VIEW_SCHEMA"
FROM _timescaledb_catalog.continuous_agg ca
INNER JOIN _timescaledb_catalog.hypertable h ON(h.id = ca.mat_hypertable_id)
WHERE user_view_name = 'mat_m1'
\gset

SET ROLE :ROLE_SUPERUSER;
insert into  :"MAT_SCHEMA_NAME".:"MAT_TABLE_NAME"
select
 time_bucket('1week', timec),  _timescaledb_internal.partialize_agg( min(location)), _timescaledb_internal.partialize_agg( sum(temperature)) , _timescaledb_internal.partialize_agg( sum(humidity)), _timescaledb_internal.partialize_agg(stddev(humidity))
,1
from conditions
where location = 'NYC'
group by time_bucket('1week', timec) ;
SET ROLE :ROLE_DEFAULT_PERM_USER;

--should have same results --
select timec, minl, sumth, stddevh
from mat_m1
where stddevh is not null
order by timec;

select time_bucket('1week', timec) ,
min(location), sum(temperature)+ sum(humidity), stddev(humidity)
from conditions
where location = 'NYC'
group by time_bucket('1week', timec)
order by time_bucket('1week', timec);

-- TEST5 --
---------test with having clause ----------------------
DROP MATERIALIZED VIEW mat_m1;
create materialized view mat_m1( timec, minl, sumth, stddevh)
WITH (timescaledb.continuous, timescaledb.materialized_only=true)
as
select time_bucket('1week', timec) ,
min(location), sum(temperature)+sum(humidity), stddev(humidity)
from conditions
group by time_bucket('1week', timec)
having stddev(humidity) is not null WITH NO DATA;
;

SELECT ca.raw_hypertable_id as "RAW_HYPERTABLE_ID",
       h.schema_name AS "MAT_SCHEMA_NAME",
       h.table_name AS "MAT_TABLE_NAME",
       partial_view_name as "PART_VIEW_NAME",
       partial_view_schema as "PART_VIEW_SCHEMA"
FROM _timescaledb_catalog.continuous_agg ca
INNER JOIN _timescaledb_catalog.hypertable h ON(h.id = ca.mat_hypertable_id)
WHERE user_view_name = 'mat_m1'
\gset

SET ROLE :ROLE_SUPERUSER;
insert into  :"MAT_SCHEMA_NAME".:"MAT_TABLE_NAME"
select
 time_bucket('1week', timec),  _timescaledb_internal.partialize_agg( min(location)), _timescaledb_internal.partialize_agg( sum(temperature)) , _timescaledb_internal.partialize_agg( sum(humidity)), _timescaledb_internal.partialize_agg(stddev(humidity))
,1
from conditions
group by time_bucket('1week', timec) ;
SET ROLE :ROLE_DEFAULT_PERM_USER;

-- should have same results --
select * from mat_m1
order by sumth;

select time_bucket('1week', timec) ,
min(location), sum(temperature)+sum(humidity), stddev(humidity)
from conditions
group by time_bucket('1week', timec)
having stddev(humidity) is not null
order by sum(temperature)+sum(humidity);

-- TEST6 --
--group by with more than 1 group column
-- having clause with a mix of columns from select list + others

drop table conditions cascade;

CREATE TABLE conditions (
      timec       TIMESTAMPTZ       NOT NULL,
      location    TEXT              NOT NULL,
      temperature DOUBLE PRECISION  NULL,
      humidity    DOUBLE PRECISION  NULL,
      lowp        numeric NULL,
      highp       numeric null
    );

select table_name from create_hypertable( 'conditions', 'timec');

insert into conditions
select generate_series('2018-12-01 00:00'::timestamp, '2018-12-31 00:00'::timestamp, '1 day'), 'POR', 55, 75, 40, 70;
insert into conditions
select generate_series('2018-11-01 00:00'::timestamp, '2018-12-31 00:00'::timestamp, '1 day'), 'NYC', 35, 45, 50, 40;
insert into conditions
select generate_series('2018-11-01 00:00'::timestamp, '2018-12-15 00:00'::timestamp, '1 day'), 'LA', 73, 55, 71, 28;

--naming with AS clauses
CREATE MATERIALIZED VIEW mat_naming
WITH (timescaledb.continuous, timescaledb.materialized_only=true)
as
select time_bucket('1week', timec) as bucket, location as loc, sum(temperature)+sum(humidity), stddev(humidity)
from conditions
group by bucket, loc
having min(location) >= 'NYC' and avg(temperature) > 20 WITH NO DATA;

SELECT ca.raw_hypertable_id as "RAW_HYPERTABLE_ID",
       h.schema_name AS "MAT_SCHEMA_NAME",
       h.table_name AS "MAT_TABLE_NAME",
       partial_view_name as "PART_VIEW_NAME",
       partial_view_schema as "PART_VIEW_SCHEMA"
FROM _timescaledb_catalog.continuous_agg ca
INNER JOIN _timescaledb_catalog.hypertable h ON(h.id = ca.mat_hypertable_id)
WHERE user_view_name = 'mat_naming'
\gset

select attnum , attname from pg_attribute
where attnum > 0 and attrelid =
(Select oid from pg_class where relname like :'MAT_TABLE_NAME')
order by attnum, attname;

DROP MATERIALIZED VIEW mat_naming;

--naming with default names
CREATE MATERIALIZED VIEW mat_naming
WITH (timescaledb.continuous, timescaledb.materialized_only=true)
as
select time_bucket('1week', timec), location, sum(temperature)+sum(humidity), stddev(humidity)
from conditions
group by 1,2
having min(location) >= 'NYC' and avg(temperature) > 20 WITH NO DATA;

SELECT ca.raw_hypertable_id as "RAW_HYPERTABLE_ID",
       h.schema_name AS "MAT_SCHEMA_NAME",
       h.table_name AS "MAT_TABLE_NAME",
       partial_view_name as "PART_VIEW_NAME",
       partial_view_schema as "PART_VIEW_SCHEMA"
FROM _timescaledb_catalog.continuous_agg ca
INNER JOIN _timescaledb_catalog.hypertable h ON(h.id = ca.mat_hypertable_id)
WHERE user_view_name = 'mat_naming'
\gset

select attnum , attname from pg_attribute
where attnum > 0 and attrelid =
(Select oid from pg_class where relname like :'MAT_TABLE_NAME')
order by attnum, attname;

DROP MATERIALIZED VIEW mat_naming;

--naming with view col names
CREATE MATERIALIZED VIEW mat_naming(bucket, loc, sum_t_h, stdd)
WITH (timescaledb.continuous, timescaledb.materialized_only=true)
as
select time_bucket('1week', timec), location, sum(temperature)+sum(humidity), stddev(humidity)
from conditions
group by 1,2
having min(location) >= 'NYC' and avg(temperature) > 20 WITH NO DATA;

SELECT ca.raw_hypertable_id as "RAW_HYPERTABLE_ID",
       h.schema_name AS "MAT_SCHEMA_NAME",
       h.table_name AS "MAT_TABLE_NAME",
       partial_view_name as "PART_VIEW_NAME",
       partial_view_schema as "PART_VIEW_SCHEMA"
FROM _timescaledb_catalog.continuous_agg ca
INNER JOIN _timescaledb_catalog.hypertable h ON(h.id = ca.mat_hypertable_id)
WHERE user_view_name = 'mat_naming'
\gset

select attnum , attname from pg_attribute
where attnum > 0 and attrelid =
(Select oid from pg_class where relname like :'MAT_TABLE_NAME')
order by attnum, attname;

DROP MATERIALIZED VIEW mat_naming;

CREATE MATERIALIZED VIEW mat_m1(timec, minl, sumth, stddevh)
WITH (timescaledb.continuous, timescaledb.materialized_only=true)
as
select time_bucket('1week', timec) ,
min(location), sum(temperature)+sum(humidity), stddev(humidity)
from conditions
group by  time_bucket('1week', timec)
having min(location) >= 'NYC' and avg(temperature) > 20 WITH NO DATA;

SELECT ca.raw_hypertable_id as "RAW_HYPERTABLE_ID",
       h.schema_name AS "MAT_SCHEMA_NAME",
       h.table_name AS "MAT_TABLE_NAME",
       partial_view_name as "PART_VIEW_NAME",
       partial_view_schema as "PART_VIEW_SCHEMA"
FROM _timescaledb_catalog.continuous_agg ca
INNER JOIN _timescaledb_catalog.hypertable h ON(h.id = ca.mat_hypertable_id)
WHERE user_view_name = 'mat_m1'
\gset

select attnum , attname from pg_attribute
where attnum > 0 and attrelid =
(Select oid from pg_class where relname like :'MAT_TABLE_NAME')
order by attnum, attname;

SET ROLE :ROLE_SUPERUSER;
insert into  :"MAT_SCHEMA_NAME".:"MAT_TABLE_NAME"
select
 time_bucket('1week', timec),  _timescaledb_internal.partialize_agg( min(location)), _timescaledb_internal.partialize_agg( sum(temperature)) , _timescaledb_internal.partialize_agg( sum(humidity)), _timescaledb_internal.partialize_agg(stddev(humidity))
,_timescaledb_internal.partialize_agg( avg(temperature))
,1
from conditions
group by time_bucket('1week', timec) ;
SET ROLE :ROLE_DEFAULT_PERM_USER;

--should have same results --
select timec, minl, sumth, stddevh
from mat_m1
order by timec, minl;

select time_bucket('1week', timec) ,
min(location), sum(temperature)+sum(humidity), stddev(humidity)
from conditions
group by  time_bucket('1week', timec)
having min(location) >= 'NYC' and avg(temperature) > 20 and avg(lowp) > 10
order by time_bucket('1week', timec), min(location);

--check view defintion in information views
select view_name, view_definition from timescaledb_information.continuous_aggregates
where view_name::text like 'mat_m1';

--TEST6 -- select from internal view

SET ROLE :ROLE_SUPERUSER;
insert into :"MAT_SCHEMA_NAME".:"MAT_TABLE_NAME"
select * from :"PART_VIEW_SCHEMA".:"PART_VIEW_NAME";
SET ROLE :ROLE_DEFAULT_PERM_USER;

--lets drop the view and check
DROP MATERIALIZED VIEW mat_m1;

drop table conditions;
CREATE TABLE conditions (
      timec       TIMESTAMPTZ       NOT NULL,
      location    TEXT              NOT NULL,
      temperature DOUBLE PRECISION  NULL,
      humidity    DOUBLE PRECISION  NULL,
      lowp        double precision NULL,
      highp       double precision null,
      allnull     double precision null
    );

select table_name from create_hypertable( 'conditions', 'timec');

insert into conditions
select generate_series('2018-12-01 00:00'::timestamp, '2018-12-31 00:00'::timestamp, '1 day'), 'POR', 55, 75, 40, 70, NULL;
insert into conditions
select generate_series('2018-11-01 00:00'::timestamp, '2018-12-31 00:00'::timestamp, '1 day'), 'NYC', 35, 45, 50, 40, NULL;
insert into conditions
select generate_series('2018-11-01 00:00'::timestamp, '2018-12-15 00:00'::timestamp, '1 day'), 'LA', 73, 55, NULL, 28, NULL;


SELECT
  $$
  select time_bucket('1week', timec) ,
  min(location) as col1, sum(temperature)+sum(humidity) as col2, stddev(humidity) as col3, min(allnull) as col4
  from conditions
  group by  time_bucket('1week', timec)
  having min(location) >= 'NYC' and avg(temperature) > 20
  $$ AS "QUERY"
\gset


\set ECHO errors
\ir include/cont_agg_equal.sql
\set ECHO all

SELECT
  $$
  select time_bucket('1week', timec), location,
  sum(temperature)+sum(humidity) as col2, stddev(humidity) as col3, min(allnull) as col4
  from conditions
  group by location, time_bucket('1week', timec)
  $$ AS "QUERY"
\gset

\set ECHO errors
\ir include/cont_agg_equal.sql
\set ECHO all

--TEST7 -- drop tests for view and hypertable
--DROP tests
\set ON_ERROR_STOP 0
SELECT  h.schema_name AS "MAT_SCHEMA_NAME",
       h.table_name AS "MAT_TABLE_NAME",
       partial_view_name as "PART_VIEW_NAME",
       partial_view_schema as "PART_VIEW_SCHEMA",
       direct_view_name as "DIR_VIEW_NAME",
       direct_view_schema as "DIR_VIEW_SCHEMA"
FROM _timescaledb_catalog.continuous_agg ca
INNER JOIN _timescaledb_catalog.hypertable h ON(h.id = ca.mat_hypertable_id)
WHERE user_view_name = 'mat_test'
\gset

DROP TABLE :"MAT_SCHEMA_NAME".:"MAT_TABLE_NAME";
DROP VIEW :"PART_VIEW_SCHEMA".:"PART_VIEW_NAME";
DROP VIEW :"DIR_VIEW_SCHEMA".:"DIR_VIEW_NAME";
\set ON_ERROR_STOP 1

--catalog entry still there;
SELECT count(*)
FROM _timescaledb_catalog.continuous_agg ca
WHERE user_view_name = 'mat_test';

--mat table, user_view, direct view and partial view all there
select count(*) from pg_class where relname = :'PART_VIEW_NAME';
select count(*) from pg_class where relname = :'MAT_TABLE_NAME';
select count(*) from pg_class where relname = :'DIR_VIEW_NAME';
select count(*) from pg_class where relname = 'mat_test';

DROP MATERIALIZED VIEW mat_test;

--catalog entry should be gone
SELECT count(*)
FROM _timescaledb_catalog.continuous_agg ca
WHERE user_view_name = 'mat_test';

--mat table, user_view, direct view and partial view all gone
select count(*) from pg_class where relname = :'PART_VIEW_NAME';
select count(*) from pg_class where relname = :'MAT_TABLE_NAME';
select count(*) from pg_class where relname = :'DIR_VIEW_NAME';
select count(*) from pg_class where relname = 'mat_test';


--test dropping raw table
DROP TABLE conditions;
CREATE TABLE conditions (
      timec       TIMESTAMPTZ       NOT NULL,
      location    TEXT              NOT NULL,
      temperature DOUBLE PRECISION  NULL,
      humidity    DOUBLE PRECISION  NULL,
      lowp        double precision NULL,
      highp       double precision null,
      allnull     double precision null
    );

select table_name from create_hypertable( 'conditions', 'timec');

--no data in hyper table on purpose so that CASCADE is not required because of chunks

CREATE MATERIALIZED VIEW mat_drop_test(timec, minl, sumt , sumh)
WITH (timescaledb.continuous, timescaledb.materialized_only=true)
as
select time_bucket('1day', timec), min(location), sum(temperature),sum(humidity)
from conditions
group by time_bucket('1day', timec) WITH NO DATA;

\set ON_ERROR_STOP 0
DROP TABLE conditions;
\set ON_ERROR_STOP 1

--insert data now

insert into conditions
select generate_series('2018-12-01 00:00'::timestamp, '2018-12-31 00:00'::timestamp, '1 day'), 'POR', 55, 75, 40, 70, NULL;
insert into conditions
select generate_series('2018-11-01 00:00'::timestamp, '2018-12-31 00:00'::timestamp, '1 day'), 'NYC', 35, 45, 50, 40, NULL;
insert into conditions
select generate_series('2018-11-01 00:00'::timestamp, '2018-12-15 00:00'::timestamp, '1 day'), 'LA', 73, 55, NULL, 28, NULL;


SELECT ca.raw_hypertable_id as "RAW_HYPERTABLE_ID",
       h.schema_name AS "MAT_SCHEMA_NAME",
       h.table_name AS "MAT_TABLE_NAME",
       partial_view_name as "PART_VIEW_NAME",
       partial_view_schema as "PART_VIEW_SCHEMA"
FROM _timescaledb_catalog.continuous_agg ca
INNER JOIN _timescaledb_catalog.hypertable h ON(h.id = ca.mat_hypertable_id)
WHERE user_view_name = 'mat_drop_test'
\gset

SET client_min_messages TO LOG;
CALL refresh_continuous_aggregate('mat_drop_test', NULL, NULL);

--force invalidation
insert into conditions
select generate_series('2017-11-01 00:00'::timestamp, '2017-12-15 00:00'::timestamp, '1 day'), 'LA', 73, 55, NULL, 28, NULL;

select count(*) from _timescaledb_catalog.continuous_aggs_invalidation_threshold;
select count(*) from _timescaledb_catalog.continuous_aggs_hypertable_invalidation_log;

DROP TABLE conditions CASCADE;

--catalog entry should be gone
SELECT count(*)
FROM _timescaledb_catalog.continuous_agg ca
WHERE user_view_name = 'mat_drop_test';
select count(*) from _timescaledb_catalog.continuous_aggs_invalidation_threshold;
select count(*) from _timescaledb_catalog.continuous_aggs_hypertable_invalidation_log;
select count(*) from _timescaledb_catalog.continuous_aggs_materialization_invalidation_log;

SELECT * FROM _timescaledb_config.bgw_job;

--mat table, user_view, and partial view all gone
select count(*) from pg_class where relname = :'PART_VIEW_NAME';
select count(*) from pg_class where relname = :'MAT_TABLE_NAME';
select count(*) from pg_class where relname = 'mat_drop_test';

--TEST With options

CREATE TABLE conditions (
      timec       TIMESTAMPTZ       NOT NULL,
      location    TEXT              NOT NULL,
      temperature DOUBLE PRECISION  NULL,
      humidity    DOUBLE PRECISION  NULL,
      lowp        double precision NULL,
      highp       double precision null,
      allnull     double precision null
    );

select table_name from create_hypertable( 'conditions', 'timec');

CREATE MATERIALIZED VIEW mat_with_test(timec, minl, sumt , sumh)
WITH (timescaledb.continuous,
      timescaledb.materialized_only=true)
as
select time_bucket('1day', timec), min(location), sum(temperature),sum(humidity)
from conditions
group by time_bucket('1day', timec), location, humidity, temperature WITH NO DATA;

SELECT add_continuous_aggregate_policy('mat_with_test', NULL, '5 h'::interval, '12 h'::interval);
SELECT alter_job(id, schedule_interval => '1h') FROM _timescaledb_config.bgw_job;
SELECT schedule_interval FROM _timescaledb_config.bgw_job;

SELECT alter_job(id, schedule_interval => '2h') FROM _timescaledb_config.bgw_job;
SELECT schedule_interval FROM _timescaledb_config.bgw_job;

select indexname, indexdef from pg_indexes where tablename =
(SELECT h.table_name
FROM _timescaledb_catalog.continuous_agg ca
INNER JOIN _timescaledb_catalog.hypertable h ON(h.id = ca.mat_hypertable_id)
WHERE user_view_name = 'mat_with_test')
order by indexname;

DROP MATERIALIZED VIEW mat_with_test;
--no additional indexes
CREATE MATERIALIZED VIEW mat_with_test(timec, minl, sumt , sumh)
WITH (timescaledb.continuous,
      timescaledb.materialized_only=true,
      timescaledb.create_group_indexes=false)
as
select time_bucket('1day', timec), min(location), sum(temperature),sum(humidity)
from conditions
group by time_bucket('1day', timec), location, humidity, temperature WITH NO DATA;

select indexname, indexdef from pg_indexes where tablename =
(SELECT h.table_name
FROM _timescaledb_catalog.continuous_agg ca
INNER JOIN _timescaledb_catalog.hypertable h ON(h.id = ca.mat_hypertable_id)
WHERE user_view_name = 'mat_with_test');

DROP TABLE conditions CASCADE;

--test WITH using a hypertable with an integer time dimension
CREATE TABLE conditions (
      timec       INT       NOT NULL,
      location    TEXT              NOT NULL,
      temperature DOUBLE PRECISION  NULL,
      humidity    DOUBLE PRECISION  NULL,
      lowp        double precision NULL,
      highp       double precision null,
      allnull     double precision null
    );

select table_name from create_hypertable( 'conditions', 'timec', chunk_time_interval=> 100);

CREATE OR REPLACE FUNCTION integer_now_conditions() returns int LANGUAGE SQL STABLE as $$ SELECT coalesce(max(timec), 0) FROM conditions $$;
SELECT set_integer_now_func('conditions', 'integer_now_conditions');

CREATE MATERIALIZED VIEW mat_with_test(timec, minl, sumt , sumh)
WITH (timescaledb.continuous,
      timescaledb.materialized_only=true)
as
select time_bucket(100, timec), min(location), sum(temperature),sum(humidity)
from conditions
group by time_bucket(100, timec) WITH NO DATA;

SELECT add_continuous_aggregate_policy('mat_with_test', NULL, 500::integer, '12 h'::interval);
SELECT alter_job(id, schedule_interval => '2h') FROM _timescaledb_config.bgw_job;

SELECT schedule_interval FROM _timescaledb_config.bgw_job;

DROP TABLE conditions CASCADE;


--test space partitions
CREATE TABLE space_table (
    time BIGINT,
    dev  BIGINT,
    data BIGINT
);

SELECT create_hypertable(
    'space_table',
    'time',
    chunk_time_interval => 10,
    partitioning_column => 'dev',
    number_partitions => 3);

CREATE OR REPLACE FUNCTION integer_now_space_table() returns BIGINT LANGUAGE SQL STABLE as $$ SELECT coalesce(max(time), BIGINT '0') FROM space_table $$;
SELECT set_integer_now_func('space_table', 'integer_now_space_table');

CREATE MATERIALIZED VIEW space_view
WITH (timescaledb.continuous,
      timescaledb.materialized_only=true)
AS SELECT time_bucket('4', time), COUNT(data)
   FROM space_table
   GROUP BY 1 WITH NO DATA;

INSERT INTO space_table VALUES
  (0, 1, 1), (0, 2, 1), (1, 1, 1), (1, 2, 1),
  (10, 1, 1), (10, 2, 1), (11, 1, 1), (11, 2, 1);

SELECT  h.schema_name AS "MAT_SCHEMA_NAME",
       h.table_name AS "MAT_TABLE_NAME",
       partial_view_name as "PART_VIEW_NAME",
       partial_view_schema as "PART_VIEW_SCHEMA",
       direct_view_name as "DIR_VIEW_NAME",
       direct_view_schema as "DIR_VIEW_SCHEMA"
FROM _timescaledb_catalog.continuous_agg ca
INNER JOIN _timescaledb_catalog.hypertable h ON(h.id = ca.mat_hypertable_id)
WHERE user_view_name = 'space_view'
\gset

SELECT * FROM :"MAT_SCHEMA_NAME".:"MAT_TABLE_NAME"
  ORDER BY time_bucket, chunk_id;


CALL refresh_continuous_aggregate('space_view', NULL, NULL);

SELECT * FROM space_view ORDER BY 1;

SELECT * FROM :"MAT_SCHEMA_NAME".:"MAT_TABLE_NAME"
  ORDER BY time_bucket, chunk_id;


INSERT INTO space_table VALUES (3, 2, 1);

CALL refresh_continuous_aggregate('space_view', NULL, NULL);

SELECT * FROM space_view ORDER BY 1;

SELECT * FROM :"MAT_SCHEMA_NAME".:"MAT_TABLE_NAME"
  ORDER BY time_bucket, chunk_id;


INSERT INTO space_table VALUES (2, 3, 1);

CALL refresh_continuous_aggregate('space_view', NULL, NULL);

SELECT * FROM space_view ORDER BY 1;

SELECT * FROM :"MAT_SCHEMA_NAME".:"MAT_TABLE_NAME"
  ORDER BY time_bucket, chunk_id;


DROP TABLE space_table CASCADE;

--
-- TEST FINALIZEFUNC_EXTRA
--

-- create special aggregate to test ffunc_extra
-- Raise warning with the actual type being passed in
CREATE OR REPLACE FUNCTION fake_ffunc(a int8, b int, c int, d int, x anyelement)
RETURNS anyelement AS $$
BEGIN
 RAISE WARNING 'type % %', pg_typeof(d), pg_typeof(x);
 RETURN x;
END;
$$
LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION fake_sfunc(a int8, b int, c int, d int, x anyelement)
RETURNS int8 AS $$
BEGIN
 RETURN b;
END; $$
LANGUAGE plpgsql;


CREATE AGGREGATE aggregate_to_test_ffunc_extra(int, int, int, anyelement) (
    SFUNC = fake_sfunc,
    STYPE = int8,
    COMBINEFUNC = int8pl,
    FINALFUNC = fake_ffunc,
    PARALLEL = SAFE,
    FINALFUNC_EXTRA
);

CREATE TABLE conditions (
      timec       INT       NOT NULL,
      location    TEXT              NOT NULL,
      temperature DOUBLE PRECISION  NULL,
      humidity    DOUBLE PRECISION  NULL,
      lowp        double precision NULL,
      highp       double precision null,
      allnull     double precision null
    );

select table_name from create_hypertable( 'conditions', 'timec', chunk_time_interval=> 100);

CREATE OR REPLACE FUNCTION integer_now_conditions() returns int LANGUAGE SQL STABLE as $$ SELECT coalesce(max(timec), 0) FROM conditions $$;
SELECT set_integer_now_func('conditions', 'integer_now_conditions');

insert into conditions
select generate_series(0, 200, 10), 'POR', 55, 75, 40, 70, NULL;


CREATE MATERIALIZED VIEW mat_ffunc_test
WITH (timescaledb.continuous, timescaledb.materialized_only=true)
as
select time_bucket(100, timec), aggregate_to_test_ffunc_extra(timec, 1, 3, 'test'::text)
from conditions
group by time_bucket(100, timec);

SELECT * FROM mat_ffunc_test;

DROP MATERIALIZED view mat_ffunc_test;

CREATE MATERIALIZED VIEW mat_ffunc_test
WITH (timescaledb.continuous, timescaledb.materialized_only=true)
as
select time_bucket(100, timec), aggregate_to_test_ffunc_extra(timec, 4, 5, bigint '123')
from conditions
group by time_bucket(100, timec);

SELECT * FROM mat_ffunc_test;

--refresh mat view test when time_bucket is not projected --
DROP MATERIALIZED VIEW mat_ffunc_test;
CREATE MATERIALIZED VIEW mat_refresh_test
WITH (timescaledb.continuous, timescaledb.materialized_only=true)
as
select location, max(humidity)
from conditions
group by time_bucket(100, timec), location WITH NO DATA;

insert into conditions
select generate_series(0, 50, 10), 'NYC', 55, 75, 40, 70, NULL;

CALL refresh_continuous_aggregate('mat_refresh_test', NULL, NULL);
SELECT * FROM mat_refresh_test order by 1,2 ;

-- test for bug when group by is not in project list
CREATE MATERIALIZED VIEW conditions_grpby_view with (timescaledb.continuous) as
select time_bucket(100, timec),  sum(humidity)
from conditions
group by time_bucket(100, timec), location;
select * from conditions_grpby_view order by 1, 2;

CREATE MATERIALIZED VIEW conditions_grpby_view2 with (timescaledb.continuous) as
select time_bucket(100, timec), sum(humidity)
from conditions
group by time_bucket(100, timec), location
having avg(temperature) > 0;

select * from conditions_grpby_view2 order by 1, 2;

-- Test internal functions for continuous aggregates
SELECT test.continuous_aggs_find_view('mat_refresh_test');

-- Test pseudotype/enum handling
CREATE TYPE status_enum AS ENUM (
  'red',
  'yellow',
  'green'
);

CREATE TABLE cagg_types (
  time TIMESTAMPTZ NOT NULL,
  status status_enum,
  names NAME[],
  floats FLOAT[]
);

SELECT
  table_name
FROM
  create_hypertable('cagg_types', 'time');

INSERT INTO cagg_types
SELECT
  '2000-01-01',
  'yellow',
  '{foo,bar,baz}',
  '{1,2.5,3}';

CREATE MATERIALIZED VIEW mat_types WITH (timescaledb.continuous) AS
SELECT
  time_bucket('1d', time),
  min(status) AS status,
  max(names) AS names,
  min(floats) AS floats
FROM
  cagg_types
GROUP BY
  1;

CALL refresh_continuous_aggregate('mat_types',NULL,NULL);
SELECT * FROM mat_types;

