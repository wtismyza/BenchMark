/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */

/*
 * This file contains source code that was copied and/or modified from the
 * PostgreSQL database, which is licensed under the open-source PostgreSQL
 * License. Please see the NOTICE at the top level directory for a copy of
 * the PostgreSQL License.
 */
#include <postgres.h>
#include <access/xact.h>
#include <catalog/pg_foreign_server.h>
#include <commands/defrem.h>
#include <common/md5.h>
#include <libpq-events.h>
#include <libpq/libpq.h>
#include <mb/pg_wchar.h>
#include <miscadmin.h>
#include <nodes/makefuncs.h>
#include <port.h>
#include <postmaster/postmaster.h>
#include <utils/builtins.h>
#include <utils/fmgrprotos.h>
#include <utils/inval.h>
#include <utils/guc.h>

#include <dist_util.h>
#include <errors.h>
#include <extension_constants.h>
#include <guc.h>
#include <telemetry/telemetry_metadata.h>
#include "connection.h"
#include "utils.h"

/*
 * Connection library for TimescaleDB.
 *
 * This library file contains convenience functionality around the libpq
 * API. The major additional functionality offered includes:
 *
 * - libpq object lifecycles are tied to transactions (connections and
 *   results). This ensures that there are no memory leaks caused by libpq
 *   objects after a transaction completes.
 * - connection configuration suitable for TimescaleDB.
 *
 * NOTE that it is strongly adviced that connection-related functions do not
 * throw exceptions with, e.g., elog(ERROR). While exceptions can be caught
 * with PG_TRY-CATCH for cleanup, it is not possible to safely continue the
 * transaction that threw the exception as if no error occurred (see the
 * following post if unconvinced:
 * https://www.postgresql.org/message-id/27190.1508727890%40sss.pgh.pa.us).
 *
 * In some cases, we need to be able to continue a transaction even if a
 * connection fails. One example is the removal of a data node, which must be
 * able to proceed even if the node is no longer available to respond to a
 * connection. Another example is performing a liveness check for node status.
 *
 * Therefore, it is best that defer throwing exceptions to high-level
 * functions that know when it is appropriate.
 */

/* for assigning cursor numbers and prepared statement numbers */
static unsigned int cursor_number = 0;
static unsigned int prep_stmt_number = 0;
static RemoteConnectionStats connstats = { 0 };

static int eventproc(PGEventId eventid, void *eventinfo, void *data);

TSConnectionId
remote_connection_id(const Oid server_oid, const Oid user_oid)
{
	TSConnectionId id = { .server_id = server_oid, .user_id = user_oid };
	return id;
}

void
remote_connection_id_set(TSConnectionId *const id, Oid const server_oid, Oid const user_oid)
{
	id->server_id = server_oid;
	id->user_id = user_oid;
}

/*
 * A simple circular list implementation for tracking libpq connection and
 * result objects. We can't use pg_list here since it is bound to PostgreSQL's
 * memory management system, while libpq is not.
 */
typedef struct ListNode
{
	struct ListNode *next;
	struct ListNode *prev;
} ListNode;

#define IS_DETACHED_ENTRY(entry) ((entry)->next == NULL && (entry)->prev == NULL)

/*
 * Detach a list node.
 *
 * Detaches a list node from the list, unless it is the anchor/head (which is
 * a no-op).
 */
static inline void
list_detach(ListNode *entry)
{
	ListNode *prev = entry->prev;
	ListNode *next = entry->next;

	next->prev = prev;
	prev->next = next;
	/* Clear entry fields */
	entry->prev = NULL;
	entry->next = NULL;
}

/*
 * Insert a list node entry after the prev node.
 */
static inline void
list_insert_after(ListNode *entry, ListNode *prev)
{
	ListNode *next = prev->next;

	next->prev = entry;
	entry->next = next;
	entry->prev = prev;
	prev->next = entry;
}

/*
 * List entry that holds a PGresult object.
 */
typedef struct ResultEntry
{
	struct ListNode ln;		  /* Must be first entry */
	TSConnection *conn;		  /* The connection the result was created on */
	SubTransactionId subtxid; /* The subtransaction ID that created this result, if any. */
	PGresult *result;
} ResultEntry;

typedef struct TSConnection
{
	ListNode ln;			  /* Must be first entry */
	PGconn *pg_conn;		  /* PostgreSQL connection */
	bool closing_guard;		  /* Guard against calling PQfinish() directly on PGconn */
	bool processing;		  /* TRUE if there is ongoing async request processing */
	NameData node_name;		  /* Associated data node name */
	char *tz_name;			  /* Timezone name last sent over connection */
	bool autoclose;			  /* Set if this connection should automatically
							   * close at the end of the (sub-)transaction */
	SubTransactionId subtxid; /* The subtransaction ID that created this connection, if any. */
	int xact_depth;			  /* 0 => no transaction, 1 => main transaction, > 1 =>
							   * levels of subtransactions */
	bool xact_transitioning;  /* TRUE if connection is transitioning to
							   * another transaction state */
	ListNode results;		  /* Head of PGresult list */
} TSConnection;

/*
 * List of all connections we create. Used to auto-free connections and/or
 * PGresults at transaction end.
 */
static ListNode connections = { &connections, &connections };

/*
 * The following event handlers make sure all PGresult are freed with
 * PQClear() when its parent connection is closed.
 *
 * It is still recommended to explicitly call PGclear() or
 * remote_connection_result_close(), however, especially when PGresults are
 * created in a tight loop (e.g., when scanning many tuples on a remote
 * table).
 */
#define EVENTPROC_FAILURE 0
#define EVENTPROC_SUCCESS 1

static void
remote_connection_free(TSConnection *conn)
{
	if (NULL != conn->tz_name)
		free(conn->tz_name);
	free(conn);
}

/*
 * Invoked on PQfinish(conn). Frees all PGresult objects created on the
 * connection, apart from those already freed with PQclear().
 */
