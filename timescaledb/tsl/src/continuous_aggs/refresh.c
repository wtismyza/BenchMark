/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */
#include <postgres.h>
#include <utils/lsyscache.h>
#include <utils/fmgrprotos.h>
#include <utils/snapmgr.h>
#include <utils/guc.h>
#include <access/xact.h>
#include <storage/lmgr.h>
#include <miscadmin.h>
#include <fmgr.h>

#include <catalog.h>
#include <continuous_agg.h>
#include <dimension.h>
#include <hypertable.h>
#include <hypertable_cache.h>
#include <time_bucket.h>
#include <time_utils.h>
#include <utils.h>

#include "refresh.h"
#include "materialize.h"
#include "invalidation.h"
#include "invalidation_threshold.h"

typedef struct CaggRefreshState
{
	ContinuousAgg cagg;
	Hypertable *cagg_ht;
	InternalTimeRange refresh_window;
	SchemaAndName partial_view;
} CaggRefreshState;

static Hypertable *
cagg_get_hypertable_or_fail(int32 hypertable_id)
{
	Hypertable *ht = ts_hypertable_get_by_id(hypertable_id);

	if (NULL == ht)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("invalid continuous aggregate state"),
				 errdetail("A continuous aggregate references a hypertable that does not exist.")));

	return ht;
}

/*
 * Compute the largest possible bucketed window given the time type and
 * internal restrictions.
 *
 * The largest bucketed window is governed by restrictions set by the type and
 * internal, TimescaleDB-specific legacy details (see get_max_window above for
 * further explanation).
 */
static InternalTimeRange
get_largest_bucketed_window(Oid timetype, int64 bucket_width)
{
	InternalTimeRange maxwindow = {
		.type = timetype,
		.start = ts_time_get_min(timetype),
		.end = ts_time_get_end_or_max(timetype),
	};
	InternalTimeRange maxbuckets;

	/* For the MIN value, the corresponding bucket either falls on the exact
	 * MIN or it will be below it. Therefore, we add (bucket_width - 1) to
	 * move to the next bucket to be within the allowed range. */
	maxwindow.start = ts_time_saturating_add(maxwindow.start, bucket_width - 1, timetype);
	maxbuckets.start = ts_time_bucket_by_type(bucket_width, maxwindow.start, timetype);
	maxbuckets.end = ts_time_get_end_or_max(timetype);

	return maxbuckets;
}

/*
 * Adjust the refresh window to align with inscribed buckets, so it includes buckets, which are
 * fully covered by the refresh window.
 *
 * Bucketing refresh window is necessary for a continuous aggregate refresh, which can refresh only
 * entire buckets. The result of the function is a bucketed window, where its start is at the start
 * of the first bucket, which is  fully inside the refresh window, and its end is at the end of the
 * last fully covered bucket.
 *
 * Example1, the window needs to shrink:
 *    [---------)      - given refresh window
 * .|....|....|....|.  - buckets
 *       [----)        - inscribed bucketed window
 *
 * Example2, the window is already aligned:
 *       [----)        - given refresh window
 * .|....|....|....|.  - buckets
 *       [----)        - inscribed bucketed window
 *
 * This function is called for the continuous aggregate policy and manual refresh. In such case
 * excluding buckets, which are not fully covered by the refresh window, avoids refreshing a bucket,
 * where part of its data were dropped by a retention policy. See #2198 for details.
 */
static InternalTimeRange
compute_inscribed_bucketed_refresh_window(const InternalTimeRange *const refresh_window,
										  const int64 bucket_width)
{
	InternalTimeRange result = *refresh_window;
	InternalTimeRange largest_bucketed_window =
		get_largest_bucketed_window(refresh_window->type, bucket_width);

	if (refresh_window->start <= largest_bucketed_window.start)
	{
		result.start = largest_bucketed_window.start;
	}
	else
	{
		/* The start time needs to be aligned with the first fully enclosed bucket.
		 * So the original window start is moved to next bucket, except if the start is
		 * already aligned with a bucket, thus 1 is subtracted to avoid moving into next
		 * bucket in the aligned case. */
		int64 included_bucket =
			ts_time_saturating_add(refresh_window->start, bucket_width - 1, refresh_window->type);
		/* Get the start of the included bucket. */
		result.start = ts_time_bucket_by_type(bucket_width, included_bucket, refresh_window->type);
	}

	if (refresh_window->end >= largest_bucketed_window.end)
	{
		result.end = largest_bucketed_window.end;
	}
	else
	{
		/* The window is reduced to the beginning of the bucket, which contains the exclusive
		 * end of the refresh window. */
		result.end =
			ts_time_bucket_by_type(bucket_width, refresh_window->end, refresh_window->type);
	}
	return result;
}

