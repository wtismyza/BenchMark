/*
 * This file and its contents are licensed under the Apache License 2.0.
 * Please see the included NOTICE for copyright information and
 * LICENSE-APACHE for a copy of the license.
 */

/*
 * This file handles commands on continuous aggs that should be allowed in
 * apache only mode. Right now this consists mostly of drop commands
 */

#include <postgres.h>
#include <access/htup_details.h>
#include <catalog/dependency.h>
#include <catalog/namespace.h>
#include <catalog/pg_trigger.h>
#include <commands/trigger.h>
#include <fmgr.h>
#include <storage/lmgr.h>
#include <utils/acl.h>
#include <utils/builtins.h>
#include <utils/date.h>
#include <utils/lsyscache.h>
#include <utils/timestamp.h>
#include <miscadmin.h>

#include "compat.h"

#include "bgw/job.h"
#include "continuous_agg.h"
#include "hypertable.h"
#include "scan_iterator.h"
#include "time_bucket.h"
#include "time_utils.h"
#include "catalog.h"

#define CHECK_NAME_MATCH(name1, name2) (namestrcmp(name1, name2) == 0)

static const WithClauseDefinition continuous_aggregate_with_clause_def[] = {
		[ContinuousEnabled] = {
			.arg_name = "continuous",
			.type_id = BOOLOID,
			.default_val = BoolGetDatum(false),
		},
		[ContinuousViewOptionCreateGroupIndex] = {
			.arg_name = "create_group_indexes",
			.type_id = BOOLOID,
			.default_val = BoolGetDatum(true),
		},
		[ContinuousViewOptionMaterializedOnly] = {
			.arg_name = "materialized_only",
			.type_id = BOOLOID,
			.default_val = BoolGetDatum(false),
		},
};

WithClauseResult *
ts_continuous_agg_with_clause_parse(const List *defelems)
{
	return ts_with_clauses_parse(defelems,
								 continuous_aggregate_with_clause_def,
								 TS_ARRAY_LEN(continuous_aggregate_with_clause_def));
}
static void
init_scan_by_mat_hypertable_id(ScanIterator *iterator, const int32 mat_hypertable_id)
{
	iterator->ctx.index = catalog_get_index(ts_catalog_get(), CONTINUOUS_AGG, CONTINUOUS_AGG_PKEY);

	ts_scan_iterator_scan_key_init(iterator,
								   Anum_continuous_agg_pkey_mat_hypertable_id,
								   BTEqualStrategyNumber,
								   F_INT4EQ,
								   Int32GetDatum(mat_hypertable_id));
}

static void
init_scan_by_raw_hypertable_id(ScanIterator *iterator, const int32 raw_hypertable_id)
{
	iterator->ctx.index =
		catalog_get_index(ts_catalog_get(), CONTINUOUS_AGG, CONTINUOUS_AGG_RAW_HYPERTABLE_ID_IDX);

	ts_scan_iterator_scan_key_init(iterator,
								   Anum_continuous_agg_raw_hypertable_id_idx_raw_hypertable_id,
								   BTEqualStrategyNumber,
								   F_INT4EQ,
								   Int32GetDatum(raw_hypertable_id));
}

static void
init_invalidation_threshold_scan_by_hypertable_id(ScanIterator *iterator,
												  const int32 raw_hypertable_id)
{
	iterator->ctx.index = catalog_get_index(ts_catalog_get(),
											CONTINUOUS_AGGS_INVALIDATION_THRESHOLD,
											CONTINUOUS_AGGS_INVALIDATION_THRESHOLD_PKEY);

	ts_scan_iterator_scan_key_init(iterator,
								   Anum_continuous_aggs_invalidation_threshold_pkey_hypertable_id,
								   BTEqualStrategyNumber,
								   F_INT4EQ,
								   Int32GetDatum(raw_hypertable_id));
}

static void
init_hypertable_invalidation_log_scan_by_hypertable_id(ScanIterator *iterator,
													   const int32 raw_hypertable_id)
{
	iterator->ctx.index = catalog_get_index(ts_catalog_get(),
											CONTINUOUS_AGGS_HYPERTABLE_INVALIDATION_LOG,
											CONTINUOUS_AGGS_HYPERTABLE_INVALIDATION_LOG_IDX);

	ts_scan_iterator_scan_key_init(
		iterator,
		Anum_continuous_aggs_hypertable_invalidation_log_idx_hypertable_id,
		BTEqualStrategyNumber,
		F_INT4EQ,
		Int32GetDatum(raw_hypertable_id));
}

