-- This file and its contents are licensed under the Timescale License.
-- Please see the included NOTICE for copyright information and
-- LICENSE-TIMESCALE for a copy of the license.

\c :TEST_DBNAME :ROLE_SUPERUSER

CREATE FUNCTION _timescaledb_internal.test_remote_connection_cache()
RETURNS void
AS :TSL_MODULE_PATHNAME, 'ts_test_remote_connection_cache'
LANGUAGE C STRICT;

CREATE FUNCTION _timescaledb_internal.test_alter_data_node(node_name NAME)
RETURNS BOOL
AS :TSL_MODULE_PATHNAME, 'ts_test_alter_data_node'
LANGUAGE C STRICT;

DO $d$
    BEGIN
        EXECUTE $$SELECT add_data_node('loopback_1', host => 'localhost',
                database => 'loopback_1',
                port => current_setting('port')::int)$$;
        EXECUTE $$SELECT add_data_node('loopback_2', host => 'localhost',
                database => 'loopback_2',
                port => current_setting('port')::int)$$;
    END;
$d$;

SELECT _timescaledb_internal.test_remote_connection_cache();