static int
handle_conn_destroy(PGEventConnDestroy *event)
{
	TSConnection *conn = PQinstanceData(event->conn, eventproc);
	unsigned int results_count = 0;
	ListNode *curr;

	Assert(NULL != conn);
	Assert(conn->closing_guard);

	curr = conn->results.next;

	while (curr != &conn->results)
	{
		ResultEntry *entry = (ResultEntry *) curr;
		PGresult *result = entry->result;

		curr = curr->next;
		PQclear(result);
		/* No need to free curr here since PQclear will invoke
		 * handle_result_destroy() which will free it */
		results_count++;
	}

	conn->pg_conn = NULL;
	list_detach(&conn->ln);

	if (results_count > 0)
		elog(DEBUG3, "cleared %u result objects on connection %p", results_count, conn);

	connstats.connections_closed++;

	if (!conn->closing_guard)
	{
		ereport(WARNING,
				(errcode(ERRCODE_CONNECTION_EXCEPTION), errmsg("invalid closing of connection")));
		remote_connection_free(conn);
	}

	return EVENTPROC_SUCCESS;
}

/*
 * Invoked on PQgetResult(conn). Adds the PGresult to the list in the parent
 * TSConnection.
 */
static int
handle_result_create(PGEventResultCreate *event)
{
	TSConnection *conn = PQinstanceData(event->conn, eventproc);
	ResultEntry *entry;

	Assert(NULL != conn);

	/* We malloc this (instead of palloc) since bound PGresult, which also
	 * lives outside PostgreSQL's memory management. */
	entry = malloc(sizeof(ResultEntry));

	if (NULL == entry)
		return EVENTPROC_FAILURE;

	MemSet(entry, 0, sizeof(ResultEntry));
	entry->ln.next = entry->ln.prev = NULL;
	entry->conn = conn;
	entry->result = event->result;
	entry->subtxid = GetCurrentSubTransactionId();

	/* Add entry as new head and set instance data */
	list_insert_after(&entry->ln, &conn->results);
	PQresultSetInstanceData(event->result, eventproc, entry);

	elog(DEBUG3,
		 "created result %p on connection %p subtxid %u",
		 event->result,
		 conn,
		 entry->subtxid);

	connstats.results_created++;

	return EVENTPROC_SUCCESS;
}

/*
 * Invoked on PQclear(result). Removes the PGresult from the list in the
 * parent TSConnection.
 */
static int
handle_result_destroy(PGEventResultDestroy *event)
{
	ResultEntry *entry = PQresultInstanceData(event->result, eventproc);

	Assert(NULL != entry);

	/* Detach entry */
	list_detach(&entry->ln);

	elog(DEBUG3, "destroyed result %p for subtxnid %u", entry->result, entry->subtxid);

	free(entry);

	connstats.results_cleared++;

	return EVENTPROC_SUCCESS;
}

/*
 * Main event handler invoked when events happen on a PGconn.
 *
 * According to the libpq API, the function should return a non-zero value if
 * it succeeds and zero if it fails. We use EVENTPROC_SUCCESS and
 * EVENTPROC_FAILURE in place of these two options.
 */
static int
eventproc(PGEventId eventid, void *eventinfo, void *data)
{
	int res = EVENTPROC_SUCCESS;

	switch (eventid)
	{
		case PGEVT_CONNDESTROY:
			res = handle_conn_destroy((PGEventConnDestroy *) eventinfo);
			break;
		case PGEVT_RESULTCREATE:
			res = handle_result_create((PGEventResultCreate *) eventinfo);
			break;
		case PGEVT_RESULTDESTROY:
			res = handle_result_destroy((PGEventResultDestroy *) eventinfo);
			break;
		default:
			/* Not of interest, so return success */
			break;
	}

	return res;
}

static PQconninfoOption *
get_libpq_options()
{
	/* make static to fetch once per backend */
	static PQconninfoOption *libpq_options = NULL;

	if (libpq_options == NULL)
	{
		/* Note that the options array is Malloc'ed */
		libpq_options = PQconndefaults();
	}

	if (libpq_options == NULL)
	{
		/* probably OOM */
		elog(ERROR, "could not get default libpq options");
	}

	return libpq_options;
}

static void
unset_libpq_envvar(void)
{
	PQconninfoOption *lopt;
	PQconninfoOption *options = PQconndefaults();

	Assert(options != NULL);

	/* Explicitly unset all libpq environment variables.
	 *
	 * By default libpq uses environment variables as a fallback
	 * to specify connection options, potentially they could be in
	 * a conflict with PostgreSQL variables and introduce
	 * security risks.
	 */
	for (lopt = options; lopt->keyword; lopt++)
	{
		if (lopt->envvar)
			unsetenv(lopt->envvar);
	}

	PQconninfoFree(options);
}

static bool
is_libpq_option(const char *keyword, char **display_option)
{
	PQconninfoOption *lopt;

	for (lopt = get_libpq_options(); lopt->keyword; lopt++)
	{
		if (strcmp(lopt->keyword, keyword) == 0)
		{
			if (display_option != NULL)
				*display_option = lopt->dispchar;
			return true;
		}
	}
	return false;
}

ConnOptionType
remote_connection_option_type(const char *keyword)
{
	char *display_option;

	if (!is_libpq_option(keyword, &display_option))
		return CONN_OPTION_TYPE_NONE;

	/* Hide debug options, as well as settings we override internally. */
	if (strchr(display_option, 'D') || strcmp(keyword, "fallback_application_name") == 0 ||
		strcmp(keyword, "client_encoding") == 0)
		return CONN_OPTION_TYPE_NONE;

	/*
	 * "user" and any secret options are allowed only on user mappings.
	 * Everything else is a data node option.
	 */
	if (strchr(display_option, '*') || strcmp(keyword, "user") == 0)
		return CONN_OPTION_TYPE_USER;

	return CONN_OPTION_TYPE_NODE;
}

bool
remote_connection_valid_user_option(const char *keyword)
{
	return remote_connection_option_type(keyword) == CONN_OPTION_TYPE_USER;
}

