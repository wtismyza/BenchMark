/*
 * This file and its contents are licensed under the Apache License 2.0.
 * Please see the included NOTICE for copyright information and
 * LICENSE-APACHE for a copy of the license.
 */
#include <postgres.h>

#include <miscadmin.h>
#include <postmaster/bgworker.h>
#include <storage/ipc.h>
#include <storage/latch.h>
#include <storage/lwlock.h>
#include <storage/lmgr.h>
#include <storage/proc.h>
#include <storage/shmem.h>
#include <utils/guc.h>
#include <utils/jsonb.h>
#include <utils/timestamp.h>
#include <utils/snapmgr.h>
#include <utils/memutils.h>
#include <utils/builtins.h>
#include <access/xact.h>
#include <pgstat.h>
#include <signal.h>

#include "extension.h"
#include "log.h"
#include "bgw/scheduler.h"
#include "bgw/job.h"
#include "bgw/job_stat.h"
#include "timer_mock.h"
#include "params.h"
#include "test_utils.h"
#include "cross_module_fn.h"

TS_FUNCTION_INFO_V1(ts_bgw_db_scheduler_test_run_and_wait_for_scheduler_finish);
TS_FUNCTION_INFO_V1(ts_bgw_db_scheduler_test_run);
TS_FUNCTION_INFO_V1(ts_bgw_db_scheduler_test_wait_for_scheduler_finish);
TS_FUNCTION_INFO_V1(ts_bgw_db_scheduler_test_main);
TS_FUNCTION_INFO_V1(ts_bgw_job_execute_test);

typedef enum TestJobType
{
	TEST_JOB_TYPE_JOB_1 = 0,
	TEST_JOB_TYPE_JOB_2_ERROR,
	TEST_JOB_TYPE_JOB_3_LONG,
	TEST_JOB_TYPE_JOB_4,
	_MAX_TEST_JOB_TYPE
} TestJobType;

static const char *test_job_type_names[_MAX_TEST_JOB_TYPE] = {
	[TEST_JOB_TYPE_JOB_1] = "bgw_test_job_1",
	[TEST_JOB_TYPE_JOB_2_ERROR] = "bgw_test_job_2_error",
	[TEST_JOB_TYPE_JOB_3_LONG] = "bgw_test_job_3_long",
	[TEST_JOB_TYPE_JOB_4] = "bgw_test_job_4",
};

static char *
serialize_test_parameters(int32 ttl)
{
	JsonbValue *result;
	JsonbValue ttl_value;
	JsonbParseState *parse_state = NULL;
	Jsonb *jb;
	StringInfo jtext = makeStringInfo();
	JsonbValue user_oid;

	user_oid.type = jbvNumeric;
	user_oid.val.numeric =
		DatumGetNumeric(DirectFunctionCall1(int4_numeric, Int32GetDatum((int32) GetUserId())));

	ttl_value.type = jbvNumeric;
	ttl_value.val.numeric = DatumGetNumeric(DirectFunctionCall1(int4_numeric, Int32GetDatum(ttl)));

	result = pushJsonbValue(&parse_state, WJB_BEGIN_ARRAY, NULL);

	result = pushJsonbValue(&parse_state, WJB_ELEM, &ttl_value);
	result = pushJsonbValue(&parse_state, WJB_ELEM, &user_oid);

	result = pushJsonbValue(&parse_state, WJB_END_ARRAY, NULL);

	jb = JsonbValueToJsonb(result);
	(void) JsonbToCString(jtext, &jb->root, VARSIZE(jb));
	TestAssertTrue(jtext->len < BGW_EXTRALEN);

	return jtext->data;
}

static void
deserialize_test_parameters(char *params, int32 *ttl, Oid *user_oid)
{
	Jsonb *jb = (Jsonb *) DatumGetPointer(DirectFunctionCall1(jsonb_in, CStringGetDatum(params)));
	JsonbValue *ttl_v = getIthJsonbValueFromContainer(&jb->root, 0);
	JsonbValue *user_v = getIthJsonbValueFromContainer(&jb->root, 1);
	Numeric ttl_numeric;
	Numeric user_numeric;

	TestAssertTrue(ttl_v->type == jbvNumeric);
	TestAssertTrue(user_v->type == jbvNumeric);

	ttl_numeric = ttl_v->val.numeric;
	user_numeric = user_v->val.numeric;
	*ttl = DatumGetInt32(DirectFunctionCall1(numeric_int4, NumericGetDatum(ttl_numeric)));
	*user_oid =
		(Oid) DatumGetInt32(DirectFunctionCall1(numeric_int4, NumericGetDatum(user_numeric)));
}

