-- This file and its contents are licensed under the Timescale License.
-- Please see the included NOTICE for copyright information and
-- LICENSE-TIMESCALE for a copy of the license.

-- Need to be super user to create extension and add data nodes
\c :TEST_DBNAME :ROLE_CLUSTER_SUPERUSER;

\unset ECHO
\o /dev/null
\ir include/remote_exec.sql
\o
\set ECHO all

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

-- Create a new table access method by reusing heap handler
CREATE ACCESS METHOD test_am TYPE TABLE HANDLER heap_tableam_handler;

SELECT * FROM test.remote_exec('{ data_node_1, data_node_2, data_node_3 }', $$
CREATE ACCESS METHOD test_am TYPE TABLE HANDLER heap_tableam_handler;
$$);

-- Create distributed hypertable using non-default access method
CREATE TABLE disttable(time timestamptz NOT NULL, device int) USING test_am;
SELECT * FROM create_distributed_hypertable('disttable', 'time', 'device', 3);

-- Make sure that distributed hypertable created on data nodes is
-- using the correct table access method
SELECT * FROM test.remote_exec('{ data_node_1, data_node_2, data_node_3 }', $$

SELECT amname AS hypertable_amname
FROM pg_class cl, pg_am am
WHERE cl.oid = 'disttable'::regclass
AND cl.relam = am.oid;
$$);

-- Check that basic operations are working as expected
INSERT INTO disttable VALUES
       ('2017-01-01 06:01', 1),
       ('2017-01-01 09:11', 3),
       ('2017-01-01 08:01', 1),
       ('2017-01-02 08:01', 2),
       ('2018-07-02 08:01', 87),
       ('2018-07-01 06:01', 13),
       ('2018-07-01 09:11', 90),
       ('2018-07-01 08:01', 29);

SELECT * FROM disttable ORDER BY time;