bool
remote_connection_valid_node_option(const char *keyword)
{
	return remote_connection_option_type(keyword) == CONN_OPTION_TYPE_NODE;
}

static int
extract_connection_options(List *defelems, const char **keywords, const char **values,
						   const char **user)
{
	ListCell *lc;
	int option_pos = 0;

	Assert(keywords != NULL);
	Assert(values != NULL);
	Assert(user != NULL);

	*user = NULL;
	foreach (lc, defelems)
	{
		DefElem *d = (DefElem *) lfirst(lc);

		if (is_libpq_option(d->defname, NULL))
		{
			keywords[option_pos] = d->defname;
			values[option_pos] = defGetString(d);
			if (strcmp(d->defname, "user") == 0)
			{
				Assert(*user == NULL);
				*user = values[option_pos];
			}
			option_pos++;
		}
	}

	Assert(*user != NULL);
	return option_pos;
}

/*
 * Internal connection configure.
 *
 * This function will send internal configuration settings if they have
 * changed. It is used to pass on configuration settings before executing a
 * command requested by module users.
 *
 * ATTENTION! This function should *not* use
 * `remote_connection_exec_ok_command` since this function is called
 * indirectly whenever a remote command is executed, which would lead to
 * infinite recursion. Stick to `PQ*` functions.
 *
 * Returns true if the current configuration is OK (no change) or was
 * successfully applied, otherwise false.
 */
bool
remote_connection_configure_if_changed(TSConnection *conn)
{
	const char *local_tz_name = pg_get_timezone_name(session_timezone);
	bool success = true;

	/*
	 * We need to enforce the same timezone setting across nodes. Otherwise,
	 * we might get the wrong result when we push down things like
	 * date_trunc(text, timestamptz). To safely do that, we also need the
	 * timezone databases to be the same on all data nodes.
	 *
	 * We save away the timezone name so that we know what we last sent over
	 * the connection. If the time zone changed since last time we sent a
	 * command, we will send a SET TIMEZONE command with the new timezone
	 * first.
	 */
	if (conn->tz_name == NULL ||
		(local_tz_name && pg_strcasecmp(conn->tz_name, local_tz_name) != 0))
	{
		char *set_timezone_cmd = psprintf("SET TIMEZONE = '%s'", local_tz_name);
		PGresult *result = PQexec(conn->pg_conn, set_timezone_cmd);

		success = PQresultStatus(result) == PGRES_COMMAND_OK;
		PQclear(result);
		pfree(set_timezone_cmd);
		free(conn->tz_name);
		conn->tz_name = strdup(local_tz_name);
	}

	return success;
}

/*
 * Default options/commands to set on every new connection.
 *
 * Timezone is indirectly set with the first command executed.
 */
static const char *default_connection_options[] = {
	/* Force the search path to contain only pg_catalog (see deparse.c) */
	"SET search_path = pg_catalog",
	/*
	 * Set values needed to ensure unambiguous data output from remote.  (This
	 * logic should match what pg_dump does.  See also set_transmission_modes
	 * in fdw.c.)
	 */
	"SET datestyle = ISO",
	"SET intervalstyle = postgres",
	"SET extra_float_digits = 3",
	NULL,
};

/*
 * Issue SET commands to make sure remote session is configured properly.
 *
 * We do this just once at connection, assuming nothing will change the
 * values later.  Since we'll never send volatile function calls to the
 * remote, there shouldn't be any way to break this assumption from our end.
 * It's possible to think of ways to break it at the remote end, eg making a
 * foreign table point to a view that includes a set_config call --- but once
 * you admit the possibility of a malicious view definition, there are any
 * number of ways to break things.
 */
bool
remote_connection_configure(TSConnection *conn)
{
	const char *cmd;
	StringInfoData sql;
	PGresult *result;
	bool success = true;
	int i = 0;

	initStringInfo(&sql);

	while ((cmd = default_connection_options[i]) != NULL)
	{
		appendStringInfo(&sql, "%s;", cmd);
		i++;
	}

	result = PQexec(conn->pg_conn, sql.data);
	success = PQresultStatus(result) == PGRES_COMMAND_OK;
	PQclear(result);

	return success;
}

static TSConnection *
remote_connection_create(PGconn *pg_conn, bool processing, const char *node_name)
{
	TSConnection *conn = malloc(sizeof(TSConnection));
	int ret;

	if (NULL == conn)
		return NULL;

	MemSet(conn, 0, sizeof(TSConnection));

	/* Must register the event procedure before attaching any instance data */
	ret = PQregisterEventProc(pg_conn, eventproc, "remote connection", conn);

	if (ret == 0)
	{
		free(conn);
		return NULL;
	}

	ret = PQsetInstanceData(pg_conn, eventproc, conn);
	Assert(ret != 0);

	conn->ln.next = conn->ln.prev = NULL;
	conn->pg_conn = pg_conn;
	conn->closing_guard = false;
	conn->processing = processing;
	namestrcpy(&conn->node_name, node_name);
	conn->tz_name = NULL;
	conn->autoclose = true;
	conn->subtxid = GetCurrentSubTransactionId();
	conn->xact_depth = 0;
	conn->xact_transitioning = false;
	/* Initialize results head */
	conn->results.next = &conn->results;
	conn->results.prev = &conn->results;
	list_insert_after(&conn->ln, &connections);

	elog(DEBUG3, "created connection %p", conn);

	connstats.connections_created++;

	return conn;
}

/*
 * Set the auto-close behavior.
 *
 * If set, the connection will be closed at the end of the (sub-)transaction
 * it was created on.
 *
 * The default value is on (true).
 *
 * Returns the previous setting.
 */
bool
remote_connection_set_autoclose(TSConnection *conn, bool autoclose)
{
	bool old = conn->autoclose;

	conn->autoclose = autoclose;
	return old;
}

int
remote_connection_xact_depth_get(const TSConnection *conn)
{
	Assert(conn->xact_depth >= 0);
	return conn->xact_depth;
}

