/*
 * This file and its contents are licensed under the Apache License 2.0.
 * Please see the included NOTICE for copyright information and
 * LICENSE-APACHE for a copy of the license.
 */
#ifndef TIMESCALEDB_COMPAT_H
#define TIMESCALEDB_COMPAT_H

#include <postgres.h>
#include <commands/explain.h>
#include <commands/trigger.h>
#include <executor/executor.h>
#include <executor/tuptable.h>
#include <nodes/execnodes.h>
#include <nodes/nodes.h>
#include <pgstat.h>
#include <utils/lsyscache.h>
#include <utils/rel.h>

#include "export.h"

#define is_supported_pg_version_11(version) ((version >= 110000) && (version < 120000))
#define is_supported_pg_version_12(version) ((version >= 120000) && (version < 130000))

#define is_supported_pg_version(version)                                                           \
	(is_supported_pg_version_11(version) || is_supported_pg_version_12(version))

#define PG11 is_supported_pg_version_11(PG_VERSION_NUM)
#define PG12 is_supported_pg_version_12(PG_VERSION_NUM)

#define PG12_LT PG11
#define PG12_GE !(PG12_LT)

#if !(is_supported_pg_version(PG_VERSION_NUM))
#error "Unsupported PostgreSQL version"
#endif

/*
 * The following are compatibility functions for different versions of
 * PostgreSQL. Each compatibility function (or group) has its own logic for
 * which versions get different behavior and is separated from others by a
 * comment with its name and any clarifying notes about supported behavior. Each
 * compatibility define is separated out by function so that it is easier to see
 * what changed about its behavior, and at what version, but closely related
 * functions that changed at the same time may be grouped together into a single
 * block. Compatibility functions are organized in alphabetical order.
 *
 * Wherever reasonable, we try to achieve forwards compatibility so that we can
 * take advantage of features added in newer PG versions. This avoids some
 * future tech debt, though may not always be possible.
 *
 * We append "compat" to the name of the function or define if we change the behavior
 * of something that existed in a previous version. If we are merely backpatching
 * behavior from a later version to an earlier version and not changing the
 * behavior of the new version we simply adopt the new version's name.
 */

#if PG11
#define ExecInsertIndexTuplesCompat(slot, estate, no_dup_err, spec_conflict, arbiter_indexes)      \
	ExecInsertIndexTuples(slot,                                                                    \
						  &((slot)->tts_tuple->t_self),                                            \
						  estate,                                                                  \
						  no_dup_err,                                                              \
						  spec_conflict,                                                           \
						  arbiter_indexes)
#else
#define ExecInsertIndexTuplesCompat(slot, estate, no_dup_err, spec_conflict, arbiter_indexes)      \
	ExecInsertIndexTuples(slot, estate, no_dup_err, spec_conflict, arbiter_indexes);
#endif

/* ExecARInsertTriggers */
#if PG11
#define ExecARInsertTriggersCompat(estate, relinfo, slot, recheck_indexes, transition_capture)     \
	do                                                                                             \
	{                                                                                              \
		bool should_free;                                                                          \
		HeapTuple tuple = ExecFetchSlotHeapTuple(slot, true, &should_free);                        \
		ExecARInsertTriggers(estate, relinfo, tuple, recheck_indexes, transition_capture);         \
		if (should_free)                                                                           \
			heap_freetuple(tuple);                                                                 \
	} while (0);
#else
#define ExecARInsertTriggersCompat(estate,                                                         \
								   result_rel_info,                                                \
								   tuple,                                                          \
								   recheck_indexes,                                                \
								   transition_capture)                                             \
	ExecARInsertTriggers(estate, result_rel_info, tuple, recheck_indexes, transition_capture)
#endif

/* execute_attr_map_tuple */
#if PG11
#define execute_attr_map_tuple do_convert_tuple
#endif

#if PG11
#define TM_Result HTSU_Result

#define TM_Ok HeapTupleMayBeUpdated
#define TM_SelfModified HeapTupleSelfUpdated
#define TM_Updated HeapTupleUpdated
#define TM_BeingModified HeapTupleBeingUpdated
#define TM_WouldBlock HeapTupleWouldBlock
#define TM_Invisible HeapTupleInvisible

#define TM_FailureData HeapUpdateFailureData
#endif

#if PG11

#define TupleTableSlotOps void
#define TTSOpsVirtualP NULL
#define TTSOpsHeapTupleP NULL
#define TTSOpsMinimalTupleP NULL
#define TTSOpsBufferHeapTupleP NULL

#else

#define TTSOpsVirtualP (&TTSOpsVirtual)
#define TTSOpsHeapTupleP (&TTSOpsHeapTuple)
#define TTSOpsMinimalTupleP (&TTSOpsMinimalTuple)
#define TTSOpsBufferHeapTupleP (&TTSOpsBufferHeapTuple)