/*
 * Adjust the refresh window to align with circumscribed buckets, so it includes buckets, which
 * fully cover the refresh window.
 *
 * Bucketing refresh window is necessary for a continuous aggregate refresh, which can refresh only
 * entire buckets. The result of the function is a bucketed window, where its start is at the start
 * of a bucket, which contains the start of the refresh window, and its end is at the end of a
 * bucket, which contains the end of the refresh window.
 *
 * Example1, the window needs to expand:
 *    [---------)      - given refresh window
 * .|....|....|....|.  - buckets
 *  [--------------)   - circumscribed bucketed window
 *
 * Example2, the window is already aligned:
 *       [----)        - given refresh window
 * .|....|....|....|.  - buckets
 *       [----)        - inscribed bucketed window
 *
 * This function is called for an invalidation window before refreshing it and after the
 * invalidation window was adjusted to be fully inside a refresh window. In the case of a
 * continuous aggregate policy or manual refresh, the refresh window is the inscribed bucketed
 * window.
 *
 * The circumscribed behaviour is also used for a refresh on drop, when the refresh is called during
 * dropping chunks manually or as part of retention policy.
 */
static InternalTimeRange
compute_circumscribed_bucketed_refresh_window(const InternalTimeRange *const refresh_window,
											  const int64 bucket_width)
{
	InternalTimeRange result = *refresh_window;
	InternalTimeRange largest_bucketed_window =
		get_largest_bucketed_window(refresh_window->type, bucket_width);

	if (refresh_window->start <= largest_bucketed_window.start)
	{
		result.start = largest_bucketed_window.start;
	}
	else
	{
		/* For alignment with a bucket, which includes the start of the refresh window, we just
		 * need to get start of the bucket. */
		result.start =
			ts_time_bucket_by_type(bucket_width, refresh_window->start, refresh_window->type);
	}

	if (refresh_window->end >= largest_bucketed_window.end)
	{
		result.end = largest_bucketed_window.end;
	}
	else
	{
		int64 exclusive_end;
		int64 bucketed_end;

		Assert(refresh_window->end > result.start);

		/* The end of the window is non-inclusive so subtract one before
		 * bucketing in case we're already at the end of the bucket (we don't
		 * want to add an extra bucket).  */
		exclusive_end = ts_time_saturating_sub(refresh_window->end, 1, refresh_window->type);
		bucketed_end = ts_time_bucket_by_type(bucket_width, exclusive_end, refresh_window->type);

		/* We get the time value for the start of the bucket, so need to add
		 * bucket_width to get the end of it. */
		result.end = ts_time_saturating_add(bucketed_end, bucket_width, refresh_window->type);
	}
	return result;
}

/*
 * Initialize the refresh state for a continuous aggregate.
 *
 * The state holds information for executing a refresh of a continuous aggregate.
 */
static void
continuous_agg_refresh_init(CaggRefreshState *refresh, const ContinuousAgg *cagg,
							const InternalTimeRange *refresh_window)
{
	MemSet(refresh, 0, sizeof(*refresh));
	refresh->cagg = *cagg;
	refresh->cagg_ht = cagg_get_hypertable_or_fail(cagg->data.mat_hypertable_id);
	refresh->refresh_window = *refresh_window;
	refresh->partial_view.schema = &refresh->cagg.data.partial_view_schema;
	refresh->partial_view.name = &refresh->cagg.data.partial_view_name;
}

/*
 * Execute a refresh.
 *
 * The refresh will materialize the area given by the refresh window in the
 * refresh state.
 */