int
remote_connection_xact_depth_inc(TSConnection *conn)
{
	Assert(conn->xact_depth >= 0);
	return ++conn->xact_depth;
}

int
remote_connection_xact_depth_dec(TSConnection *conn)
{
	Assert(conn->xact_depth > 0);
	return --conn->xact_depth;
}

void
remote_connection_xact_transition_begin(TSConnection *conn)
{
	Assert(!conn->xact_transitioning);
	conn->xact_transitioning = true;
}

void
remote_connection_xact_transition_end(TSConnection *conn)
{
	Assert(conn->xact_transitioning);
	conn->xact_transitioning = false;
}

bool
remote_connection_xact_is_transitioning(const TSConnection *conn)
{
	return conn->xact_transitioning;
}

PGconn *
remote_connection_get_pg_conn(const TSConnection *conn)
{
	Assert(conn != NULL);
	return conn->pg_conn;
}

bool
remote_connection_is_processing(const TSConnection *conn)
{
	Assert(conn != NULL);
	return conn->processing;
}

void
remote_connection_set_processing(TSConnection *conn, bool processing)
{
	Assert(conn != NULL);
	conn->processing = processing;
}

static void
remote_elog(int elevel, int errorcode, const char *node_name, const char *primary,
			const char *detail, const char *hint, const char *context, const char *sql)
{
	ereport(elevel,
			(errcode(errorcode),
			 primary ? errmsg_internal("[%s]: %s", node_name, primary) :
					   errmsg("could not obtain message string for remote error"),
			 detail ? errdetail_internal("%s", detail) : 0,
			 hint ? errhint("%s", hint) : 0,
			 context ? errcontext("%s", context) : 0,
			 sql ? errcontext("Remote SQL command: %s", sql) : 0));
}

const char *
remote_connection_node_name(const TSConnection *conn)
{
	return NameStr(conn->node_name);
}

void
remote_connection_elog(TSConnection *conn, int elevel)
{
	const char *msg = pchomp(PQerrorMessage(conn->pg_conn));

	remote_elog(elevel,
				ERRCODE_CONNECTION_FAILURE,
				NameStr(conn->node_name),
				msg,
				NULL,
				NULL,
				NULL,
				NULL);
}

/*
 * Report an error we got from the remote host.
 *
 * elevel: error level to use (typically ERROR, but might be less)
 * res: PGresult containing the error
 *
 * Note: callers that choose not to throw ERROR for a remote error are
 * responsible for making sure that the associated ConnCacheEntry gets
 * marked with have_error = true.
 */
