/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */

#include <postgres.h>
#include <access/xact.h>
#include <miscadmin.h>
#include <utils/builtins.h>

#include "bgw/job.h"
#include "compression_api.h"
#include "errors.h"
#include "hypertable.h"
#include "hypertable_cache.h"
#include "policy_utils.h"
#include "utils.h"
#include "jsonb_utils.h"
#include "bgw_policy/job.h"

/*
 * Default scheduled interval for compress jobs = default chunk length.
 * If this is non-timestamp based hypertable, then default is 1 day
 */
#define DEFAULT_SCHEDULE_INTERVAL                                                                  \
	DatumGetIntervalP(DirectFunctionCall3(interval_in, CStringGetDatum("1 day"), InvalidOid, -1))

/* Default max runtime is unlimited for compress chunks */
#define DEFAULT_MAX_RUNTIME                                                                        \
	DatumGetIntervalP(DirectFunctionCall3(interval_in, CStringGetDatum("0"), InvalidOid, -1))

/* Right now, there is an infinite number of retries for compress_chunks jobs */
#define DEFAULT_MAX_RETRIES -1
/* Default retry period for reorder_jobs is currently 1 hour */
#define DEFAULT_RETRY_PERIOD                                                                       \
	DatumGetIntervalP(DirectFunctionCall3(interval_in, CStringGetDatum("1 hour"), InvalidOid, -1))

#define POLICY_COMPRESSION_PROC_NAME "policy_compression"
#define CONFIG_KEY_HYPERTABLE_ID "hypertable_id"
#define CONFIG_KEY_COMPRESS_AFTER "compress_after"

int32
policy_compression_get_hypertable_id(const Jsonb *config)
{
	bool found;
	int32 hypertable_id = ts_jsonb_get_int32_field(config, CONFIG_KEY_HYPERTABLE_ID, &found);

	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not find hypertable_id in config for job")));

	return hypertable_id;
}

int64
policy_compression_get_compress_after_int(const Jsonb *config)
{
	bool found;
	int32 hypertable_id = ts_jsonb_get_int64_field(config, CONFIG_KEY_COMPRESS_AFTER, &found);

	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not find %s in config for job", CONFIG_KEY_COMPRESS_AFTER)));

	return hypertable_id;
}