static void
continuous_agg_refresh_execute(const CaggRefreshState *refresh,
							   const InternalTimeRange *bucketed_refresh_window)
{
	SchemaAndName cagg_hypertable_name = {
		.schema = &refresh->cagg_ht->fd.schema_name,
		.name = &refresh->cagg_ht->fd.table_name,
	};
	/* The materialization function takes two ranges, one for new data and one
	 * for invalidated data. A refresh just uses one of them so the other one
	 * has a zero range. */
	InternalTimeRange unused_invalidation_range = {
		.type = refresh->refresh_window.type,
		.start = 0,
		.end = 0,
	};
	Dimension *time_dim = hyperspace_get_open_dimension(refresh->cagg_ht->space, 0);

	Assert(time_dim != NULL);

	continuous_agg_update_materialization(refresh->partial_view,
										  cagg_hypertable_name,
										  &time_dim->fd.column_name,
										  *bucketed_refresh_window,
										  unused_invalidation_range,
										  refresh->cagg.data.bucket_width);
}

static void
log_refresh_window(int elevel, const ContinuousAgg *cagg, const InternalTimeRange *refresh_window,
				   const char *msg)
{
	Datum start_ts;
	Datum end_ts;
	Oid outfuncid = InvalidOid;
	bool isvarlena;

	if (client_min_messages > elevel)
		return;

	start_ts = ts_internal_to_time_value(refresh_window->start, refresh_window->type);
	end_ts = ts_internal_to_time_value(refresh_window->end, refresh_window->type);
	getTypeOutputInfo(refresh_window->type, &outfuncid, &isvarlena);
	Assert(!isvarlena);

	elog(elevel,
		 "%s \"%s\" in window [ %s, %s ]",
		 msg,
		 NameStr(cagg->data.user_view_name),
		 DatumGetCString(OidFunctionCall1(outfuncid, start_ts)),
		 DatumGetCString(OidFunctionCall1(outfuncid, end_ts)));
}

static void
continuous_agg_refresh_with_window(const ContinuousAgg *cagg,
								   const InternalTimeRange *refresh_window,
								   const InvalidationStore *invalidations)
{
	CaggRefreshState refresh;
	TupleTableSlot *slot;

	continuous_agg_refresh_init(&refresh, cagg, refresh_window);
	slot = MakeSingleTupleTableSlotCompat(invalidations->tupdesc, &TTSOpsMinimalTuple);

	while (tuplestore_gettupleslot(invalidations->tupstore,
								   true /* forward */,
								   false /* copy */,
								   slot))
	{
		bool isnull;
		Datum start = slot_getattr(
			slot,
			Anum_continuous_aggs_materialization_invalidation_log_lowest_modified_value,
			&isnull);
		Datum end = slot_getattr(
			slot,
			Anum_continuous_aggs_materialization_invalidation_log_greatest_modified_value,
			&isnull);
		InternalTimeRange invalidation = {
			.type = refresh_window->type,
			.start = DatumGetInt64(start),
			/* Invalidations are inclusive at the end, while refresh windows
			 * aren't, so add one to the end of the invalidated region */
			.end = ts_time_saturating_add(DatumGetInt64(end), 1, refresh_window->type),
		};

		InternalTimeRange bucketed_refresh_window =
			compute_circumscribed_bucketed_refresh_window(&invalidation, cagg->data.bucket_width);

		log_refresh_window(DEBUG1, cagg, &bucketed_refresh_window, "invalidation refresh on");
		continuous_agg_refresh_execute(&refresh, &bucketed_refresh_window);
	}

	ExecDropSingleTupleTableSlot(slot);
}

#define REFRESH_FUNCTION_NAME "refresh_continuous_aggregate()"
/*
 * Refresh a continuous aggregate across the given window.
 */