static void
init_materialization_invalidation_log_scan_by_materialization_id(ScanIterator *iterator,
																 const int32 materialization_id)
{
	iterator->ctx.index = catalog_get_index(ts_catalog_get(),
											CONTINUOUS_AGGS_MATERIALIZATION_INVALIDATION_LOG,
											CONTINUOUS_AGGS_MATERIALIZATION_INVALIDATION_LOG_IDX);

	ts_scan_iterator_scan_key_init(
		iterator,
		Anum_continuous_aggs_materialization_invalidation_log_idx_materialization_id,
		BTEqualStrategyNumber,
		F_INT4EQ,
		Int32GetDatum(materialization_id));
}

static int32
number_of_continuous_aggs_attached(int32 raw_hypertable_id)
{
	ScanIterator iterator =
		ts_scan_iterator_create(CONTINUOUS_AGG, AccessShareLock, CurrentMemoryContext);
	int32 count = 0;

	init_scan_by_raw_hypertable_id(&iterator, raw_hypertable_id);
	ts_scanner_foreach(&iterator) { count++; }
	return count;
}

static void
invalidation_threshold_delete(int32 raw_hypertable_id)
{
	ScanIterator iterator = ts_scan_iterator_create(CONTINUOUS_AGGS_INVALIDATION_THRESHOLD,
													RowExclusiveLock,
													CurrentMemoryContext);

	init_invalidation_threshold_scan_by_hypertable_id(&iterator, raw_hypertable_id);

	ts_scanner_foreach(&iterator)
	{
		TupleInfo *ti = ts_scan_iterator_tuple_info(&iterator);
		ts_catalog_delete_tid(ti->scanrel, ts_scanner_get_tuple_tid(ti));
	}
}

static void
hypertable_invalidation_log_delete(int32 raw_hypertable_id)
{
	ScanIterator iterator = ts_scan_iterator_create(CONTINUOUS_AGGS_HYPERTABLE_INVALIDATION_LOG,
													RowExclusiveLock,
													CurrentMemoryContext);

	init_hypertable_invalidation_log_scan_by_hypertable_id(&iterator, raw_hypertable_id);

	ts_scanner_foreach(&iterator)
	{
		TupleInfo *ti = ts_scan_iterator_tuple_info(&iterator);
		ts_catalog_delete_tid(ti->scanrel, ts_scanner_get_tuple_tid(ti));
	}
}

static void
materialization_invalidation_log_delete(int32 materialization_id)
{
	ScanIterator iterator =
		ts_scan_iterator_create(CONTINUOUS_AGGS_MATERIALIZATION_INVALIDATION_LOG,
								RowExclusiveLock,
								CurrentMemoryContext);

	init_materialization_invalidation_log_scan_by_materialization_id(&iterator, materialization_id);

	ts_scanner_foreach(&iterator)
	{
		TupleInfo *ti = ts_scan_iterator_tuple_info(&iterator);
		ts_catalog_delete_tid(ti->scanrel, ts_scanner_get_tuple_tid(ti));
	}
}

static void
continuous_agg_init(ContinuousAgg *cagg, const Form_continuous_agg fd)
{
	Oid nspid = get_namespace_oid(NameStr(fd->user_view_schema), false);

	cagg->relid = get_relname_relid(NameStr(fd->user_view_name), nspid);
	memcpy(&cagg->data, fd, sizeof(cagg->data));
}

TSDLLEXPORT ContinuousAggHypertableStatus
ts_continuous_agg_hypertable_status(int32 hypertable_id)
{
	ScanIterator iterator =
		ts_scan_iterator_create(CONTINUOUS_AGG, AccessShareLock, CurrentMemoryContext);
	ContinuousAggHypertableStatus status = HypertableIsNotContinuousAgg;

	ts_scanner_foreach(&iterator)
	{
		bool should_free;
		HeapTuple tuple = ts_scan_iterator_fetch_heap_tuple(&iterator, false, &should_free);
		FormData_continuous_agg *data = (FormData_continuous_agg *) GETSTRUCT(tuple);

		if (data->raw_hypertable_id == hypertable_id)
			status |= HypertableIsRawTable;
		if (data->mat_hypertable_id == hypertable_id)
			status |= HypertableIsMaterialization;

		if (should_free)
			heap_freetuple(tuple);

		if (status == HypertableIsMaterializationAndRaw)
		{
			ts_scan_iterator_close(&iterator);
			return status;
		}
	}

	return status;
}

