/*
 * This file and its contents are licensed under the Apache License 2.0.
 * Please see the included NOTICE for copyright information and
 * LICENSE-APACHE for a copy of the license.
 */
#ifndef TIMESCALEDB_PLANNER_H
#define TIMESCALEDB_PLANNER_H

#include <postgres.h>
#include <nodes/pg_list.h>
#include <nodes/parsenodes.h>

#include "compat.h"
#include "export.h"

#if PG12_GE
#include <nodes/pathnodes.h>
#else
#include <nodes/relation.h>
#endif

typedef struct TsFdwRelationInfo TsFdwRelationInfo;
typedef struct TimescaleDBPrivate
{
	bool appends_ordered;
	/* attno of the time dimension in the parent table if appends are ordered */
	int order_attno;
	List *nested_oids;
	bool compressed;
	List *chunk_oids;
	List *serverids;
	Relids server_relids;
	TsFdwRelationInfo *fdw_relation_info;
} TimescaleDBPrivate;

extern TSDLLEXPORT bool ts_rte_is_hypertable(const RangeTblEntry *rte, bool *isdistributed);

static inline TimescaleDBPrivate *
ts_create_private_reloptinfo(RelOptInfo *rel)
{
	Assert(rel->fdw_private == NULL);
	rel->fdw_private = palloc0(sizeof(TimescaleDBPrivate));
	return rel->fdw_private;
}

static inline TimescaleDBPrivate *
ts_get_private_reloptinfo(const RelOptInfo *rel)
{
	return rel->fdw_private;
}

/*
 * TsRelType provides consistent classification of planned relations across
 * planner hooks.
 */
typedef enum TsRelType
{
	TS_REL_HYPERTABLE,		 /* A hypertable with no parent */
	TS_REL_CHUNK,			 /* Chunk with no parent (i.e., it's part of the
							  * plan as a standalone table. For example,
							  * querying the chunk directly and not via the
							  * parent hypertable). */
	TS_REL_HYPERTABLE_CHILD, /* Self child. With PostgreSQL's table expansion,
							  * the root table is expanded as a child of
							  * itself. This happens when our expansion code
							  * is turned off. */
	TS_REL_CHUNK_CHILD,		 /* Chunk with parent and the result of table
							  * expansion. */
	TS_REL_OTHER,			 /* Anything which is none of the above */
} TsRelType;

#endif /* TIMESCALEDB_PLANNER_H */