#endif

/*
 * ExecInitExtraTupleSlot & MakeTupleTableSlot
 *
 * PG11 introduced the ability to pass in a tupledesc and avoid having to
 * separately run ExecSetSlotDescriptor. Additionally, it added the ability to have a
 * fixed tupleDesc when making a TupleTableSlot, which can be useful during
 * execution for JITted operations. (See:
 * https://github.com/postgres/postgres/commit/ad7dbee368a7cd9e595d2a957be784326b08c943).
 * We adopt the PG11 conventions so that we can take advantage of JITing more easily in the future.
 */
#if PG11

#define ExecInitExtraTupleSlotCompat(estate, tupledesc, tts_ops)                                   \
	ExecInitExtraTupleSlot(estate, tupledesc)
#define MakeTupleTableSlotCompat(tupdesc, tts_ops) MakeTupleTableSlot(tupdesc)
#define MakeSingleTupleTableSlotCompat(tupdesc, tts_ops) MakeSingleTupleTableSlot(tupdesc)
#define ExecStoreHeapTupleCompat(tuple, slot, should_free)                                         \
	ExecStoreTuple(tuple, slot, InvalidBuffer, should_free)
#define ExecForceStoreHeapTupleCompat(tuple, slot, should_free)                                    \
	ExecStoreTuple(tuple, slot, InvalidBuffer, should_free)

#else /* PG12_GE */

#define ExecInitExtraTupleSlotCompat(estate, tupdesc, tts_ops)                                     \
	ExecInitExtraTupleSlot(estate, tupdesc, tts_ops)
#define MakeTupleTableSlotCompat(tupdesc, tts_ops) MakeTupleTableSlot(tupdesc, tts_ops)
#define MakeSingleTupleTableSlotCompat(tupdesc, tts_ops) MakeSingleTupleTableSlot(tupdesc, tts_ops)
#define ExecStoreHeapTupleCompat(tuple, slot, should_free)                                         \
	ExecStoreHeapTuple(tuple, slot, should_free)
#define ExecForceStoreHeapTupleCompat(tuple, slot, should_free)                                    \
	ExecForceStoreHeapTuple(tuple, slot, should_free)

#endif

/* fmgr
 * In a9c35cf postgres changed how it calls SQL functions so that the number of
 * argument-slots allocated is chosen dynamically, instead of being fixed. This
 * change was ABI-breaking, so we cannot backport this optimization, however,
 * we do backport the interface, so that all our code will be compatible with
 * new versions.
 */
#if PG11

/* unlike the pg12 version, this is just a wrapper for FunctionCallInfoData */
#define LOCAL_FCINFO(name, nargs)                                                                  \
	union                                                                                          \
	{                                                                                              \
		FunctionCallInfoData fcinfo;                                                               \
	} name##data;                                                                                  \
	FunctionCallInfo name = &name##data.fcinfo

#define SizeForFunctionCallInfo(nargs) sizeof(FunctionCallInfoData)

/* convenience macro to allocate FunctionCallInfoData on the heap */
#define HEAP_FCINFO(nargs) palloc(sizeof(FunctionCallInfoData))

/* getting arguments has a different API, so these macros unify the versions */
#define FC_ARG(fcinfo, n) ((fcinfo)->arg[(n)])
#define FC_NULL(fcinfo, n) ((fcinfo)->argnull[(n)])

#else

/* convenience macro to allocate FunctionCallInfoData on the heap */
#define HEAP_FCINFO(nargs) palloc(SizeForFunctionCallInfo(nargs))

/* getting arguments has a different API, so these macros unify the versions */
#define FC_ARG(fcinfo, n) ((fcinfo)->args[(n)].value)
#define FC_NULL(fcinfo, n) ((fcinfo)->args[(n)].isnull)

#endif

#define FC_FN_OID(fcinfo) ((fcinfo)->flinfo->fn_oid)

/* convenience setters */
#define FC_SET_ARG(fcinfo, n, val)                                                                 \
	do                                                                                             \
	{                                                                                              \
		short _n = (n);                                                                            \
		FunctionCallInfo _fcinfo = (fcinfo);                                                       \
		FC_ARG(_fcinfo, _n) = (val);                                                               \
		FC_NULL(_fcinfo, _n) = false;                                                              \
	} while (0)

#define FC_SET_NULL(fcinfo, n)                                                                     \
	do                                                                                             \
	{                                                                                              \
		short _n = (n);                                                                            \
		FunctionCallInfo _fcinfo = (fcinfo);                                                       \
		FC_ARG(_fcinfo, _n) = 0;                                                                   \
		FC_NULL(_fcinfo, _n) = true;                                                               \
	} while (0)