TSDLLEXPORT List *
ts_continuous_aggs_find_by_raw_table_id(int32 raw_hypertable_id)
{
	List *continuous_aggs = NIL;
	ScanIterator iterator =
		ts_scan_iterator_create(CONTINUOUS_AGG, AccessShareLock, CurrentMemoryContext);

	init_scan_by_raw_hypertable_id(&iterator, raw_hypertable_id);
	ts_scanner_foreach(&iterator)
	{
		ContinuousAgg *ca;
		bool should_free;
		HeapTuple tuple = ts_scan_iterator_fetch_heap_tuple(&iterator, false, &should_free);
		Form_continuous_agg data = (Form_continuous_agg) GETSTRUCT(tuple);
		MemoryContext oldmctx;

		oldmctx = MemoryContextSwitchTo(ts_scan_iterator_get_result_memory_context(&iterator));
		ca = palloc0(sizeof(*ca));
		continuous_agg_init(ca, data);
		continuous_aggs = lappend(continuous_aggs, ca);
		MemoryContextSwitchTo(oldmctx);

		if (should_free)
			heap_freetuple(tuple);
	}

	return continuous_aggs;
}

/* Find a continuous aggregate by the materialized hypertable id */
ContinuousAgg *
ts_continuous_agg_find_by_mat_hypertable_id(int32 mat_hypertable_id)
{
	ContinuousAgg *ca = NULL;
	ScanIterator iterator =
		ts_scan_iterator_create(CONTINUOUS_AGG, RowExclusiveLock, CurrentMemoryContext);

	init_scan_by_mat_hypertable_id(&iterator, mat_hypertable_id);
	ts_scanner_foreach(&iterator)
	{
		bool should_free;
		HeapTuple tuple = ts_scan_iterator_fetch_heap_tuple(&iterator, false, &should_free);
		Form_continuous_agg form = (Form_continuous_agg) GETSTRUCT(tuple);

		/* Note that this scan can only match at most once, so we assert on
		 * `ca` here. */
		Assert(ca == NULL);
		ca = ts_scan_iterator_alloc_result(&iterator, sizeof(*ca));
		continuous_agg_init(ca, form);

		Assert(ca && ca->data.mat_hypertable_id == mat_hypertable_id);

		if (should_free)
			heap_freetuple(tuple);
	}
	ts_scan_iterator_close(&iterator);
	return ca;
}

ContinuousAgg *
ts_continuous_agg_find_by_view_name(const char *schema, const char *name,
									ContinuousAggViewType type)
{
	ScanIterator iterator;
	ContinuousAgg *ca = NULL;
	AttrNumber view_name_attrnum = 0;
	AttrNumber schema_name_attrnum = 0;
	int count = 0;

	Assert(schema);
	Assert(name);

	switch (type)
	{
		case ContinuousAggUserView:
			schema_name_attrnum = Anum_continuous_agg_user_view_schema;
			view_name_attrnum = Anum_continuous_agg_user_view_name;
			break;
		case ContinuousAggPartialView:
			schema_name_attrnum = Anum_continuous_agg_partial_view_schema;
			view_name_attrnum = Anum_continuous_agg_partial_view_name;
			break;
		case ContinuousAggDirectView:
			schema_name_attrnum = Anum_continuous_agg_direct_view_schema;
			view_name_attrnum = Anum_continuous_agg_direct_view_name;
			break;
		case ContinuousAggAnyView:
			break;
	}

	iterator = ts_scan_iterator_create(CONTINUOUS_AGG, AccessShareLock, CurrentMemoryContext);

	if (type != ContinuousAggAnyView)
	{
		ts_scan_iterator_scan_key_init(&iterator,
									   schema_name_attrnum,
									   BTEqualStrategyNumber,
									   F_NAMEEQ,
									   CStringGetDatum(schema));
		ts_scan_iterator_scan_key_init(&iterator,
									   view_name_attrnum,
									   BTEqualStrategyNumber,
									   F_NAMEEQ,
									   CStringGetDatum(name));
	}

	ts_scanner_foreach(&iterator)
	{
		bool should_free;
		HeapTuple tuple = ts_scan_iterator_fetch_heap_tuple(&iterator, false, &should_free);
		FormData_continuous_agg *data = (FormData_continuous_agg *) GETSTRUCT(tuple);
		ContinuousAggViewType vtype = type;

		if (vtype == ContinuousAggAnyView)
			vtype = ts_continuous_agg_view_type(data, schema, name);

		if (vtype != ContinuousAggAnyView)
		{
			ca = ts_scan_iterator_alloc_result(&iterator, sizeof(*ca));
			continuous_agg_init(ca, data);
			count++;
		}

		if (should_free)
			heap_freetuple(tuple);
	}

	Assert(count <= 1);

	return ca;
}

