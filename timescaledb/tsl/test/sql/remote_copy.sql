-- This file and its contents are licensed under the Timescale License.
-- Please see the included NOTICE for copyright information and
-- LICENSE-TIMESCALE for a copy of the license.

\c :TEST_DBNAME :ROLE_CLUSTER_SUPERUSER;

-- Cleanup from other potential tests that created these databases
SET client_min_messages TO ERROR;
DROP DATABASE IF EXISTS data_node_1;
DROP DATABASE IF EXISTS data_node_2;
DROP DATABASE IF EXISTS data_node_3;
SET client_min_messages TO NOTICE;

-- Add data nodes using the TimescaleDB node management API
SELECT * FROM add_data_node('data_node_1', host => 'localhost',
                            database => 'data_node_1');
SELECT * FROM add_data_node('data_node_2', host => 'localhost',
                            database => 'data_node_2');
SELECT * FROM add_data_node('data_node_3', host => 'localhost',
                            database => 'data_node_3');
GRANT USAGE ON FOREIGN SERVER data_node_1, data_node_2, data_node_3 TO PUBLIC;

-- Start out testing text copy code
SET timescaledb.enable_connection_binary_data=false;

SET ROLE :ROLE_1;

-- Use some horrible names to make sure the parsing code works
CREATE TABLE "+ri(k33_')" (
    "thyme" bigint NOT NULL,
    "))_" double precision NOT NULL,
    "flavor" text DEFAULT 'mint',
    "pH" float DEFAULT 7.0,
    optional text
);

SELECT create_hypertable('"+ri(k33_'')"', 'thyme', partitioning_column=>'pH', number_partitions=>4, chunk_time_interval => 100, replication_factor => 2);

-- Run some successful copies
COPY "+ri(k33_')" FROM STDIN;
1	11	strawberry	2.3	stuff
\.

\copy public    .		"+ri(k33_')" ("pH",     "))_"   ,	thyme) fROm stdIN deLIMitER '-';
.01-40-208
10.-37-315
\.

cOpy public."+ri(k33_')" (thYme, "pH", "))_", "flavor") FrOm
StDiN wiTH dElImITeR ','
;
15,1,403,\N
203,1.0,3.21321,something like lemon
333,1.00,2309424231,  _''garbled*(#\\)@#$*)
\.

COPY "+ri(k33_')" FROM STDIN (FORCE_NULL (flavor, "))_"), QUOTE '`', FREEZE, FORMAT csv, NULL 'empties', FORCE_NOT_NULL ("pH", "thyme"));
120321,4.4324424324254352345345,``,0,empties
4201,3333333333333333333333333333,"",1.0000000000000000000000000000000001,`empties`
342,4324,"empties",4,\N
\.

-- Run some error cases
\set ON_ERROR_STOP 0

-- Bad input
COPY "+ri(k33_')" FROM STDIN WITH DELIMITER ',';
1,white,blue,grey,teal
\.

-- Missing paritioning column
COPY "+ri(k33_')" (thYme, "))_", "flavor") FROM STDIN;
1234,\N,resentment
\.

-- Missing required column, these generate a WARNING with a transaction id in them (too flimsy to output)
SET client_min_messages TO ERROR;
COPY "+ri(k33_')" (thyme, flavor, "pH") FROM STDIN WITH DELIMITER ',';
5,blue,2.0
\.
COPY "+ri(k33_')" FROM STDIN WITH DELIMITER ',';
5,\N,blue,1,blah
\.
SET client_min_messages TO INFO;

-- Invalid data after new chunk creation, data and chunks should be rolled back
COPY "+ri(k33_')" FROM STDIN WITH DELIMITER ',';
700,7,neopolitan,7,seven
800,8,pesto,8,eight
900,9,salami,9,nine
1000,white,blue,ten,hi
\.

\set ON_ERROR_STOP 1

-- Now do some testing of the binary frontend/backend path.
SET timescaledb.enable_connection_binary_data=true;

COPY "+ri(k33_')" FROM STDIN;
10	11	strawberry	12.3	stuff
\.

\copy public    .		"+ri(k33_')" ("pH",     "))_"   ,	thyme) fROm stdIN deLIMitER '-';
.001-40-2080
100.-37-3150
\.

cOpy public."+ri(k33_')" (thYme, "pH", "))_", "flavor") FrOm
StDiN wiTH dElImITeR ','
;
150,10,403,\N
2030,10.0,3.21321,something like lemon
3330,10.00,2309424231,  _''garbled*(#\\)@#$*)
\.

COPY "+ri(k33_')" FROM STDIN (FORCE_NULL (flavor, "))_"), QUOTE '`', FREEZE, FORMAT csv, NULL 'empties', FORCE_NOT_NULL ("pH", "thyme"));
1203210,4.4324424324254352345345,``,0,empties
42010,3333333333333333333333333333,"",1.00000000000000000000000000000000001,`empties`
3420,4324,"empties",40,\N
\.

SELECT * FROM "+ri(k33_')";
SELECT * FROM _timescaledb_catalog.chunk;
SELECT * FROM _timescaledb_catalog.chunk_data_node;
SELECT * FROM _timescaledb_catalog.hypertable_data_node;
select * from show_chunks('"+ri(k33_'')"');
\c data_node_1
SELECT * FROM "+ri(k33_')";
select * from show_chunks('"+ri(k33_'')"');
\c data_node_2
SELECT * FROM "+ri(k33_')";
select * from show_chunks('"+ri(k33_'')"');
\c data_node_3
SELECT * FROM "+ri(k33_')";
select * from show_chunks('"+ri(k33_'')"');
\c :TEST_DBNAME :ROLE_SUPERUSER;
SET ROLE :ROLE_1;

DROP TABLE "+ri(k33_')" CASCADE;
SET ROLE :ROLE_CLUSTER_SUPERUSER;
SELECT * FROM delete_data_node('data_node_1');
SELECT * FROM delete_data_node('data_node_2');
SELECT * FROM delete_data_node('data_node_3');
DROP DATABASE data_node_1;
DROP DATABASE data_node_2;
DROP DATABASE data_node_3;