Datum
continuous_agg_refresh(PG_FUNCTION_ARGS)
{
	Oid cagg_relid = PG_ARGISNULL(0) ? InvalidOid : PG_GETARG_OID(0);
	ContinuousAgg *cagg;
	Hypertable *cagg_ht;
	Dimension *time_dim;
	InternalTimeRange refresh_window = {
		.type = InvalidOid,
	};

	if (!OidIsValid(cagg_relid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid continuous aggregate")));

	cagg = ts_continuous_agg_find_by_relid(cagg_relid);

	if (NULL == cagg)
	{
		const char *relname = get_rel_name(cagg_relid);

		if (relname == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 (errmsg("continuous aggregate does not exist"))));
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 (errmsg("relation \"%s\" is not a continuous aggregate", relname))));
	}

	cagg_ht = ts_hypertable_get_by_id(cagg->data.mat_hypertable_id);
	Assert(cagg_ht != NULL);
	time_dim = hyperspace_get_open_dimension(cagg_ht->space, 0);
	Assert(time_dim != NULL);
	refresh_window.type = ts_dimension_get_partition_type(time_dim);

	if (!PG_ARGISNULL(1))
		refresh_window.start = ts_time_value_from_arg(PG_GETARG_DATUM(1),
													  get_fn_expr_argtype(fcinfo->flinfo, 1),
													  refresh_window.type);
	else
		refresh_window.start = ts_time_get_min(refresh_window.type);

	if (!PG_ARGISNULL(2))
		refresh_window.end = ts_time_value_from_arg(PG_GETARG_DATUM(2),
													get_fn_expr_argtype(fcinfo->flinfo, 2),
													refresh_window.type);
	else
		refresh_window.end = ts_time_get_noend_or_max(refresh_window.type);

	continuous_agg_refresh_internal(cagg, &refresh_window, false);
	PG_RETURN_VOID();
}

static void
emit_up_to_date_notice(const ContinuousAgg *cagg)
{
	elog(NOTICE,
		 "continuous aggregate \"%s\" is already up-to-date",
		 NameStr(cagg->data.user_view_name));
}

static bool
process_cagg_invalidations_and_refresh(const ContinuousAgg *cagg,
									   const InternalTimeRange *refresh_window, bool verbose)
{
	InvalidationStore *invalidations;
	Oid hyper_relid = ts_hypertable_id_to_relid(cagg->data.mat_hypertable_id);

	/* Lock the continuous aggregate's materialized hypertable to protect
	 * against concurrent refreshes. Only concurrent reads will be
	 * allowed. This is a heavy lock that serializes all refreshes on the same
	 * continuous aggregate. We might want to consider relaxing this in the
	 * future, e.g., we'd like to at least allow concurrent refreshes on the
	 * same continuous aggregate when they don't have overlapping refresh
	 * windows.
	 */
	LockRelationOid(hyper_relid, ExclusiveLock);
	invalidations = invalidation_process_cagg_log(cagg, refresh_window);

	if (invalidations != NULL)
	{
		if (verbose)
		{
			Assert(OidIsValid(cagg->relid));
			ereport(NOTICE,
					(errmsg("refreshing continuous aggregate \"%s\"", get_rel_name(cagg->relid)),
					 errhint("Use WITH NO DATA if you do not want to refresh the continuous "
							 "aggregate on creation.")));
		}
		continuous_agg_refresh_with_window(cagg, refresh_window, invalidations);
		invalidation_store_free(invalidations);
		return true;
	}

	return false;
}