ContinuousAgg *
ts_continuous_agg_find_userview_name(const char *schema, const char *name)
{
	return ts_continuous_agg_find_by_view_name(schema, name, ContinuousAggUserView);
}

/*
 * Find a continuous agg object by the main relid.
 *
 * The relid is the user-facing object ID that represents the continuous
 * aggregate (i.e., the query view's ID).
 */
ContinuousAgg *
ts_continuous_agg_find_by_relid(Oid relid)
{
	const char *relname = get_rel_name(relid);
	const char *schemaname = get_namespace_name(get_rel_namespace(relid));

	if (NULL == relname || NULL == schemaname)
		return NULL;

	return ts_continuous_agg_find_userview_name(schemaname, relname);
}

/*
 * Find a continuous aggregate by range var.
 */
ContinuousAgg *
ts_continuous_agg_find_by_rv(const RangeVar *rv)
{
	Oid relid;
	if (rv == NULL)
		return NULL;
	relid = RangeVarGetRelid(rv, NoLock, true);
	if (!OidIsValid(relid))
		return NULL;
	return ts_continuous_agg_find_by_relid(relid);
}

/*
 * Drops continuous aggs and all related objects.
 *
 * These objects are: the user view itself, the catalog entry in
 * continuous-agg , the partial view,
 * the materialization hypertable,
 * trigger on the raw hypertable (hypertable specified in the user view )
 * copy of the user view query (aka the direct view)
 * NOTE: The order in which the objects are dropped should be EXACTLY the same as in materialize.c"
 *
 * drop_user_view indicates whether to drop the user view.
 *                (should be false if called as part of the drop-user-view callback)
 */
