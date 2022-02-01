-- This file and its contents are licensed under the Timescale License.
-- Please see the included NOTICE for copyright information and
-- LICENSE-TIMESCALE for a copy of the license.

\c :TEST_DBNAME :ROLE_SUPERUSER

CREATE OR REPLACE FUNCTION _timescaledb_internal.get_tabledef(tbl REGCLASS) RETURNS TEXT
AS :TSL_MODULE_PATHNAME, 'ts_test_get_tabledef' LANGUAGE C VOLATILE STRICT;

CREATE OR REPLACE FUNCTION tsl_test_deparse_drop_chunks(
    table_name REGCLASS,
    older_than "any" = NULL,
    newer_than "any" = NULL,
    verbose BOOLEAN = FALSE) RETURNS TEXT
AS :TSL_MODULE_PATHNAME, 'ts_test_deparse_drop_chunks' LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION tsl_test_deparse_scalar_func(
    schema_name NAME = NULL,
    table_name  NAME = NULL,
    "time" TIMESTAMPTZ = NOW(),
    message TEXT = NULL,
    not_set BOOLEAN = TRUE,
    option BOOLEAN = FALSE
    ) RETURNS BOOLEAN
AS :TSL_MODULE_PATHNAME, 'ts_test_deparse_func' LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION tsl_test_deparse_named_scalar_func(
    schema_name NAME = NULL,
    table_name  NAME = NULL,
    "time" TIMESTAMPTZ = NOW(),
    message TEXT = NULL,
    not_set BOOLEAN = TRUE,
    INOUT option BOOLEAN = FALSE
    )
AS :TSL_MODULE_PATHNAME, 'ts_test_deparse_func' LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION tsl_test_deparse_composite_func(
    schema_name NAME = NULL,
    table_name  NAME = NULL,
    "time" TIMESTAMPTZ = NOW(),
    message TEXT = NULL,
    not_set BOOLEAN = NULL,
    option BOOLEAN = FALSE
    ) RETURNS TABLE (success BOOLEAN, message TEXT)
AS :TSL_MODULE_PATHNAME, 'ts_test_deparse_func' LANGUAGE C VOLATILE;

SET ROLE :ROLE_DEFAULT_PERM_USER;