/*
 * In PG12 OID columns were removed changing all OID columns in the catalog to
 * be regular columns. This necessitates passing in the attnum of said column to
 * any function that wishes to access these columns. In earlier versions, this
 * parameter can be safely ignored.
 */
#if PG11
#define GetSysCacheOid1Compat(cacheid, oidcol, key1) GetSysCacheOid1(cacheid, key1)
#define GetSysCacheOid2Compat(cacheid, oidcol, key1, key2) GetSysCacheOid2(cacheid, key1, key2)
#define GetSysCacheOid3Compat(cacheid, oidcol, key1, key2, key3)                                   \
	GetSysCacheOid3(cacheid, key1, key2, key3)
#define GetSysCacheOid4Compat(cacheid, oidcol, key1, key2, key3, key4)                             \
	GetSysCacheOid4(cacheid, key1, key2, key3, key4)
#else
#define GetSysCacheOid1Compat(cacheid, oidcol, key1) GetSysCacheOid1(cacheid, oidcol, key1)
#define GetSysCacheOid2Compat(cacheid, oidcol, key1, key2)                                         \
	GetSysCacheOid2(cacheid, oidcol, key1, key2)
#define GetSysCacheOid3Compat(cacheid, oidcol, key1, key2, key3)                                   \
	GetSysCacheOid3(cacheid, oidcol, key1, key2, key3)
#define GetSysCacheOid4Compat(cacheid, oidcol, key1, key2, key3, key4)                             \
	GetSysCacheOid4(cacheid, oidcol, key1, key2, key3, key4)
#endif

/* RenameRelationInternal
 */
#if PG11
#define RenameRelationInternalCompat(relid, name, is_internal, is_index)                           \
	RenameRelationInternal(relid, name, is_internal)
#else
#define RenameRelationInternalCompat RenameRelationInternal
#endif

/* create this function for symmetry with pq_sendint32 */
#define pq_getmsgint32(buf) pq_getmsgint(buf, 4)

#if PG11
#define TUPLE_DESC_HAS_OIDS(desc) (desc)->tdhasoid
#else
#define TUPLE_DESC_HAS_OIDS(desc) false
#endif

/* Compatibility functions for table access method API introduced in PG12 */
#if PG11
#include "compat/tupconvert.h"
#include "compat/tuptable.h"
#include "compat/tableam.h"

#else
#define ts_tuptableslot_set_table_oid(slot, table_oid) (slot)->tts_tableOid = table_oid
#endif

#if PG11
#define ExecTypeFromTLCompat(tlist, hasoid) ExecTypeFromTL(tlist, hasoid)
#else
#define ExecTypeFromTLCompat(tlist, hasoid) ExecTypeFromTL(tlist)
#endif

#if PG11
#define create_append_path_compat(root,                                                            \
								  rel,                                                             \
								  subpaths,                                                        \
								  partial_subpaths,                                                \
								  pathkeys,                                                        \
								  required_outer,                                                  \
								  parallel_workers,                                                \
								  parallel_aware,                                                  \
								  partitioned_rels,                                                \
								  rows)                                                            \
	create_append_path(root,                                                                       \
					   rel,                                                                        \
					   subpaths,                                                                   \
					   partial_subpaths,                                                           \
					   required_outer,                                                             \
					   parallel_workers,                                                           \
					   parallel_aware,                                                             \
					   partitioned_rels,                                                           \
					   rows)

#else
#define create_append_path_compat create_append_path
#endif

/*
 * estimate_hashagg_tablesize is a static function in PG11 and earlier, so we map
 * to our own copy when it's not available.
 */
#if PG11
#define estimate_hashagg_tablesize(p, c, n) ts_estimate_hashagg_tablesize(p, c, n)
#endif

#include <commands/vacuum.h>
#include <commands/defrem.h>

static inline int
get_vacuum_options(const VacuumStmt *stmt)
{
#if PG12_GE
	/* In PG12, the vacuum options is a list of DefElems and require
	 * parsing. Here we only parse the options we might be interested in since
	 * PostgreSQL itself will parse the options fully when it executes the
	 * vacuum. */
	ListCell *lc;
	bool analyze = false;
	bool verbose = false;

	foreach (lc, stmt->options)
	{
		DefElem *opt = (DefElem *) lfirst(lc);

		/* Parse common options for VACUUM and ANALYZE */
		if (strcmp(opt->defname, "verbose") == 0)
			verbose = defGetBoolean(opt);
		/* Parse options available on VACUUM */
		else if (strcmp(opt->defname, "analyze") == 0)
			analyze = defGetBoolean(opt);
	}

	return (stmt->is_vacuumcmd ? VACOPT_VACUUM : VACOPT_ANALYZE) | (verbose ? VACOPT_VERBOSE : 0) |
		   (analyze ? VACOPT_ANALYZE : 0);
#else
	return stmt->options;
#endif
}

#endif /* TIMESCALEDB_COMPAT_H */