static void
drop_continuous_agg(ContinuousAgg *agg, bool drop_user_view)
{
	ScanIterator iterator =
		ts_scan_iterator_create(CONTINUOUS_AGG, RowExclusiveLock, CurrentMemoryContext);
	Catalog *catalog = ts_catalog_get();
	ObjectAddress user_view = { .objectId = InvalidOid }, partial_view = { .objectId = InvalidOid },
				  rawht_trig = { .objectId = InvalidOid }, direct_view = { .objectId = InvalidOid };
	Hypertable *mat_hypertable, *raw_hypertable;
	int32 count = 0;
	bool raw_hypertable_has_other_caggs = true;
	bool raw_hypertable_exists;

	/* NOTE: the lock order matters, see tsl/src/materialization.c. Perform all locking upfront */

	/* delete the job before taking locks as it kills long-running jobs which we would otherwise
	 * wait on */
	List *jobs = ts_bgw_job_find_by_hypertable_id(agg->data.mat_hypertable_id);
	ListCell *lc;

	foreach (lc, jobs)
	{
		BgwJob *job = lfirst(lc);
		ts_bgw_job_delete_by_id(job->fd.id);
	}

	user_view = (ObjectAddress){
		.classId = RelationRelationId,
		.objectId =
			get_relname_relid(NameStr(agg->data.user_view_name),
							  get_namespace_oid(NameStr(agg->data.user_view_schema), false)),
	};
	/* The partial view may already be dropped by PG's dependency system (e.g. the raw table was
	 * dropped) */
	if (OidIsValid(user_view.objectId))
		LockRelationOid(user_view.objectId, AccessExclusiveLock);

	raw_hypertable = ts_hypertable_get_by_id(agg->data.raw_hypertable_id);
	/* The raw hypertable might be already dropped if this is a cascade from that drop */
	raw_hypertable_exists =
		(raw_hypertable != NULL && OidIsValid(raw_hypertable->main_table_relid));
	if (raw_hypertable_exists)
		/* AccessExclusiveLock is needed to drop triggers.
		 * Also prevent concurrent DML commands */
		LockRelationOid(raw_hypertable->main_table_relid, AccessExclusiveLock);
	mat_hypertable = ts_hypertable_get_by_id(agg->data.mat_hypertable_id);
	/* AccessExclusiveLock is needed to drop this table. */
	LockRelationOid(mat_hypertable->main_table_relid, AccessExclusiveLock);

	/* lock catalogs */
	LockRelationOid(catalog_get_table_id(catalog, BGW_JOB), RowExclusiveLock);
	LockRelationOid(catalog_get_table_id(catalog, CONTINUOUS_AGG), RowExclusiveLock);
	raw_hypertable_has_other_caggs =
		raw_hypertable_exists && number_of_continuous_aggs_attached(raw_hypertable->fd.id) > 1;
	if (!raw_hypertable_has_other_caggs)
		LockRelationOid(catalog_get_table_id(catalog, CONTINUOUS_AGGS_HYPERTABLE_INVALIDATION_LOG),
						RowExclusiveLock);
	if (!raw_hypertable_has_other_caggs)
		LockRelationOid(catalog_get_table_id(catalog, CONTINUOUS_AGGS_INVALIDATION_THRESHOLD),
						RowExclusiveLock);

	/* The trigger will be dropped if the hypertable still exists and no other caggs attached */
	if (!raw_hypertable_has_other_caggs && raw_hypertable_exists)
	{
		Oid rawht_trigoid =
			get_trigger_oid(raw_hypertable->main_table_relid, CAGGINVAL_TRIGGER_NAME, false);
		rawht_trig = (ObjectAddress){ .classId = TriggerRelationId,
									  .objectId = rawht_trigoid,
									  .objectSubId = 0 };
		/* raw hypertable is locked above */
		LockRelationOid(rawht_trigoid, AccessExclusiveLock);
	}

	partial_view = (ObjectAddress){
		.classId = RelationRelationId,
		.objectId =
			get_relname_relid(NameStr(agg->data.partial_view_name),
							  get_namespace_oid(NameStr(agg->data.partial_view_schema), false)),
	};
	/* The partial view may already be dropped by PG's dependency system (e.g. the raw table was
	 * dropped) */
	if (OidIsValid(partial_view.objectId))
		LockRelationOid(partial_view.objectId, AccessExclusiveLock);

	direct_view = (ObjectAddress){
		.classId = RelationRelationId,
		.objectId =
			get_relname_relid(NameStr(agg->data.direct_view_name),
							  get_namespace_oid(NameStr(agg->data.direct_view_schema), false)),
	};
	if (OidIsValid(direct_view.objectId))
		LockRelationOid(direct_view.objectId, AccessExclusiveLock);

	/*  END OF LOCKING. Perform actual deletions now. */

	if (OidIsValid(user_view.objectId))
		performDeletion(&user_view, DROP_RESTRICT, 0);

	/* Delete catalog entry. */
	init_scan_by_mat_hypertable_id(&iterator, agg->data.mat_hypertable_id);
	ts_scanner_foreach(&iterator)
	{
		TupleInfo *ti = ts_scan_iterator_tuple_info(&iterator);
		bool should_free;
		HeapTuple tuple = ts_scan_iterator_fetch_heap_tuple(&iterator, false, &should_free);
		Form_continuous_agg form = (Form_continuous_agg) GETSTRUCT(tuple);

		ts_catalog_delete_tid(ti->scanrel, ts_scanner_get_tuple_tid(ti));

		/* delete all related rows */
		if (!raw_hypertable_has_other_caggs)
			hypertable_invalidation_log_delete(form->raw_hypertable_id);

		if (!raw_hypertable_has_other_caggs)
			invalidation_threshold_delete(form->raw_hypertable_id);
		materialization_invalidation_log_delete(form->mat_hypertable_id);
		count++;

		if (should_free)
			heap_freetuple(tuple);
	}
	Assert(count == 1);

	if (OidIsValid(rawht_trig.objectId))
		ts_hypertable_drop_trigger(raw_hypertable, CAGGINVAL_TRIGGER_NAME);

	/* delete the materialization table */
	ts_hypertable_drop(mat_hypertable, DROP_CASCADE);

	if (OidIsValid(partial_view.objectId))
		performDeletion(&partial_view, DROP_RESTRICT, 0);
	if (OidIsValid(direct_view.objectId))
		performDeletion(&direct_view, DROP_RESTRICT, 0);
}

