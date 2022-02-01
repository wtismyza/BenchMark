-- This file and its contents are licensed under the Timescale License.
-- Please see the included NOTICE for copyright information and
-- LICENSE-TIMESCALE for a copy of the license.

\c :TEST_DBNAME :ROLE_CLUSTER_SUPERUSER

-- Support for execute_sql_and_filter_server_name_on_error()
\unset ECHO
\o /dev/null
\ir include/remote_exec.sql
\ir include/filter_exec.sql
\o
\set ECHO all

-- Cleanup from other potential tests that created these databases
SET client_min_messages TO ERROR;
DROP DATABASE IF EXISTS data_node_1;
DROP DATABASE IF EXISTS data_node_2;
DROP DATABASE IF EXISTS data_node_3;
SET client_min_messages TO NOTICE;

SELECT * FROM add_data_node('data_node_1', host => 'localhost',
                            database => 'data_node_1');
SELECT * FROM add_data_node('data_node_2', host => 'localhost',
                            database => 'data_node_2');
SELECT * FROM add_data_node('data_node_3', host => 'localhost',
                            database => 'data_node_3');
GRANT USAGE ON FOREIGN SERVER data_node_1, data_node_2, data_node_3 TO PUBLIC;

\des+

RESET ROLE;
CREATE FUNCTION _timescaledb_internal.invoke_distributed_commands()
RETURNS void
AS :TSL_MODULE_PATHNAME, 'ts_invoke_distributed_commands'
LANGUAGE C STRICT;

CREATE FUNCTION _timescaledb_internal.invoke_faulty_distributed_command()
RETURNS void
AS :TSL_MODULE_PATHNAME, 'ts_invoke_faulty_distributed_command'
LANGUAGE C STRICT;
SET ROLE :ROLE_1;

SELECT _timescaledb_internal.invoke_distributed_commands();

\c data_node_1
\dt
SELECT * FROM disttable1;
\c data_node_2
\dt
SELECT * FROM disttable1;
\c data_node_3
\dt
SELECT * FROM disttable1;
\c :TEST_DBNAME :ROLE_SUPERUSER
SET ROLE :ROLE_1;

-- Verify failed insert command gets fully rolled back
\set ON_ERROR_STOP 0
SELECT _timescaledb_internal.invoke_faulty_distributed_command();
\set ON_ERROR_STOP 1

\c data_node_1
SELECT * from disttable2;
\c data_node_2
SELECT * from disttable2;

-- Test connection session identity
\c :TEST_DBNAME :ROLE_SUPERUSER
\unset ECHO
\o /dev/null
\ir include/remote_exec.sql
\o
\set ECHO all

-- Register is_frontend_session() function and test that it returns false for
-- connections openned by test suite. This simualates behaviour expected
-- with a client connections.
CREATE OR REPLACE FUNCTION is_frontend_session()
RETURNS BOOL
AS :TSL_MODULE_PATHNAME, 'ts_test_is_frontend_session' LANGUAGE C;

\c data_node_1
CREATE OR REPLACE FUNCTION is_frontend_session()
RETURNS BOOL
AS :TSL_MODULE_PATHNAME, 'ts_test_is_frontend_session' LANGUAGE C;
SELECT is_frontend_session();

\c data_node_2
CREATE OR REPLACE FUNCTION is_frontend_session()
RETURNS BOOL
AS :TSL_MODULE_PATHNAME, 'ts_test_is_frontend_session' LANGUAGE C;
SELECT is_frontend_session();

\c data_node_3
CREATE OR REPLACE FUNCTION is_frontend_session()
RETURNS BOOL
AS :TSL_MODULE_PATHNAME, 'ts_test_is_frontend_session' LANGUAGE C;
SELECT is_frontend_session();

\c :TEST_DBNAME :ROLE_SUPERUSER
SET ROLE :ROLE_1;
SELECT is_frontend_session();

-- Ensure peer dist id is already set and can be set only once
\set ON_ERROR_STOP 0
SELECT * FROM test.remote_exec('{data_node_1}', $$ SELECT * FROM _timescaledb_internal.set_peer_dist_id('77348176-09da-4a80-bc78-e31bdf5e63ec'); $$);
\set ON_ERROR_STOP 1

-- Repeat is_frontend_session() test again, but this time using connections openned from frontend
-- to backend nodes. Must return true.
SELECT * FROM test.remote_exec(NULL, $$ SELECT is_frontend_session(); $$);

-- Test distributed_exec()

-- Make sure dist session is properly set
CALL distributed_exec('DO $$ BEGIN ASSERT(SELECT is_frontend_session()) = true; END; $$;');

-- Test creating and dropping a table
CALL distributed_exec('CREATE TABLE dist_test (id int)');
CALL distributed_exec('INSERT INTO dist_test values (7)');
SELECT * FROM test.remote_exec(NULL, $$ SELECT * from dist_test; $$);
CALL distributed_exec('DROP TABLE dist_test');
\set ON_ERROR_STOP 0
CALL distributed_exec('INSERT INTO dist_test VALUES (8)', '{data_node_1}');
\set ON_ERROR_STOP 1

