-- This file and its contents are licensed under the Timescale License.
-- Please see the included NOTICE for copyright information and
-- LICENSE-TIMESCALE for a copy of the license.

CREATE SCHEMA IF NOT EXISTS test;
GRANT USAGE ON SCHEMA test TO PUBLIC;

CREATE OR REPLACE FUNCTION test.execute_sql_and_filter_data_node_name_on_error(cmd TEXT, data_node_prefix TEXT DEFAULT 'data_node')
RETURNS VOID LANGUAGE PLPGSQL AS $BODY$
DECLARE
  original_error_text TEXT;
  error_text TEXT;
BEGIN
  EXECUTE cmd;
EXCEPTION
  WHEN others THEN
     GET STACKED DIAGNOSTICS original_error_text = MESSAGE_TEXT;
     SELECT regexp_replace(original_error_text, '\[' || data_node_prefix ||'_.+\]', '[' || data_node_prefix || '_x]', 'g') INTO error_text;
     RAISE '%', error_text;
END
$BODY$;