/*
 * This is a called when a hypertable gets dropped.
 *
 * If the hypertable is a raw hypertable for a continuous agg,
 * drop the continuous agg.
 *
 * If the hypertable is a materialization hypertable, error out
 * and force the user to drop the continuous agg instead.
 */
void
ts_continuous_agg_drop_hypertable_callback(int32 hypertable_id)
{
	ScanIterator iterator =
		ts_scan_iterator_create(CONTINUOUS_AGG, AccessShareLock, CurrentMemoryContext);
	ContinuousAgg ca;

	ts_scanner_foreach(&iterator)
	{
		bool should_free;
		HeapTuple tuple = ts_scan_iterator_fetch_heap_tuple(&iterator, false, &should_free);
		FormData_continuous_agg *data = (FormData_continuous_agg *) GETSTRUCT(tuple);

		if (data->raw_hypertable_id == hypertable_id)
		{
			continuous_agg_init(&ca, data);
			drop_continuous_agg(&ca, true);
		}
		if (data->mat_hypertable_id == hypertable_id)
			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
					 errmsg("cannot drop the materialized table because it is required by a "
							"continuous aggregate")));

		if (should_free)
			heap_freetuple(tuple);
	}
}

/* Block dropping the partial and direct view if the continuous aggregate still exists */
static void
drop_internal_view(ContinuousAgg *agg)
{
	ScanIterator iterator =
		ts_scan_iterator_create(CONTINUOUS_AGG, AccessShareLock, CurrentMemoryContext);
	int count = 0;
	init_scan_by_mat_hypertable_id(&iterator, agg->data.mat_hypertable_id);
	ts_scanner_foreach(&iterator)
	{
		TupleInfo *ti = ts_scan_iterator_tuple_info(&iterator);
		ts_catalog_delete_tid(ti->scanrel, ts_scanner_get_tuple_tid(ti));
		count++;
	}
	if (count > 0)
		ereport(ERROR,
				(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
				 errmsg(
					 "cannot drop the partial/direct view because it is required by a continuous "
					 "aggregate")));
}

/* This gets called when a view gets dropped. */
void
ts_continuous_agg_drop_view_callback(ContinuousAgg *ca, const char *schema, const char *name)
{
	ContinuousAggViewType vtyp;
	vtyp = ts_continuous_agg_view_type(&ca->data, schema, name);
	switch (vtyp)
	{
		case ContinuousAggUserView:
			drop_continuous_agg(ca, false /* The user view has already been dropped */);
			break;
		case ContinuousAggPartialView:
		case ContinuousAggDirectView:
			drop_internal_view(ca);
			break;
		default:
			elog(ERROR, "unknown continuous aggregate view type");
	}
}

static inline bool
ts_continuous_agg_is_user_view_schema(FormData_continuous_agg *data, const char *schema)
{
	return CHECK_NAME_MATCH(&data->user_view_schema, schema);
}

static inline bool
ts_continuous_agg_is_partial_view_schema(FormData_continuous_agg *data, const char *schema)
{
	return CHECK_NAME_MATCH(&data->partial_view_schema, schema);
}

static inline bool
ts_continuous_agg_is_direct_view_schema(FormData_continuous_agg *data, const char *schema)
{
	return CHECK_NAME_MATCH(&data->direct_view_schema, schema);
}

ContinuousAggViewType
ts_continuous_agg_view_type(FormData_continuous_agg *data, const char *schema, const char *name)
{
	if (CHECK_NAME_MATCH(&data->user_view_schema, schema) &&
		CHECK_NAME_MATCH(&data->user_view_name, name))
		return ContinuousAggUserView;
	else if (CHECK_NAME_MATCH(&data->partial_view_schema, schema) &&
			 CHECK_NAME_MATCH(&data->partial_view_name, name))
		return ContinuousAggPartialView;
	else if (CHECK_NAME_MATCH(&data->direct_view_schema, schema) &&
			 CHECK_NAME_MATCH(&data->direct_view_name, name))
		return ContinuousAggDirectView;
	else
		return ContinuousAggAnyView;
}

static FormData_continuous_agg *
ensure_new_tuple(HeapTuple old_tuple, HeapTuple *new_tuple)
{
	if (*new_tuple == NULL)
		*new_tuple = heap_copytuple(old_tuple);

	return (FormData_continuous_agg *) GETSTRUCT(*new_tuple);
}

