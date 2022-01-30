/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */
#ifndef TIMESCALEDB_TSL_ASYNC_APPEND_H
#define TIMESCALEDB_TSL_ASYNC_APPEND_H

#include <postgres.h>
#include <nodes/execnodes.h>

#include "compat.h"
#if PG12_GE
#include <nodes/pathnodes.h>
#else
#include <nodes/relation.h>
#endif

typedef struct AsyncAppendPath
{
	CustomPath cpath;
} AsyncAppendPath;

/*
 * A wrapper node for any descendant node that AsyncAppend plan needs to interact with.
 * This node provides an async interface to underlying node.
 *
 * This node should not be confused with AsyncAppend plan state node
 */
typedef struct AsyncScanState
{
	CustomScanState css;
	void (*init)(struct AsyncScanState *state);
	void (*fetch_tuples)(struct AsyncScanState *state);
} AsyncScanState;

extern void async_append_add_paths(PlannerInfo *root, RelOptInfo *hyper_rel);

#endif
