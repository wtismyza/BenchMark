-- This file and its contents are licensed under the Timescale License.
-- Please see the included NOTICE for copyright information and
-- LICENSE-TIMESCALE for a copy of the license.


-- ===================================================================
-- create fake fdw to create data node
-- ===================================================================

\c :TEST_DBNAME :ROLE_SUPERUSER

CREATE FUNCTION test.remote_connection_tests()
RETURNS void
AS :TSL_MODULE_PATHNAME, 'ts_test_remote_connection'
LANGUAGE C STRICT;

CREATE FUNCTION test.remote_async_tests()
RETURNS void
AS :TSL_MODULE_PATHNAME, 'ts_test_remote_async'
LANGUAGE C STRICT;

CREATE FUNCTION test.send_remote_query_that_generates_exception()
RETURNS void
AS :TSL_MODULE_PATHNAME, 'ts_test_bad_remote_query'
LANGUAGE C STRICT;

CREATE FUNCTION test.get_connection_stats()
RETURNS TABLE(connections_created bigint, connections_closed bigint, results_created bigint, results_cleared bigint)
AS :TSL_MODULE_PATHNAME, 'ts_test_get_connection_stats'
LANGUAGE C STRICT;

-- ===================================================================
-- create tables
-- ===================================================================

CREATE SCHEMA "S 1";
CREATE TABLE "S 1"."T 1" (
    "C 1" int NOT NULL,
    c2 int NOT NULL,
    c3 text,
    c4 timestamptz,
    c5 timestamp,
    c6 varchar(10),
    c7 char(10),
    CONSTRAINT t1_pkey PRIMARY KEY ("C 1")
);

ANALYZE "S 1"."T 1";

INSERT INTO "S 1"."T 1"
    SELECT id,
           id % 10,
           to_char(id, 'FM00000'),
           '1970-01-01'::timestamptz + ((id % 100) || ' days')::interval,
           '1970-01-01'::timestamp + ((id % 100) || ' days')::interval,
           id % 10,
           id % 10
    FROM generate_series(1, 1000) id;

-- ===================================================================
-- run tests
-- ===================================================================

SELECT * FROM test.get_connection_stats();
\set ON_ERROR_STOP 0
SELECT test.send_remote_query_that_generates_exception();
\set ON_ERROR_STOP 1
SELECT * FROM test.get_connection_stats();

SELECT test.remote_connection_tests();
SET client_min_messages TO ERROR;
SELECT test.remote_async_tests();

\echo 'End Of Test'