void
remote_result_elog(PGresult *res, int elevel)
{
	ResultEntry *entry = PQresultInstanceData(res, eventproc);
	const char *sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
	const char *primary = PQresultErrorField(res, PG_DIAG_MESSAGE_PRIMARY);
	const char *detail = PQresultErrorField(res, PG_DIAG_MESSAGE_DETAIL);
	const char *hint = PQresultErrorField(res, PG_DIAG_MESSAGE_HINT);
	const char *context = PQresultErrorField(res, PG_DIAG_CONTEXT);
	const char *stmt = PQresultErrorField(res, PG_DIAG_STATEMENT_POSITION);
	TSConnection *conn;
	int code;

	PG_TRY();
	{
		if (NULL == entry)
			elog(ERROR, "unexpected result object in error handler");

		conn = entry->conn;
		Assert(NULL != conn);

		if (sqlstate && strlen(sqlstate) == 5)
			code = MAKE_SQLSTATE(sqlstate[0], sqlstate[1], sqlstate[2], sqlstate[3], sqlstate[4]);
		else
			code = ERRCODE_CONNECTION_FAILURE;

		/*
		 * If we don't get a message from the PGresult, try the PGconn.  This
		 * is needed because for connection-level failures, PQexec may just
		 * return NULL, not a PGresult at all.
		 */
		if (primary == NULL)
			primary = pchomp(PQerrorMessage(conn->pg_conn));

		remote_elog(elevel, code, NameStr(conn->node_name), primary, detail, hint, context, stmt);
	}
	PG_CATCH();
	{
		PQclear(res);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * Execute a remote command.
 *
 * Like PQexec, which this functions uses internally, the PGresult returned
 * describes only the last command executed in a multi-command string.
 */
PGresult *
remote_connection_exec(TSConnection *conn, const char *cmd)
{
	if (!remote_connection_configure_if_changed(conn))
	{
		PGresult *res = PQmakeEmptyPGresult(conn->pg_conn, PGRES_FATAL_ERROR);
		PQfireResultCreateEvents(conn->pg_conn, res);
		return res;
	}

	return PQexec(conn->pg_conn, cmd);
}

/*
 * Must be a macro since va_start() must be called in the function that takes
 * a variable number of arguments.
 */
#define stringinfo_va(fmt, sql)                                                                    \
	do                                                                                             \
	{                                                                                              \
		initStringInfo((sql));                                                                     \
		for (;;)                                                                                   \
		{                                                                                          \
			va_list args;                                                                          \
			int needed;                                                                            \
			va_start(args, fmt);                                                                   \
			needed = appendStringInfoVA((sql), fmt, args);                                         \
			va_end(args);                                                                          \
			if (needed == 0)                                                                       \
				break;                                                                             \
			/* Increase the buffer size and try again. */                                          \
			enlargeStringInfo((sql), needed);                                                      \
		}                                                                                          \
	} while (0);

/*
 * Execute a remote command.
 *
 * Like remote_connection_exec but takes a variable number of arguments.
 */
PGresult *
remote_connection_execf(TSConnection *conn, const char *fmt, ...)
{
	PGresult *res;
	StringInfoData sql;

	stringinfo_va(fmt, &sql);
	res = remote_connection_exec(conn, sql.data);
	pfree(sql.data);

	return res;
}

PGresult *
remote_connection_queryf_ok(TSConnection *conn, const char *fmt, ...)
{
	StringInfoData sql;
	PGresult *res;

	stringinfo_va(fmt, &sql);
	res = remote_result_query_ok(remote_connection_exec(conn, sql.data));
	pfree(sql.data);
	return res;
}

PGresult *
remote_connection_query_ok(TSConnection *conn, const char *query)
{
	return remote_result_query_ok(remote_connection_exec(conn, query));
}

void
remote_connection_cmd_ok(TSConnection *conn, const char *cmd)
{
	remote_result_cmd_ok(remote_connection_exec(conn, cmd));
}

void
remote_connection_cmdf_ok(TSConnection *conn, const char *fmt, ...)
{
	StringInfoData sql;

	stringinfo_va(fmt, &sql);
	remote_result_cmd_ok(remote_connection_exec(conn, sql.data));
	pfree(sql.data);
}

static PGresult *
remote_result_ok(PGresult *res, ExecStatusType expected)
{
	if (PQresultStatus(res) != expected)
		remote_result_elog(res, ERROR);

	return res;
}

void
remote_result_cmd_ok(PGresult *res)
{
	PQclear(remote_result_ok(res, PGRES_COMMAND_OK));
}

PGresult *
remote_result_query_ok(PGresult *res)
{
	return remote_result_ok(res, PGRES_TUPLES_OK);
}

/**
 * Validate extension version.
 */
void
remote_validate_extension_version(TSConnection *conn, const char *data_node_version)
{
	bool old_version;

	if (!dist_util_is_compatible_version(data_node_version, TIMESCALEDB_VERSION, &old_version))
		ereport(ERROR,
				(errcode(ERRCODE_TS_DATA_NODE_INVALID_CONFIG),
				 errmsg("remote PostgreSQL instance has an incompatible timescaledb extension "
						"version"),
				 errdetail_internal("Access node version: %s, remote version: %s.",
									TIMESCALEDB_VERSION_MOD,
									data_node_version)));
	if (old_version)
		ereport(WARNING,
				(errmsg("remote PostgreSQL instance has an outdated timescaledb extension version"),
				 errdetail_internal("Access node version: %s, remote version: %s.",
									TIMESCALEDB_VERSION_MOD,
									data_node_version)));
}

/*
 * Check timescaledb extension version on a data node.
 *
 * Compare remote connection extension version with the one installed
 * locally on the access node.
 *
 * Return false if extension is not found, true otherwise.
 */
bool
remote_connection_check_extension(TSConnection *conn)
{
	PGresult *res;

	res = remote_connection_execf(conn,
								  "SELECT extversion FROM pg_extension WHERE extname = %s",
								  quote_literal_cstr(EXTENSION_NAME));

	/* Just to capture any bugs in the SELECT above */
	Assert(PQnfields(res) == 1);

	switch (PQntuples(res))
	{
		case 0: /* extension does not exists */
			PQclear(res);
			return false;

		case 1:
			break;

		default: /* something strange happend */
			ereport(WARNING,
					(errcode(ERRCODE_TS_DATA_NODE_INVALID_CONFIG),
					 errmsg("more than one TimescaleDB extension loaded")));
			break;
	}

	/* validate extension version on data node and make sure that it is
	 * compatible */
	remote_validate_extension_version(conn, PQgetvalue(res, 0, 0));

	PQclear(res);
	return true;
}

/*
 * Configure remote connection using current instance UUID.
 *
 * This allows remote side to reason about whether this connection has been
 * originated by access node.
 *
 * Returns true on success and false on error, in which case the optional
 * errmsg parameter can be used to retrieve an error message.
 */
static bool
remote_connection_set_peer_dist_id(TSConnection *conn)
{
	Datum id_string = DirectFunctionCall1(uuid_out, ts_telemetry_metadata_get_uuid());
	PGresult *res;
	bool success = true;

	res = remote_connection_execf(conn,
								  "SELECT * FROM _timescaledb_internal.set_peer_dist_id('%s')",
								  DatumGetCString(id_string));
	success = PQresultStatus(res) == PGRES_TUPLES_OK;
	PQclear(res);

	return success;
}

/* fallback_application_name, client_encoding, end marker */
#define REMOTE_CONNECTION_SESSION_OPTIONS_N 3

/* passfile */
#define REMOTE_CONNECTION_PASSWORD_OPTIONS_N 1

/* sslmode, sslrootcert, sslcert, sslkey */
#define REMOTE_CONNECTION_SSL_OPTIONS_N 4

/* default password file basename */
#define DEFAULT_PASSFILE_NAME "passfile"

static void
set_password_options(const char **keywords, const char **values, int *option_start)
{
	int option_pos = *option_start;

	/* Set user specified password file path using timescaledb.passfile or
	 * use default path assuming that the file is stored in the
	 * data directory */
	keywords[option_pos] = "passfile";
	if (ts_guc_passfile)
		values[option_pos] = ts_guc_passfile;
	else
		values[option_pos] = psprintf("%s/" DEFAULT_PASSFILE_NAME, DataDir);
	option_pos++;

	*option_start = option_pos;
}

typedef enum PathKind
{
	PATH_KIND_CRT,
	PATH_KIND_KEY
} PathKind;

/* Path description for human consumption */
static const char *path_kind_text[PATH_KIND_KEY + 1] = {
	[PATH_KIND_CRT] = "certificate",
	[PATH_KIND_KEY] = "private key",
};

/* Path extension string for file system */
static const char *path_kind_ext[PATH_KIND_KEY + 1] = {
	[PATH_KIND_CRT] = "crt",
	[PATH_KIND_KEY] = "key",
};

/*
 * Helper function to report error.
 *
 * This is needed to avoid code coverage reporting low coverage for error
 * cases in `make_user_path` that cannot be reached in normal situations.
 */
static void
report_path_error(PathKind path_kind, const char *user_name)
{
	elog(ERROR,
		 "cannot write %s for user \"%s\": path too long",
		 path_kind_text[path_kind],
		 user_name);
}

/*
 * Make a user path with the given extension and user name in a portable and
 * safe manner.
 *
 * We use MD5 to compute a filename for the user name, which allows all forms
 * of user names. It is not necessary for the function to be cryptographically
 * secure, only to have a low risk of collisions, and MD5 is fast and with a
 * low risk of collisions.
 *
 * Will return the resulting path, or abort with an error.
 */
static StringInfo
make_user_path(const char *user_name, PathKind path_kind)
{
	const char *ssl_dir = ts_guc_ssl_dir ? ts_guc_ssl_dir : DataDir;
	char ret_path[MAXPGPATH];
	char hexsum[33];
	StringInfo result;

	pg_md5_hash(user_name, strlen(user_name), hexsum);

	if (strlcpy(ret_path, ssl_dir, MAXPGPATH) > MAXPGPATH)
		report_path_error(path_kind, user_name);

	canonicalize_path(ret_path);

	join_path_components(ret_path, ret_path, EXTENSION_NAME);
	join_path_components(ret_path, ret_path, "certs");
	join_path_components(ret_path, ret_path, hexsum);

	result = makeStringInfo();
	appendStringInfo(result, "%s.%s", ret_path, path_kind_ext[path_kind]);
	return result;
}

static void
set_ssl_options(const char *user_name, const char **keywords, const char **values,
				int *option_start)
{
	int option_pos = *option_start;
	const char *ssl_enabled;
	const char *ssl_ca_file;

	ssl_enabled = GetConfigOption("ssl", true, false);

	if (!ssl_enabled || strcmp(ssl_enabled, "on") != 0)
		return;

	/* If SSL is enabled on AN then we assume it is also should be used for DN
	 * connections as well, otherwise we need to introduce some other way to
	 * control it */
	keywords[option_pos] = "sslmode";
	values[option_pos] = "require";
	option_pos++;

	ssl_ca_file = GetConfigOption("ssl_ca_file", true, false);

	/* Use ssl_ca_file as the root certificate when verifying the
	 * data node we connect to */
	if (ssl_ca_file)
	{
		keywords[option_pos] = "sslrootcert";
		values[option_pos] = ssl_ca_file;
		option_pos++;
	}

	/* Search for the user certificate in the user subdirectory of either
	 * timescaledb.ssl_dir or data directory. The user subdirectory is
	 * currently hardcoded. */

	keywords[option_pos] = "sslcert";
	values[option_pos] = make_user_path(user_name, PATH_KIND_CRT)->data;
	option_pos++;

	keywords[option_pos] = "sslkey";
	values[option_pos] = make_user_path(user_name, PATH_KIND_KEY)->data;
	option_pos++;

	*option_start = option_pos;
}

/*
 * This will only open a connection to a specific node, but not do anything
 * else. In particular, it will not perform any validation nor configure the
 * connection since it cannot know that it connects to a data node database or
 * not. For that, please use the `remote_connection_open_with_options`
 * function.
 */
TSConnection *
remote_connection_open_with_options_nothrow(const char *node_name, List *connection_options)
{
	PGconn *pg_conn = NULL;
	const char *user_name = NULL;
	TSConnection *ts_conn;
	const char **keywords;
	const char **values;
	int option_count;
	int option_pos;

	/*
	 * Construct connection params from generic options of ForeignServer
	 * and user. (Some of them might not be libpq options, in
	 * which case we'll just waste a few array slots.)  Add 3 extra slots
	 * for fallback_application_name, client_encoding, end marker.
	 * One additional slot to set passfile and 4 slots for ssl options.
	 */
	option_count = list_length(connection_options) + REMOTE_CONNECTION_SESSION_OPTIONS_N +
				   REMOTE_CONNECTION_PASSWORD_OPTIONS_N + REMOTE_CONNECTION_SSL_OPTIONS_N;
	keywords = (const char **) palloc(option_count * sizeof(char *));
	values = (const char **) palloc(option_count * sizeof(char *));

	option_pos = extract_connection_options(connection_options, keywords, values, &user_name);

	/* Use the extension name as fallback_application_name. */
	keywords[option_pos] = "fallback_application_name";
	values[option_pos] = EXTENSION_NAME;
	option_pos++;

	/* Set client_encoding so that libpq can convert encoding properly. */
	keywords[option_pos] = "client_encoding";
	values[option_pos] = GetDatabaseEncodingName();
	option_pos++;

	/* Set passfile options */
	set_password_options(keywords, values, &option_pos);

	/* Set client specific SSL connection options */
	set_ssl_options(user_name, keywords, values, &option_pos);

	/* Set end marker */
	keywords[option_pos] = values[option_pos] = NULL;
	Assert(option_pos <= option_count);

	pg_conn = PQconnectdbParams(keywords, values, false);

	/* Cast to (char **) to silence warning with MSVC compiler */
	pfree((char **) keywords);
	pfree((char **) values);

	if (NULL == pg_conn)
		return NULL;

	ts_conn = remote_connection_create(pg_conn, false, node_name);

	if (NULL == ts_conn)
		PQfinish(pg_conn);

	return ts_conn;
}

/*
 * Opens a connection.
 *
 * Raw connections are not part of the transaction and do not have transactions
 * auto-started. They must be explicitly closed by
 * remote_connection_close. Note that connections are allocated using malloc
 * and so if you do not call remote_connection_close, you'll have a memory
 * leak. Note that the connection cache handles all of this for you so use
 * that if you can.
 */
TSConnection *
remote_connection_open_with_options(const char *node_name, List *connection_options,
									bool set_dist_id)
{
	TSConnection *conn = remote_connection_open_with_options_nothrow(node_name, connection_options);

	if (NULL == conn)
		ereport(ERROR,
				(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
				 errmsg("could not connect to \"%s\"", node_name)));

	/*
	 * Use PG_TRY block to ensure closing connection on error.
	 */
	PG_TRY();
	{
		Assert(NULL != conn->pg_conn);

		if (PQstatus(conn->pg_conn) != CONNECTION_OK)
			ereport(ERROR,
					(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
					 errmsg("could not connect to \"%s\"", node_name),
					 errdetail_internal("%s", pchomp(PQerrorMessage(conn->pg_conn)))));

		/* Prepare new session for use */
		if (!remote_connection_configure(conn))
			ereport(ERROR,
					(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
					 errmsg("could not configure remote connection to \"%s\"", node_name),
					 errdetail_internal("%s", PQerrorMessage(conn->pg_conn))));

		/* Check a data node extension version and show a warning
		 * message if it differs */
		remote_connection_check_extension(conn);

		if (set_dist_id)
		{
			/* Inform remote node about instance UUID */
			if (!remote_connection_set_peer_dist_id(conn))
				ereport(ERROR,
						(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
						 errmsg("could not set distributed ID for \"%s\"", node_name),
						 errdetail_internal("%s", PQerrorMessage(conn->pg_conn))));
		}
	}
	PG_CATCH();
	{
		/* Release PGconn data structure if we managed to create one */
		remote_connection_close(conn);
		PG_RE_THROW();
	}
	PG_END_TRY();

	return conn;
}

static List *
add_username_to_server_options(ForeignServer *server, Oid user_id)
{
	const char *user_name = GetUserNameFromId(user_id, false);
	List *server_options = list_copy(server->options);

	return lappend(server_options,
				   makeDefElem("user", (Node *) makeString(pstrdup(user_name)), -1));
}

TSConnection *
remote_connection_open_by_id(TSConnectionId id)
{
	ForeignServer *server = GetForeignServer(id.server_id);
	List *connection_options = add_username_to_server_options(server, id.user_id);

	return remote_connection_open_with_options(server->servername, connection_options, true);
}

TSConnection *
remote_connection_open(Oid server_id, Oid user_id)
{
	TSConnectionId id = remote_connection_id(server_id, user_id);

	return remote_connection_open_by_id(id);
}

/*
 * Open a connection without throwing and error.
 *
 * Returns the connection pointer on success. On failure NULL is returned and
 * the errmsg (if given) is used to return an error message.
 */
TSConnection *
remote_connection_open_nothrow(Oid server_id, Oid user_id, char **errmsg)
{
	ForeignServer *server = GetForeignServer(server_id);
	Oid fdwid = get_foreign_data_wrapper_oid(EXTENSION_FDW_NAME, false);
	List *connection_options;
	TSConnection *conn;

	if (server->fdwid != fdwid)
	{
		elog(WARNING, "invalid node type for \"%s\"", server->servername);
		return NULL;
	}

	connection_options = add_username_to_server_options(server, user_id);
	conn = remote_connection_open_with_options_nothrow(server->servername, connection_options);

	if (NULL == conn)
	{
		if (NULL != errmsg)
			*errmsg = "internal connection error";
		return NULL;
	}

	if (PQstatus(conn->pg_conn) != CONNECTION_OK || !remote_connection_set_peer_dist_id(conn))
	{
		if (NULL != errmsg)
			*errmsg = pchomp(PQerrorMessage(conn->pg_conn));
		remote_connection_close(conn);
		return NULL;
	}

	return conn;
}

#define PING_QUERY "SELECT 1"

bool
remote_connection_ping(const char *node_name)
{
	Oid server_id = get_foreign_server_oid(node_name, false);
	TSConnection *conn = remote_connection_open_nothrow(server_id, GetUserId(), NULL);
	bool success = false;

	if (NULL == conn)
		return false;

	if (PQstatus(conn->pg_conn) == CONNECTION_OK)
	{
		if (1 == PQsendQuery(conn->pg_conn, PING_QUERY))
		{
			PGresult *res = PQgetResult(conn->pg_conn);

			success = (PQresultStatus(res) == PGRES_TUPLES_OK);
			PQclear(res);
		}
	}

	remote_connection_close(conn);

	return success;
}

void
remote_connection_close(TSConnection *conn)
{
	Assert(conn != NULL);

	conn->closing_guard = true;

	if (NULL != conn->pg_conn)
		PQfinish(conn->pg_conn);

	/* Assert that PQfinish detached this connection from the global list of
	 * connections */
	Assert(IS_DETACHED_ENTRY(&conn->ln));
	remote_connection_free(conn);
}

/*
 * Assign a "unique" number for a cursor.
 *
 * TODO should this be moved into the session?
 *
 * These really only need to be unique per connection within a transaction.
 * For the moment we ignore the per-connection point and assign them across
 * all connections in the transaction, but we ask for the connection to be
 * supplied in case we want to refine that.
 *
 * Note that even if wraparound happens in a very long transaction, actual
 * collisions are highly improbable; just be sure to use %u not %d to print.
 */
unsigned int
remote_connection_get_cursor_number()
{
	return ++cursor_number;
}

void
remote_connection_reset_cursor_number()
{
	cursor_number = 0;
}

/*
 * Assign a "unique" number for a prepared statement.
 *
 * This works much like remote_connection_get_cursor_number, except that we never reset the counter
 * within a session.  That's because we can't be 100% sure we've gotten rid
 * of all prepared statements on all connections, and it's not really worth
 * increasing the risk of prepared-statement name collisions by resetting.
 */
unsigned int
remote_connection_get_prep_stmt_number()
{
	return ++prep_stmt_number;
}

/*
 * Drain a connection of all data coming in and discard the results. Return
 * success if all data is drained before the deadline expires.
 *
 * This is mainly used in abort processing. This result being returned
 * might be for a query that is being interrupted by transaction abort, or it might
 * be a query that was initiated as part of transaction abort to get the remote
 * side back to the appropriate state.
 *
 * It's not a huge problem if we throw an ERROR here, but if we get into error
 * recursion trouble, we'll end up slamming the connection shut, which will
 * necessitate failing the entire toplevel transaction even if subtransactions
 * were used.  Try to use WARNING where we can.
 *
 * end_time is the time at which we should give up and assume the remote
 * side is dead.
 */

static bool
remote_connection_drain(PGconn *conn, TimestampTz endtime)
{
	for (;;)
	{
		PGresult *res;

		while (PQisBusy(conn))
		{
			int wc;
			TimestampTz now = GetCurrentTimestamp();
			long secs;
			int microsecs;
			long cur_timeout;

			/* If timeout has expired, give up, else get sleep time. */
			if (now >= endtime)
			{
				elog(WARNING, "timeout occured while trying to drain the connection");
				return false;
			}

			TimestampDifference(now, endtime, &secs, &microsecs);

			/* To protect against clock skew, limit sleep to one minute. */
			cur_timeout = Min(60000, secs * USECS_PER_SEC + microsecs);

			/* Sleep until there's something to do */
			wc = WaitLatchOrSocket(MyLatch,
								   WL_LATCH_SET | WL_SOCKET_READABLE | WL_TIMEOUT |
									   WL_POSTMASTER_DEATH,
								   PQsocket(conn),
								   cur_timeout,
								   PG_WAIT_EXTENSION);
			ResetLatch(MyLatch);

			CHECK_FOR_INTERRUPTS();

			if (wc & WL_POSTMASTER_DEATH)
			{
				/* Postmaster died -> exit) */
				return false;
			}

			/* Data available in socket? */
			if (wc & WL_SOCKET_READABLE)
			{
				if (!PQconsumeInput(conn))
					/* connection trouble; treat the same as a timeout */
					return false;
			}
		}

		res = PQgetResult(conn);
		if (res == NULL)
			return true; /* query is complete */
		PQclear(res);
	}
	Assert(false);
}

/*
 * Cancel the currently-in-progress query and ignore the result.  Returns true if we successfully
 * cancel the query and discard any pending result, and false if not.
 */
bool
remote_connection_cancel_query(TSConnection *conn)
{
	PGcancel *cancel;
	char errbuf[256];
	TimestampTz endtime;

	if (!conn)
		return true;

	/*
	 * If it takes too long to cancel the query and discard the result, assume
	 * the connection is dead.
	 */
	endtime = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), 30000);

	/* We assume that processing is over no matter if cancel completes
	 * successfully or not. */
	remote_connection_set_processing(conn, false);

	/*
	 * Issue cancel request.  Unfortunately, there's no good way to limit the
	 * amount of time that we might block inside PQcancel().
	 */
	if ((cancel = PQgetCancel(conn->pg_conn)))
	{
		if (!PQcancel(cancel, errbuf, sizeof(errbuf)))
		{
			ereport(WARNING,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("could not send cancel request: %s", errbuf)));
			PQfreeCancel(cancel);
			return false;
		}
		PQfreeCancel(cancel);
	}

	return remote_connection_drain(conn->pg_conn, endtime);
}