Interval *
policy_compression_get_compress_after_interval(const Jsonb *config)
{
	Interval *interval = ts_jsonb_get_interval_field(config, CONFIG_KEY_COMPRESS_AFTER);

	if (interval == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not find %s in config for job", CONFIG_KEY_COMPRESS_AFTER)));

	return interval;
}

Datum
policy_compression_proc(PG_FUNCTION_ARGS)
{
	if (PG_NARGS() != 2 || PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_VOID();

	PreventCommandIfReadOnly("policy_compression()");

	policy_compression_execute(PG_GETARG_INT32(0), PG_GETARG_JSONB_P(1));

	PG_RETURN_VOID();
}

Datum
policy_compression_add(PG_FUNCTION_ARGS)
{
	NameData application_name;
	NameData compress_chunks_name;
	NameData proc_name, proc_schema, owner;
	int32 job_id;
	Oid ht_oid = PG_GETARG_OID(0);
	Datum compress_after_datum = PG_GETARG_DATUM(1);
	Oid compress_after_type = PG_ARGISNULL(1) ? InvalidOid : get_fn_expr_argtype(fcinfo->flinfo, 1);
	bool if_not_exists = PG_GETARG_BOOL(2);
	Interval *default_schedule_interval = DEFAULT_SCHEDULE_INTERVAL;

	PreventCommandIfReadOnly("add_compression_policy()");

	Hypertable *hypertable;
	Cache *hcache;
	Dimension *dim;
	ts_hypertable_permissions_check(ht_oid, GetUserId());
	Oid owner_id = ts_hypertable_permissions_check(ht_oid, GetUserId());

	/* check if this is a table with compression enabled */
	hypertable = ts_hypertable_cache_get_cache_and_entry(ht_oid, CACHE_FLAG_NONE, &hcache);

	if (hypertable_is_distributed(hypertable))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("add_compression_policy not implemented for distributed hypertable"),
				 errdetail(
					 "Current version doesn't implement support for add_compression_policy() on "
					 "distributed hypertables.")));

	if (!TS_HYPERTABLE_HAS_COMPRESSION(hypertable))
	{
		ts_cache_release(hcache);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("compression not enabled on hypertable \"%s\"", get_rel_name(ht_oid)),
				 errhint("Enable compression before adding a compression policy.")));
	}

	ts_bgw_job_validate_job_owner(owner_id);

	/* Make sure that an existing policy doesn't exist on this hypertable */
	List *jobs = ts_bgw_job_find_by_proc_and_hypertable_id(POLICY_COMPRESSION_PROC_NAME,
														   INTERNAL_SCHEMA_NAME,
														   hypertable->fd.id);

	dim = hyperspace_get_open_dimension(hypertable->space, 0);
	Oid partitioning_type = ts_dimension_get_partition_type(dim);

	if (jobs != NIL)
	{
		if (!if_not_exists)
		{
			ts_cache_release(hcache);
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("compression policy already exists for hypertable \"%s\"",
							get_rel_name(ht_oid)),
					 errhint("Set option \"if_not_exists\" to true to avoid error.")));
		}
		Assert(list_length(jobs) == 1);
		BgwJob *existing = linitial(jobs);
		if (policy_config_check_hypertable_lag_equality(existing->fd.config,
														CONFIG_KEY_COMPRESS_AFTER,
														partitioning_type,
														compress_after_type,
														compress_after_datum))
		{
			/* If all arguments are the same, do nothing */
			ts_cache_release(hcache);
			ereport(NOTICE,
					(errmsg("compression policy already exists on hypertable \"%s\", skipping",
							get_rel_name(ht_oid))));
			PG_RETURN_INT32(-1);
		}
		else
		{
			ts_cache_release(hcache);
			ereport(WARNING,
					(errmsg("compression policy already exists for hypertable \"%s\" with "
							"different arguments",
							get_rel_name(ht_oid)),
					 errhint("Remove the existing policy before adding a new one.")));
			PG_RETURN_INT32(-1);
		}
	}

	if (dim && IS_TIMESTAMP_TYPE(ts_dimension_get_partition_type(dim)))
	{
		default_schedule_interval = DatumGetIntervalP(
			ts_internal_to_interval_value(dim->fd.interval_length / 2, INTERVALOID));
	}

	/* insert a new job into jobs table */
	namestrcpy(&application_name, "Compression Policy");
	namestrcpy(&compress_chunks_name, "compress_chunks");
	namestrcpy(&proc_name, POLICY_COMPRESSION_PROC_NAME);
	namestrcpy(&proc_schema, INTERNAL_SCHEMA_NAME);
	namestrcpy(&owner, GetUserNameFromId(owner_id, false));

	JsonbParseState *parse_state = NULL;

	pushJsonbValue(&parse_state, WJB_BEGIN_OBJECT, NULL);
	ts_jsonb_add_int32(parse_state, CONFIG_KEY_HYPERTABLE_ID, hypertable->fd.id);

	switch (compress_after_type)
	{
		case INTERVALOID:
			ts_jsonb_add_interval(parse_state,
								  CONFIG_KEY_COMPRESS_AFTER,
								  DatumGetIntervalP(compress_after_datum));
			break;
		case INT2OID:
			ts_jsonb_add_int64(parse_state,
							   CONFIG_KEY_COMPRESS_AFTER,
							   DatumGetInt16(compress_after_datum));
			break;
		case INT4OID:
			ts_jsonb_add_int64(parse_state,
							   CONFIG_KEY_COMPRESS_AFTER,
							   DatumGetInt32(compress_after_datum));
			break;
		case INT8OID:
			ts_jsonb_add_int64(parse_state,
							   CONFIG_KEY_COMPRESS_AFTER,
							   DatumGetInt64(compress_after_datum));
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unsupported datatype for %s: %s",
							CONFIG_KEY_COMPRESS_AFTER,
							format_type_be(compress_after_type))));
	}

	JsonbValue *result = pushJsonbValue(&parse_state, WJB_END_OBJECT, NULL);
	Jsonb *config = JsonbValueToJsonb(result);

	job_id = ts_bgw_job_insert_relation(&application_name,
										&compress_chunks_name,
										default_schedule_interval,
										DEFAULT_MAX_RUNTIME,
										DEFAULT_MAX_RETRIES,
										DEFAULT_RETRY_PERIOD,
										&proc_schema,
										&proc_name,
										&owner,
										true,
										hypertable->fd.id,
										config);

	ts_cache_release(hcache);

	PG_RETURN_INT32(job_id);
}

Datum
policy_compression_remove(PG_FUNCTION_ARGS)
{
	Oid hypertable_oid = PG_GETARG_OID(0);
	bool if_exists = PG_GETARG_BOOL(1);

	PreventCommandIfReadOnly("remove_compression_policy()");

	int ht_id = ts_hypertable_relid_to_id(hypertable_oid);

	List *jobs = ts_bgw_job_find_by_proc_and_hypertable_id(POLICY_COMPRESSION_PROC_NAME,
														   INTERNAL_SCHEMA_NAME,
														   ht_id);
	if (jobs == NIL)
	{
		if (!if_exists)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("cannot remove compress chunks policy, no such policy exists")));
		else
		{
			ereport(NOTICE,
					(errmsg("compress chunks policy does not exist on hypertable \"%s\", skipping",
							get_rel_name(hypertable_oid))));
			PG_RETURN_BOOL(false);
		}
	}

	ts_hypertable_permissions_check(hypertable_oid, GetUserId());

	Assert(list_length(jobs) == 1);
	BgwJob *job = linitial(jobs);

	ts_bgw_job_delete_by_id(job->fd.id);

	PG_RETURN_BOOL(true);
}
