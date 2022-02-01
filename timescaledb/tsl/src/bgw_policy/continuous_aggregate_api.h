/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */

#ifndef TIMESCALEDB_TSL_BGW_POLICY_CAGG_API_H
#define TIMESCALEDB_TSL_BGW_POLICY_CAGG_API_H

#include <postgres.h>
#include <utils/jsonb.h>
#include "dimension.h"

extern Datum policy_refresh_cagg_add(PG_FUNCTION_ARGS);
extern Datum policy_refresh_cagg_proc(PG_FUNCTION_ARGS);
extern Datum policy_refresh_cagg_remove(PG_FUNCTION_ARGS);

int32 policy_continuous_aggregate_get_mat_hypertable_id(const Jsonb *config);
int64 policy_refresh_cagg_get_refresh_start(const Dimension *dim, const Jsonb *config);
int64 policy_refresh_cagg_get_refresh_end(const Dimension *dim, const Jsonb *config);
#endif /* TIMESCALEDB_TSL_BGW_POLICY_CAGG_API_H */