void
ts_continuous_agg_rename_schema_name(char *old_schema, char *new_schema)
{
	ScanIterator iterator =
		ts_scan_iterator_create(CONTINUOUS_AGG, RowExclusiveLock, CurrentMemoryContext);

	ts_scanner_foreach(&iterator)
	{
		TupleInfo *tinfo = ts_scan_iterator_tuple_info(&iterator);
		bool should_free;
		HeapTuple tuple = ts_scan_iterator_fetch_heap_tuple(&iterator, false, &should_free);
		FormData_continuous_agg *data = (FormData_continuous_agg *) GETSTRUCT(tuple);
		HeapTuple new_tuple = NULL;

		if (ts_continuous_agg_is_user_view_schema(data, old_schema))
		{
			FormData_continuous_agg *new_data = ensure_new_tuple(tuple, &new_tuple);
			namestrcpy(&new_data->user_view_schema, new_schema);
		}

		if (ts_continuous_agg_is_partial_view_schema(data, old_schema))
		{
			FormData_continuous_agg *new_data = ensure_new_tuple(tuple, &new_tuple);
			namestrcpy(&new_data->partial_view_schema, new_schema);
		}

		if (ts_continuous_agg_is_direct_view_schema(data, old_schema))
		{
			FormData_continuous_agg *new_data = ensure_new_tuple(tuple, &new_tuple);
			namestrcpy(&new_data->direct_view_schema, new_schema);
		}

		if (new_tuple != NULL)
		{
			ts_catalog_update(tinfo->scanrel, new_tuple);
			heap_freetuple(new_tuple);
		}

		if (should_free)
			heap_freetuple(tuple);
	}
	return;
}

extern void
ts_continuous_agg_rename_view(const char *old_schema, const char *name, const char *new_schema,
							  const char *new_name, ObjectType *object_type)
{
	ScanIterator iterator =
		ts_scan_iterator_create(CONTINUOUS_AGG, RowExclusiveLock, CurrentMemoryContext);

	Assert(object_type);

	ts_scanner_foreach(&iterator)
	{
		TupleInfo *tinfo = ts_scan_iterator_tuple_info(&iterator);
		bool should_free;
		HeapTuple tuple = ts_scan_iterator_fetch_heap_tuple(&iterator, false, &should_free);
		FormData_continuous_agg *data = (FormData_continuous_agg *) GETSTRUCT(tuple);
		HeapTuple new_tuple = NULL;
		ContinuousAggViewType vtyp = ts_continuous_agg_view_type(data, old_schema, name);

		switch (vtyp)
		{
			case ContinuousAggUserView:
			{
				FormData_continuous_agg *new_data;

				if (*object_type == OBJECT_VIEW)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot alter continuous aggregate using ALTER VIEW"),
							 errhint(
								 "Use ALTER MATERIALIZED VIEW to alter a continuous aggregate.")));

				Assert(*object_type == OBJECT_MATVIEW);
				*object_type = OBJECT_VIEW;

				new_data = ensure_new_tuple(tuple, &new_tuple);
				namestrcpy(&new_data->user_view_schema, new_schema);
				namestrcpy(&new_data->user_view_name, new_name);
				break;
			}
			case ContinuousAggPartialView:
			{
				FormData_continuous_agg *new_data = ensure_new_tuple(tuple, &new_tuple);
				namestrcpy(&new_data->partial_view_schema, new_schema);
				namestrcpy(&new_data->partial_view_name, new_name);
				break;
			}
			case ContinuousAggDirectView:
			{
				FormData_continuous_agg *new_data = ensure_new_tuple(tuple, &new_tuple);
				namestrcpy(&new_data->direct_view_schema, new_schema);
				namestrcpy(&new_data->direct_view_name, new_name);
				break;
			}
			default:
				break;
		}

		if (new_tuple != NULL)
		{
			ts_catalog_update(tinfo->scanrel, new_tuple);
			heap_freetuple(new_tuple);
		}

		if (should_free)
			heap_freetuple(tuple);
	}
	return;
}

TSDLLEXPORT int32
ts_number_of_continuous_aggs()
{
	int32 count = 0;
	ScanIterator iterator =
		ts_scan_iterator_create(CONTINUOUS_AGG, AccessShareLock, CurrentMemoryContext);
	ts_scanner_foreach(&iterator) { count++; }

	return count;
}

