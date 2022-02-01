-- This file and its contents are licensed under the Apache License 2.0.
-- Please see the included NOTICE for copyright information and
-- LICENSE-APACHE for a copy of the license.

\c :TEST_DBNAME :ROLE_SUPERUSER
CREATE OR REPLACE FUNCTION test.condition() RETURNS VOID
    AS :MODULE_PATHNAME, 'ts_test_utils_condition' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE OR REPLACE FUNCTION test.int64_eq() RETURNS VOID
    AS :MODULE_PATHNAME, 'ts_test_utils_int64_eq' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE OR REPLACE FUNCTION test.ptr_eq() RETURNS VOID
    AS :MODULE_PATHNAME, 'ts_test_utils_ptr_eq' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
CREATE OR REPLACE FUNCTION test.double_eq() RETURNS VOID
    AS :MODULE_PATHNAME, 'ts_test_utils_double_eq' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
SET ROLE :ROLE_DEFAULT_PERM_USER;

-- We're testing that the test utils work and generate errors on
-- failing conditions
\set ON_ERROR_STOP 0
SELECT test.condition();
SELECT test.int64_eq();
SELECT test.ptr_eq();
SELECT test.double_eq();
\set ON_ERROR_STOP 1