void
remote_result_close(PGresult *res)
{
	PQclear(res);
}

/*
 * Cleanup connections and results at the end of a (sub-)transaction.
 *
 * This function is called at the end of transactions and sub-transactions to
 * auto-cleanup connections and result objects.
 */
static void
remote_connections_cleanup(SubTransactionId subtxid, bool isabort)
{
	ListNode *curr = connections.next;
	unsigned int num_connections = 0;
	unsigned int num_results = 0;

	while (curr != &connections)
	{
		TSConnection *conn = (TSConnection *) curr;

		/* Move to next connection since closing the current one might
		 * otherwise make the curr pointer invalid. */
		curr = curr->next;

		if (conn->autoclose && (subtxid == InvalidSubTransactionId || subtxid == conn->subtxid))
		{
			/* Closes the connection and frees all its PGresult objects */
			remote_connection_close(conn);
			num_connections++;
		}
		else
		{
			/* We're not closing the connection, but we should clean up any
			 * lingering results */
			ListNode *curr_result = conn->results.next;

			while (curr_result != &conn->results)
			{
				ResultEntry *entry = (ResultEntry *) curr_result;

				curr_result = curr_result->next;

				if (subtxid == InvalidSubTransactionId || subtxid == entry->subtxid)
				{
					PQclear(entry->result);
					num_results++;
				}
			}
		}
	}

	if (subtxid == InvalidSubTransactionId)
		elog(DEBUG3,
			 "cleaned up %u connections and %u results at %s of transaction",
			 num_connections,
			 num_results,
			 isabort ? "abort" : "commit");
	else
		elog(DEBUG3,
			 "cleaned up %u connections and %u results at %s of sub-transaction %u",
			 num_connections,
			 num_results,
			 isabort ? "abort" : "commit",
			 subtxid);
}