Oid
ts_continuous_agg_get_user_view_oid(ContinuousAgg *agg)
{
	Oid view_relid =
		get_relname_relid(NameStr(agg->data.user_view_name),
						  get_namespace_oid(NameStr(agg->data.user_view_schema), false));
	if (!OidIsValid(view_relid))
		elog(ERROR, "could not find user view for continuous agg");
	return view_relid;
}

static int32
find_raw_hypertable_for_materialization(int32 mat_hypertable_id)
{
	short count = 0;
	int32 htid = INVALID_HYPERTABLE_ID;
	ScanIterator iterator =
		ts_scan_iterator_create(CONTINUOUS_AGG, RowExclusiveLock, CurrentMemoryContext);

	init_scan_by_mat_hypertable_id(&iterator, mat_hypertable_id);
	ts_scanner_foreach(&iterator)
	{
		bool isnull;
		Datum datum = slot_getattr(ts_scan_iterator_slot(&iterator),
								   Anum_continuous_agg_raw_hypertable_id,
								   &isnull);

		Assert(!isnull);
		htid = DatumGetInt32(datum);
		count++;
	}
	Assert(count <= 1);
	ts_scan_iterator_close(&iterator);
	return htid;
}

/* Continuous aggregate materialization hypertables inherit integer_now func
 * from the raw hypertable (unless it was explicitly reset for cont. aggregate.
 * Walk the materialization hypertable ->raw hypertable tree till
 * we find a hypertable that has integer_now_func set.
 */
TSDLLEXPORT Dimension *
ts_continuous_agg_find_integer_now_func_by_materialization_id(int32 mat_htid)
{
	int32 raw_htid = mat_htid;
	Dimension *par_dim = NULL;
	while (raw_htid != INVALID_HYPERTABLE_ID)
	{
		Hypertable *raw_ht = ts_hypertable_get_by_id(raw_htid);
		Dimension *open_dim = hyperspace_get_open_dimension(raw_ht->space, 0);
		if (strlen(NameStr(open_dim->fd.integer_now_func)) != 0 &&
			strlen(NameStr(open_dim->fd.integer_now_func_schema)) != 0)
		{
			par_dim = open_dim;
			break;
		}
		mat_htid = raw_htid;
		raw_htid = find_raw_hypertable_for_materialization(mat_htid);
	}
	return par_dim;
}

TS_FUNCTION_INFO_V1(ts_continuous_agg_watermark);

/*
 * Get the watermark for a real-time aggregation query on a continuous
 * aggregate.
 *
 * The watermark determines where the materialization ends for a continuous
 * aggregate. It is used by real-time aggregation as the threshold between the
 * materialized data and real-time data in the UNION query.
 *
 * The watermark is defined as the end of the last (highest) bucket in the
 * materialized hypertable of a continuous aggregate.
 *
 * The materialized hypertable ID is given as input argument.
 */
Datum
ts_continuous_agg_watermark(PG_FUNCTION_ARGS)
{
	const int32 hyper_id = PG_GETARG_INT32(0);
	ContinuousAgg *cagg;
	Hypertable *ht;
	Dimension *dim;
	Datum maxdat;
	bool max_isnull;
	int64 watermark;
	Oid timetype;
	AclResult aclresult;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid continuous aggregate hypertable")));

	cagg = ts_continuous_agg_find_by_mat_hypertable_id(hyper_id);

	if (NULL == cagg)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%d is not a materialized hypertable", hyper_id)));

	/* Preemptive permission check to ensure the function complains about lack
	 * of permissions on the cagg rather than the materialized hypertable */
	aclresult = pg_class_aclcheck(cagg->relid, GetUserId(), ACL_SELECT);
	aclcheck_error(aclresult, OBJECT_MATVIEW, get_rel_name(cagg->relid));

	ht = ts_hypertable_get_by_id(hyper_id);
	Assert(NULL != ht);
	dim = hyperspace_get_open_dimension(ht->space, 0);
	timetype = ts_dimension_get_partition_type(dim);
	maxdat = ts_hypertable_get_open_dim_max_value(ht, 0, &max_isnull);

	if (!max_isnull)
	{
		/* Add one bucket to get to the end of the last bucket */
		watermark = ts_time_value_to_internal(maxdat, timetype);
		watermark = ts_time_saturating_add(watermark, cagg->data.bucket_width, timetype);
	}
	else
	{
		/* Nothing materialized, so return min */
		watermark = ts_time_get_min(timetype);
	}

	PG_RETURN_INT64(watermark);
}