void
continuous_agg_refresh_internal(const ContinuousAgg *cagg,
								const InternalTimeRange *refresh_window_arg, bool verbose)
{
	Catalog *catalog = ts_catalog_get();
	int32 mat_id = cagg->data.mat_hypertable_id;
	InternalTimeRange refresh_window;
	int64 computed_invalidation_threshold;
	int64 invalidation_threshold;

	/* Like regular materialized views, require owner to refresh. */
	if (!pg_class_ownercheck(cagg->relid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER,
					   get_relkind_objtype(get_rel_relkind(cagg->relid)),
					   get_rel_name(cagg->relid));

	PreventCommandIfReadOnly(REFRESH_FUNCTION_NAME);

	/* Prevent running refresh if we're in a transaction block since a refresh
	 * can run two transactions and might take a long time to release locks if
	 * there's a lot to materialize. Strictly, it is optional to prohibit
	 * transaction blocks since there will be only one transaction if the
	 * invalidation threshold needs no update. However, materialization might
	 * still take a long time and it is probably best for consistency to always
	 * prevent transaction blocks.  */
	PreventInTransactionBlock(true, REFRESH_FUNCTION_NAME);

	if (refresh_window_arg->start >= refresh_window_arg->end)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid refresh window"),
				 errhint("The start of the window must be before the end.")));

	refresh_window =
		compute_inscribed_bucketed_refresh_window(refresh_window_arg, cagg->data.bucket_width);
	log_refresh_window(DEBUG1, cagg, &refresh_window, "refreshing continuous aggregate");

	/* Perform the refresh across two transactions.
	 *
	 * The first transaction moves the invalidation threshold (if needed) and
	 * copies over invalidations from the hypertable log to the cagg
	 * invalidation log. Doing the threshold and copying as part of the first
	 * transaction ensures that the threshold and new invalidations will be
	 * visible as soon as possible to concurrent refreshes and that we keep
	 * locks for only a short period. Note that the first transaction
	 * serializes around the threshold table lock, which protects both the
	 * threshold and the invalidation processing against concurrent refreshes.
	 *
	 * The second transaction processes the cagg invalidation log and then
	 * performs the actual refresh (materialization of data). This transaction
	 * serializes around a lock on the materialized hypertable for the
	 * continuous aggregate that gets refreshed.
	 */
	LockRelationOid(catalog_get_table_id(catalog, CONTINUOUS_AGGS_INVALIDATION_THRESHOLD),
					AccessExclusiveLock);

	/* Compute new invalidation threshold. Note that this computation caps the
	 * threshold at the end of the last bucket that holds data in the
	 * underlying hypertable. */
	computed_invalidation_threshold = invalidation_threshold_compute(cagg, &refresh_window);

	/* Set the new invalidation threshold. Note that this only updates the
	 * threshold if the new value is greater than the old one. Otherwise, the
	 * existing threshold is returned. */
	invalidation_threshold = invalidation_threshold_set_or_get(cagg->data.raw_hypertable_id,
															   computed_invalidation_threshold);

	/* We must also cap the refresh window at the invalidation threshold. If
	 * we process invalidations after the threshold, the continuous aggregates
	 * won't be refreshed when the threshold is moved forward in the
	 * future. The invalidation threshold should already be aligned on bucket
	 * boundary. */
	if (refresh_window_arg->end > invalidation_threshold)
		refresh_window.end = invalidation_threshold;

	/* Capping the end might have made the window 0, or negative, so
	 * nothing to refresh in that case */
	if (refresh_window.start >= refresh_window.end)
	{
		emit_up_to_date_notice(cagg);
		return;
	}

	/* Process invalidations in the hypertable invalidation log */
	invalidation_process_hypertable_log(cagg);

	/* Start a new transaction. Note that this invalidates previous memory
	 * allocations (and locks). */
	PopActiveSnapshot();
	CommitTransactionCommand();
	StartTransactionCommand();
	cagg = ts_continuous_agg_find_by_mat_hypertable_id(mat_id);

	if (!process_cagg_invalidations_and_refresh(cagg, &refresh_window, verbose))
		emit_up_to_date_notice(cagg);
}

/*
 * Refresh all continuous aggregates on a hypertable.
 *
 * The refreshing happens in a single transaction. For this to work correctly,
 * there must be no new invalidations written in the refreshed region during
 * the refresh. Therefore, the caller is responsible for proper locking to
 * ensure there are no invalidations (INSERTs, DELETEs, etc.). For instance,
 * exclusively locking the hypertable or the individual chunks covered by the
 * region would work.
 */
void
continuous_agg_refresh_all(const Hypertable *ht, int64 start, int64 end)
{
	Catalog *catalog = ts_catalog_get();
	List *caggs = ts_continuous_aggs_find_by_raw_table_id(ht->fd.id);
	Dimension *dim = hyperspace_get_open_dimension(ht->space, 0);
	InternalTimeRange refresh_window = {
		.type = ts_dimension_get_partition_type(dim),
		.start = start,
		.end = end,
	};
	ListCell *lc;

	/* We're not doing any specific permissions checks here. It's assumed that
	 * whoever calls this function has done appropriate checks for the
	 * operation. For instance, if this is called as a result of
	 * "refresh-on-drop", it is assumed that refresh can happen if the user is
	 * permitted to drop data. */

	Assert(list_length(caggs) > 0);
	LockRelationOid(catalog_get_table_id(catalog, CONTINUOUS_AGGS_INVALIDATION_THRESHOLD),
					AccessExclusiveLock);
	invalidation_threshold_set_or_get(ht->fd.id, refresh_window.end);

	/* It is enough to process the hypertable invalidation log once,
	 * so do it only for the first continuous aggregate. */
	invalidation_process_hypertable_log(linitial(caggs));
	/* Must make invalidation processing visible */
	CommandCounterIncrement();

	foreach (lc, caggs)
	{
		const ContinuousAgg *cagg = lfirst(lc);

		process_cagg_invalidations_and_refresh(cagg, &refresh_window, false);
	}
}