static void
remote_connection_xact_end(XactEvent event, void *unused_arg)
{
	switch (event)
	{
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
			remote_connections_cleanup(InvalidSubTransactionId, true);
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
			remote_connections_cleanup(InvalidSubTransactionId, false);
			break;
		default:
			break;
	}
}

static void
remote_connection_subxact_end(SubXactEvent event, SubTransactionId subtxid,
							  SubTransactionId parent_subtxid, void *unused_arg)
{
	switch (event)
	{
		case SUBXACT_EVENT_ABORT_SUB:
			remote_connections_cleanup(subtxid, true);
			break;
		case SUBXACT_EVENT_COMMIT_SUB:
			remote_connections_cleanup(subtxid, false);
			break;
		default:
			break;
	}
}

bool
remote_connection_set_single_row_mode(TSConnection *conn)
{
	return PQsetSingleRowMode(conn->pg_conn);
}

#if TS_DEBUG
/*
 * Reset the current connection stats.
 */
void
remote_connection_stats_reset(void)
{
	MemSet(&connstats, 0, sizeof(RemoteConnectionStats));
}

/*
 * Get the current connection stats.
 */
RemoteConnectionStats *
remote_connection_stats_get(void)
{
	return &connstats;
}
#endif

void
_remote_connection_init(void)
{
	RegisterXactCallback(remote_connection_xact_end, NULL);
	RegisterSubXactCallback(remote_connection_subxact_end, NULL);

	unset_libpq_envvar();
}

void
_remote_connection_fini(void)
{
	UnregisterXactCallback(remote_connection_xact_end, NULL);
	UnregisterSubXactCallback(remote_connection_subxact_end, NULL);
}
