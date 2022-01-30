/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */
#ifndef TIMESCALEDB_TSL_REMOTE_STMT_PARAMS_H
#define TIMESCALEDB_TSL_REMOTE_STMT_PARAMS_H

#include <postgres.h>
#include <fmgr.h>
#include <nodes/pg_list.h>
#include <executor/tuptable.h>

#include "data_format.h"

typedef struct StmtParams StmtParams;

extern int stmt_params_validate_num_tuples(int num_params, int num_tuples);
extern StmtParams *stmt_params_create(List *target_attr_nums, bool ctid, TupleDesc tuple_desc,
									  int num_tuples);
extern StmtParams *stmt_params_create_from_values(const char **param_values, int n_params);
extern void stmt_params_convert_values(StmtParams *params, TupleTableSlot *slot,
									   ItemPointer tupleid);
extern const int *stmt_params_formats(StmtParams *stmt_params);
extern const int *stmt_params_lengths(StmtParams *stmt_params);
extern const char *const *stmt_params_values(StmtParams *stmt_params);
extern const int stmt_params_num_params(StmtParams *stmt_params);
extern void stmt_params_reset(StmtParams *params);
extern void stmt_params_free(StmtParams *params);
extern const int stmt_params_total_values(StmtParams *stmt_params);
extern const int stmt_params_converted_tuples(StmtParams *stmt_params);

#endif