extern Datum
ts_bgw_db_scheduler_test_main(PG_FUNCTION_ARGS)
{
	Oid db_oid = DatumGetObjectId(MyBgworkerEntry->bgw_main_arg);
	int32 ttl;
	Oid user_oid;

	BackgroundWorkerBlockSignals();
	/* Setup any signal handlers here */
	ts_bgw_scheduler_register_signal_handlers();
	BackgroundWorkerUnblockSignals();
	ts_bgw_scheduler_setup_callbacks();

	deserialize_test_parameters(MyBgworkerEntry->bgw_extra, &ttl, &user_oid);

	elog(WARNING, "scheduler user id %d", user_oid);
	elog(WARNING, "running a test in the background: db=%d ttl=%d", db_oid, ttl);

	BackgroundWorkerInitializeConnectionByOid(db_oid, user_oid, 0);

	StartTransactionCommand();
	ts_params_get();
	ts_initialize_timer_latch();
	CommitTransactionCommand();

	ts_bgw_log_set_application_name("DB Scheduler");
	ts_register_emit_log_hook();

	ts_timer_set(&ts_mock_timer);

	ts_bgw_job_set_job_entrypoint_function_name("ts_bgw_job_execute_test");

	pgstat_report_appname("DB Scheduler Test");

	ts_bgw_scheduler_setup_mctx();

	ts_bgw_scheduler_process(ttl, ts_timer_mock_register_bgw_handle);

	PG_RETURN_VOID();
}

static BackgroundWorkerHandle *
start_test_scheduler(char *params)
{
	/*
	 * This is where we would increment the number of bgw used, if we
	 * decide to do so
	 */
	ts_bgw_scheduler_setup_mctx();

	return ts_bgw_start_worker("ts_bgw_db_scheduler_test_main",
							   "ts_bgw_db_scheduler_test_main",
							   params);
}

extern Datum
ts_bgw_db_scheduler_test_run_and_wait_for_scheduler_finish(PG_FUNCTION_ARGS)
{
	char *params = serialize_test_parameters(PG_GETARG_INT32(0));
	BackgroundWorkerHandle *worker_handle;
	pid_t pid;

	worker_handle = start_test_scheduler(params);

	if (worker_handle != NULL)
	{
		BgwHandleStatus status = WaitForBackgroundWorkerStartup(worker_handle, &pid);
		TestAssertTrue(BGWH_STARTED == status);
		if (status != BGWH_STARTED)
			elog(ERROR, "bgw not started");

		status = WaitForBackgroundWorkerShutdown(worker_handle);
		TestAssertTrue(BGWH_STOPPED == status);
		if (status != BGWH_STOPPED)
			elog(ERROR, "bgw not stopped");
	}

	PG_RETURN_VOID();
}

static BackgroundWorkerHandle *current_handle = NULL;

extern Datum
ts_bgw_db_scheduler_test_run(PG_FUNCTION_ARGS)
{
	char *params = serialize_test_parameters(PG_GETARG_INT32(0));
	pid_t pid;
	MemoryContext old_ctx;
	BgwHandleStatus status;

	old_ctx = MemoryContextSwitchTo(TopMemoryContext);
	current_handle = start_test_scheduler(params);
	MemoryContextSwitchTo(old_ctx);

	status = WaitForBackgroundWorkerStartup(current_handle, &pid);
	TestAssertTrue(BGWH_STARTED == status);
	if (status != BGWH_STARTED)
		elog(ERROR, "bgw not started");

	PG_RETURN_VOID();
}

extern Datum
ts_bgw_db_scheduler_test_wait_for_scheduler_finish(PG_FUNCTION_ARGS)
{
	if (current_handle != NULL)
	{
		BgwHandleStatus status = WaitForBackgroundWorkerShutdown(current_handle);
		TestAssertTrue(BGWH_STOPPED == status);
		if (status != BGWH_STOPPED)
			elog(ERROR, "bgw not stopped");
	}
	PG_RETURN_VOID();
}

