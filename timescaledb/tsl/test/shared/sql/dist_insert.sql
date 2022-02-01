-- This file and its contents are licensed under the Timescale License.
-- Please see the included NOTICE for copyright information and
-- LICENSE-TIMESCALE for a copy of the license.

-- #168407735
-- Segfault when cancelling long running distributed insert

CREATE TABLE dist_insert (
  inserted TIMESTAMPTZ NOT NULL,
  partkey TEXT NOT NULL,
  value FLOAT
);

SELECT
  table_name
FROM
  create_distributed_hypertable('dist_insert', 'inserted', 'partkey');

INSERT INTO dist_insert
SELECT
  '2000-01-01'::TIMESTAMPTZ + format('%s hour', i)::INTERVAL,
  (i / 100)::TEXT,
  random()
FROM
  generate_series(1, 1000) AS sub(i);

SET statement_timeout TO '100ms';

\set ON_ERROR_STOP 0
INSERT INTO dist_insert
SELECT
  '2000-01-01'::TIMESTAMPTZ + format('%s min', i * 10)::INTERVAL,
  (i / 100)::TEXT,
  random()
FROM
  generate_series(1, 30000) AS sub(i);

\set ON_ERROR_STOP 1
RESET statement_timeout;

DROP TABLE dist_insert;