-- Test creating and dropping a role
CREATE ROLE dist_test_role;
-- Expect this to be an error, since data nodes are created on the same instance
\set ON_ERROR_STOP 0
SELECT test.execute_sql_and_filter_data_node_name_on_error($$
CALL distributed_exec('CREATE ROLE dist_test_role');
$$);
\set ON_ERROR_STOP 1
SELECT * FROM test.remote_exec(NULL, $$ SELECT true from pg_catalog.pg_roles WHERE rolname = 'dist_test_role'; $$);
DROP ROLE DIST_TEST_ROLE;
\set ON_ERROR_STOP 0
SELECT test.execute_sql_and_filter_data_node_name_on_error($$
CALL distributed_exec('DROP ROLE dist_test_role');
$$);
\set ON_ERROR_STOP 1

-- Do not allow to run distributed_exec() on a data nodes
\c data_node_1
\set ON_ERROR_STOP 0
CALL distributed_exec('SELECT 1');
\set ON_ERROR_STOP 1

\c :TEST_DBNAME :ROLE_SUPERUSER
SELECT * FROM delete_data_node('data_node_1');
SELECT * FROM delete_data_node('data_node_2');
SELECT * FROM delete_data_node('data_node_3');
DROP DATABASE data_node_1;
DROP DATABASE data_node_2;
DROP DATABASE data_node_3;

-- Test TS execution on non-TSDB server
CREATE EXTENSION postgres_fdw;
CREATE SERVER myserver FOREIGN DATA WRAPPER postgres_fdw
    OPTIONS (host 'foo', dbname 'foodb', port '5432');
\set ON_ERROR_STOP 0
SELECT * FROM test.remote_exec('{myserver}', $$ SELECT 1; $$);
\set ON_ERROR_STOP 1
DROP SERVER myserver;
DROP EXTENSION postgres_fdw;

-- Test that transactional behaviour is the default and that it can be
-- disabled.
--
-- In this case, we only execute it on one data node since we are
-- creating a database and multiple creations of the database would
-- clash when executed on the same instace.
--
-- We prefix the database names with the test file to be able to
-- parallelize the test. Not possible right now because there are
-- other databases above that prevents this.
\c :TEST_DBNAME :ROLE_SUPERUSER
SELECT * FROM add_data_node('dist_commands_1', host => 'localhost',
                            database => 'dist_commands_1');
SELECT * FROM add_data_node('dist_commands_2', host => 'localhost',
                            database => 'dist_commands_2');
GRANT USAGE ON FOREIGN SERVER dist_commands_1, dist_commands_2 TO PUBLIC;

\set ON_ERROR_STOP 0
CALL distributed_exec('CREATE DATABASE dist_commands_magic',
       node_list => '{dist_commands_1}');
\set ON_ERROR_STOP 1
CALL distributed_exec('CREATE DATABASE dist_commands_magic',
       node_list => '{dist_commands_1}', transactional => FALSE);
DROP DATABASE dist_commands_magic;

-- Test that distributed_exec honor the 2PC behaviour when starting a
-- transaction locally. It should also give an error if attempting to
-- execute non-transactionally inside a local transaction.

-- To test that distributed_exec honors transactions, we create a
-- table on both data nodes, and then tweak one of the tables so that
-- we get a duplicate key when updating the table on both data
-- nodes. This should then abort the transaction on all data nodes.
\c :TEST_DBNAME :ROLE_1
CALL distributed_exec($$
  CREATE TABLE my_table (key INT, value TEXT, PRIMARY KEY (key));
$$);

\c dist_commands_1
INSERT INTO my_table VALUES (1, 'foo');

\c :TEST_DBNAME :ROLE_1
\set ON_ERROR_STOP 0
BEGIN;
CALL distributed_exec($$ INSERT INTO my_table VALUES (1, 'bar') $$);
COMMIT;
\set ON_ERROR_STOP 1

-- No changes should be there
SELECT * FROM test.remote_exec(NULL, $$ SELECT * FROM my_table; $$);

-- This should work.
BEGIN;
CALL distributed_exec($$ INSERT INTO my_table VALUES (2, 'bar'); $$);
COMMIT;

-- We should see changes
SELECT * FROM test.remote_exec(NULL, $$ SELECT * FROM my_table; $$);

-- This should fail since we are inside a transaction and asking for
-- transactional execution on the remote nodes. Non-transactional
-- execution should be outside transactions.
\set ON_ERROR_STOP 0
BEGIN;
CALL distributed_exec(
     $$ INSERT INTO my_table VALUES (3, 'baz') $$,
     transactional => FALSE
);
COMMIT;
\set ON_ERROR_STOP 1

-- We should see no changes
SELECT * FROM test.remote_exec(NULL, $$ SELECT * FROM my_table; $$);