static bool
test_job_1()
{
	StartTransactionCommand();
	elog(WARNING, "Execute job 1");

	CommitTransactionCommand();
	return true;
}

static bool
test_job_2_error()
{
	StartTransactionCommand();
	elog(WARNING, "Before error job 2");

	elog(ERROR, "Error job 2");

	elog(WARNING, "After error job 2");

	CommitTransactionCommand();
	return true;
}

static pqsigfunc prev_signal_func = NULL;

static void log_terminate_signal(SIGNAL_ARGS)
{
	elog(WARNING, "Job got term signal");

	if (prev_signal_func != NULL)
		prev_signal_func(postgres_signal_arg);
}

TS_FUNCTION_INFO_V1(ts_bgw_test_job_sleep);

/*
 * This function is used for testing removing jobs with
 * a currently running background job.
 */
Datum
ts_bgw_test_job_sleep(PG_FUNCTION_ARGS)
{
	BackgroundWorkerBlockSignals();

	/*
	 * Only set prev_signal_func once to prevent it from being set to
	 * log_terminate_signal.
	 */
	if (prev_signal_func == NULL)
		prev_signal_func = pqsignal(SIGTERM, log_terminate_signal);
	/* Setup any signal handlers here */
	BackgroundWorkerUnblockSignals();

	elog(WARNING, "Before sleep");
	PopActiveSnapshot();
	/*
	 * we commit here so the effect of the elog which is written
	 * to a table with a emit_log_hook is seen by other transactions
	 * to verify the background job started
	 */
	CommitTransactionCommand();

	StartTransactionCommand();
	DirectFunctionCall1(pg_sleep, Float8GetDatum(10));

	elog(WARNING, "After sleep");

	PG_RETURN_VOID();
}

static bool
test_job_3_long()
{
	BackgroundWorkerBlockSignals();

	/*
	 * Only set prev_signal_func once to prevent it from being set to
	 * log_terminate_signal.
	 */
	if (prev_signal_func == NULL)
		prev_signal_func = pqsignal(SIGTERM, log_terminate_signal);
	/* Setup any signal handlers here */
	BackgroundWorkerUnblockSignals();

	elog(WARNING, "Before sleep job 3");

	DirectFunctionCall1(pg_sleep, Float8GetDatum(0.5L));

	elog(WARNING, "After sleep job 3");
	return true;
}

/* Exactly like job 1, except a wrapper will change its next_start. */
static bool
test_job_4(void)
{
	elog(WARNING, "Execute job 4");
	return true;
}

static TestJobType
get_test_job_type_from_name(Name job_type_name)
{
	int i;

	for (i = 0; i < _MAX_TEST_JOB_TYPE; i++)
	{
		if (namestrcmp(job_type_name, test_job_type_names[i]) == 0)
			return i;
	}
	return _MAX_TEST_JOB_TYPE;
}

static bool
test_job_dispatcher(BgwJob *job)
{
	ts_register_emit_log_hook();
	ts_bgw_log_set_application_name(strdup(NameStr(job->fd.application_name)));

	StartTransactionCommand();
	ts_params_get();
	CommitTransactionCommand();

	switch (get_test_job_type_from_name(&job->fd.proc_name))
	{
		case TEST_JOB_TYPE_JOB_1:
			return test_job_1();
		case TEST_JOB_TYPE_JOB_2_ERROR:
			return test_job_2_error();
		case TEST_JOB_TYPE_JOB_3_LONG:
			return test_job_3_long();
		case TEST_JOB_TYPE_JOB_4:
		{
			/* Set next_start to 200ms */
			Interval new_interval = { .time = .2 * USECS_PER_SEC };
			return ts_bgw_job_run_and_set_next_start(job, test_job_4, 3, &new_interval);
		}
		default:
			return ts_cm_functions->job_execute(job);
	}
	return false;
}

Datum
ts_bgw_job_execute_test(PG_FUNCTION_ARGS)
{
	ts_timer_set(&ts_mock_timer);
	ts_bgw_job_set_scheduler_test_hook(test_job_dispatcher);

	return ts_bgw_job_entrypoint(fcinfo);
}
