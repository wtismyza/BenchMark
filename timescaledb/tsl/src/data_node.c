/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */
#include <postgres.h>

#include <access/htup_details.h>
#include <access/xact.h>
#include <catalog.h>
#include <catalog/namespace.h>
#include <catalog/pg_database.h>
#include <catalog/pg_foreign_server.h>
#include <catalog/pg_inherits.h>
#include <catalog/pg_namespace.h>
#include <chunk_data_node.h>
#include <commands/dbcommands.h>
#include <commands/defrem.h>
#include <commands/event_trigger.h>
#include <compat.h>
#include <extension.h>
#include <funcapi.h>
#include <hypertable_data_node.h>
#include <libpq/crypt.h>
#include <miscadmin.h>
#include <nodes/makefuncs.h>
#include <nodes/parsenodes.h>
#include <utils/acl.h>
#include <utils/builtins.h>
#include <utils/builtins.h>
#include <utils/guc.h>
#include <utils/inval.h>
#include <utils/syscache.h>

#include "fdw/fdw.h"
#include "remote/async.h"
#include "remote/connection.h"
#include "remote/connection_cache.h"
#include "data_node.h"
#include "remote/utils.h"
#include "hypertable.h"
#include "hypertable_cache.h"
#include "errors.h"
#include "dist_util.h"
#include "utils/uuid.h"
#include "mb/pg_wchar.h"
#include "chunk.h"

#define TS_DEFAULT_POSTGRES_PORT 5432
#define TS_DEFAULT_POSTGRES_HOST "localhost"

#define ERRCODE_DUPLICATE_DATABASE_STR "42P04"
#define ERRCODE_DUPLICATE_SCHEMA_STR "42P06"

typedef struct DbInfo
{
	NameData name;
	int32 encoding;
	NameData chartype;
	NameData collation;
} DbInfo;

static bool data_node_validate_database(TSConnection *conn, const DbInfo *database);

/*
 * get_database_info - given a database OID, look up info about the database
 *
 * Returns:
 *  True if a record for the OID was found, false otherwise.
 */
static bool
get_database_info(Oid dbid, DbInfo *database)
{
	HeapTuple dbtuple;
	Form_pg_database dbrecord;

	dbtuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dbid));

	if (!HeapTupleIsValid(dbtuple))
		return false;

	dbrecord = (Form_pg_database) GETSTRUCT(dbtuple);

	database->encoding = dbrecord->encoding;
	database->collation = dbrecord->datcollate;
	database->chartype = dbrecord->datctype;

	ReleaseSysCache(dbtuple);
	return true;
}

/*
 * Verify that server is TimescaleDB data node and perform optional ACL check.
 *
 * The function returns true iif the server is valid TimescaleDB data node and
 * the ACL check succeeds. Otherwise, false is returned, or, an error is thrown
 * if fail_on_aclcheck is set to true.
 */
static bool
validate_foreign_server(const ForeignServer *server, AclMode const mode, bool fail_on_aclcheck)
{
	Oid const fdwid = get_foreign_data_wrapper_oid(EXTENSION_FDW_NAME, false);
	Oid curuserid = GetUserId();
	AclResult aclresult;
	bool valid;

	Assert(NULL != server);
	if (server->fdwid != fdwid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("data node \"%s\" is not a TimescaleDB server", server->servername)));

	if (mode == ACL_NO_CHECK)
		return true;

	/* Must have permissions on the server object */
	aclresult = pg_foreign_server_aclcheck(server->serverid, curuserid, mode);

	valid = (aclresult == ACLCHECK_OK);

	if (!valid && fail_on_aclcheck)
		aclcheck_error(aclresult, OBJECT_FOREIGN_SERVER, server->servername);

	return valid;
}

/*
 * Lookup the foreign server by name
 */
ForeignServer *
data_node_get_foreign_server(const char *node_name, AclMode mode, bool fail_on_aclcheck,
							 bool missing_ok)
{
	ForeignServer *server;
	bool valid;

	if (node_name == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid node_name: cannot be NULL")));

	server = GetForeignServerByName(node_name, missing_ok);
	if (NULL == server)
		return NULL;

	valid = validate_foreign_server(server, mode, fail_on_aclcheck);

	if (mode != ACL_NO_CHECK && !valid)
		return NULL;

	return server;
}

ForeignServer *
data_node_get_foreign_server_by_oid(Oid server_oid, AclMode mode)
{
	ForeignServer *server = GetForeignServer(server_oid);
	bool PG_USED_FOR_ASSERTS_ONLY valid = validate_foreign_server(server, mode, true);
	Assert(valid); /* Should always be valid since we should see error otherwise */
	return server;
}

/*
 * Create a foreign server.
 *
 * Returns true if the server was created and set the `oid` to the server oid.
 */
static bool
create_foreign_server(const char *const node_name, const char *const host, int32 port,
					  const char *const dbname, bool if_not_exists, Oid *const oid)
{
	ForeignServer *server = NULL;
	ObjectAddress objaddr;
	CreateForeignServerStmt stmt = {
		.type = T_CreateForeignServerStmt,
		.servername = (char *) node_name,
		.fdwname = EXTENSION_FDW_NAME,
		.options = list_make3(makeDefElem("host", (Node *) makeString(pstrdup(host)), -1),
							  makeDefElem("port", (Node *) makeInteger(port), -1),
							  makeDefElem("dbname", (Node *) makeString(pstrdup(dbname)), -1)),
		.if_not_exists = if_not_exists,
	};

	if (NULL == host)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 (errmsg("invalid host"),
				  (errhint("A hostname or IP address must be specified when "
						   "a data node does not already exist.")))));

	if (if_not_exists)
	{
		server = data_node_get_foreign_server(node_name, ACL_NO_CHECK, false, true);

		if (NULL != server)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("data node \"%s\" already exists, skipping", node_name)));

			if (oid != NULL)
				*oid = server->serverid;
			return false;
		}
	}

	/* Permissions checks done in CreateForeignServer() */
	objaddr = CreateForeignServer(&stmt);

	/* CreateForeignServer returns InvalidOid if server already exists */
	if (!OidIsValid(objaddr.objectId))
	{
		Assert(if_not_exists);

		server = data_node_get_foreign_server(node_name, ACL_USAGE, true, false);

		if (oid != NULL)
			*oid = server->serverid;
		return false;
	}

	if (oid != NULL)
		*oid = objaddr.objectId;

	return true;
}

TSConnection *
data_node_get_connection(const char *const data_node, RemoteTxnPrepStmtOption const ps_opt,
						 bool transactional)
{
	const ForeignServer *server;
	TSConnectionId id;

	Assert(data_node != NULL);
	server = data_node_get_foreign_server(data_node, ACL_NO_CHECK, false, false);
	id = remote_connection_id(server->serverid, GetUserId());

	if (transactional)
		return remote_dist_txn_get_connection(id, ps_opt);

	return remote_connection_cache_get_connection(id);
}

/* Attribute numbers for datum returned by create_data_node() */
enum Anum_create_data_node
{
	Anum_create_data_node_name = 1,
	Anum_create_data_node_host,
	Anum_create_data_node_port,
	Anum_create_data_node_dbname,
	Anum_create_data_node_node_created,
	Anum_create_data_node_database_created,
	Anum_create_data_node_extension_created,
	_Anum_create_data_node_max,
};

#define Natts_create_data_node (_Anum_create_data_node_max - 1)

static Datum
create_data_node_datum(FunctionCallInfo fcinfo, const char *node_name, const char *host, int32 port,
					   const char *dbname, bool node_created, bool database_created,
					   bool extension_created)
{
	TupleDesc tupdesc;
	Datum values[_Anum_create_data_node_max];
	bool nulls[_Anum_create_data_node_max] = { false };
	HeapTuple tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in "
						"context that cannot accept type record")));

	tupdesc = BlessTupleDesc(tupdesc);
	values[AttrNumberGetAttrOffset(Anum_create_data_node_name)] = CStringGetDatum(node_name);
	values[AttrNumberGetAttrOffset(Anum_create_data_node_host)] = CStringGetTextDatum(host);
	values[AttrNumberGetAttrOffset(Anum_create_data_node_port)] = Int32GetDatum(port);
	values[AttrNumberGetAttrOffset(Anum_create_data_node_dbname)] = CStringGetDatum(dbname);
	values[AttrNumberGetAttrOffset(Anum_create_data_node_node_created)] =
		BoolGetDatum(node_created);
	values[AttrNumberGetAttrOffset(Anum_create_data_node_database_created)] =
		BoolGetDatum(database_created);
	values[AttrNumberGetAttrOffset(Anum_create_data_node_extension_created)] =
		BoolGetDatum(extension_created);
	tuple = heap_form_tuple(tupdesc, values, nulls);

	return HeapTupleGetDatum(tuple);
}

static Datum
create_hypertable_data_node_datum(FunctionCallInfo fcinfo, HypertableDataNode *node)
{
	TupleDesc tupdesc;
	Datum values[Natts_hypertable_data_node];
	bool nulls[Natts_hypertable_data_node] = { false };
	HeapTuple tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in "
						"context that cannot accept type record")));

	tupdesc = BlessTupleDesc(tupdesc);
	values[AttrNumberGetAttrOffset(Anum_hypertable_data_node_hypertable_id)] =
		Int32GetDatum(node->fd.hypertable_id);
	values[AttrNumberGetAttrOffset(Anum_hypertable_data_node_node_hypertable_id)] =
		Int32GetDatum(node->fd.node_hypertable_id);
	values[AttrNumberGetAttrOffset(Anum_hypertable_data_node_node_name)] =
		NameGetDatum(&node->fd.node_name);
	tuple = heap_form_tuple(tupdesc, values, nulls);

	return HeapTupleGetDatum(tuple);
}

static List *
create_data_node_options(const char *host, int32 port, const char *dbname, const char *user)
{
	DefElem *host_elm = makeDefElem("host", (Node *) makeString(pstrdup(host)), -1);
	DefElem *port_elm = makeDefElem("port", (Node *) makeInteger(port), -1);
	DefElem *dbname_elm = makeDefElem("dbname", (Node *) makeString(pstrdup(dbname)), -1);
	DefElem *user_elm = makeDefElem("user", (Node *) makeString(pstrdup(user)), -1);

	return list_make4(host_elm, port_elm, dbname_elm, user_elm);
}

/* Returns 'true' if the database was created. */
static bool
data_node_bootstrap_database(TSConnection *conn, const DbInfo *database)
{
	const char *const username = PQuser(remote_connection_get_pg_conn(conn));

	Assert(database);

	if (data_node_validate_database(conn, database))
	{
		/* If the database already existed on the remote node, we will log a
		 * notice and proceed since it is not an error if the database already
		 * existed on the remote node. */
		elog(NOTICE,
			 "database \"%s\" already exists on data node, skipping",
			 NameStr(database->name));
		return false;
	}

	/* Create the database with the user as owner. There is no need to
	 * validate the database after this command since it should be created
	 * correctly. */
	PGresult *res =
		remote_connection_execf(conn,
								"CREATE DATABASE %s ENCODING %s LC_COLLATE %s LC_CTYPE %s "
								"TEMPLATE template0 OWNER %s",
								quote_identifier(NameStr(database->name)),
								quote_identifier(pg_encoding_to_char(database->encoding)),
								quote_literal_cstr(NameStr(database->collation)),
								quote_literal_cstr(NameStr(database->chartype)),
								quote_identifier(username));
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		remote_result_elog(res, ERROR);
	return true;
}

/* Validate the database.
 *
 * Errors:
 *   Will abort with errors if the database exists but is not correctly set
 *   up.
 * Returns:
 *   true if the database exists and is valid
 *   false if it does not exist.
 */
static bool
data_node_validate_database(TSConnection *conn, const DbInfo *database)
{
	PGresult *res;
	uint32 actual_encoding;
	const char *actual_chartype;
	const char *actual_collation;

	res = remote_connection_execf(conn,
								  "SELECT encoding, datcollate, datctype "
								  "FROM pg_database WHERE datname = %s",
								  quote_literal_cstr(NameStr(database->name)));

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_EXCEPTION), errmsg("%s", PQresultErrorMessage(res))));

	if (PQntuples(res) == 0)
		return false;

	Assert(PQnfields(res) > 2);

	actual_encoding = atoi(PQgetvalue(res, 0, 0));
	if (actual_encoding != database->encoding)
		ereport(ERROR,
				(errcode(ERRCODE_TS_DATA_NODE_INVALID_CONFIG),
				 errmsg("database exists but has wrong encoding"),
				 errdetail("Expected database encoding to be \"%s\" (%u) but it was \"%s\" (%u)",
						   pg_encoding_to_char(database->encoding),
						   database->encoding,
						   pg_encoding_to_char(actual_encoding),
						   actual_encoding)));

	actual_collation = PQgetvalue(res, 0, 1);
	Assert(actual_collation != NULL);
	if (strcmp(actual_collation, NameStr(database->collation)) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_TS_DATA_NODE_INVALID_CONFIG),
				 errmsg("database exists but has wrong collation"),
				 errdetail("Expected collation \"%s\" but it was \"%s\"",
						   NameStr(database->collation),
						   actual_collation)));

	actual_chartype = PQgetvalue(res, 0, 2);
	Assert(actual_chartype != NULL);
	if (strcmp(actual_chartype, NameStr(database->chartype)) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_TS_DATA_NODE_INVALID_CONFIG),
				 errmsg("database exists but has wrong LC_CTYPE"),
				 errdetail("Expected LC_CTYPE \"%s\" but it was \"%s\"",
						   NameStr(database->chartype),
						   actual_chartype)));
	return true;
}

static void
data_node_validate_extension(TSConnection *conn)
{
	const char *const dbname = PQdb(remote_connection_get_pg_conn(conn));
	const char *const host = PQhost(remote_connection_get_pg_conn(conn));
	const char *const port = PQport(remote_connection_get_pg_conn(conn));

	if (!remote_connection_check_extension(conn))
		ereport(ERROR,
				(errcode(ERRCODE_TS_DATA_NODE_INVALID_CONFIG),
				 errmsg("database does not have TimescaleDB extension loaded"),
				 errdetail("The TimescaleDB extension is not loaded in database %s on node at "
						   "%s:%s.",
						   dbname,
						   host,
						   port)));
}

static void
data_node_validate_as_data_node(TSConnection *conn)
{
	PGresult *res =
		remote_connection_exec(conn, "SELECT _timescaledb_internal.validate_as_data_node()");

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		ereport(ERROR,
				(errcode(ERRCODE_TS_DATA_NODE_INVALID_CONFIG),
				 (errmsg("%s is not valid as data node", remote_connection_node_name(conn)),
				  errdetail("%s", PQresultErrorMessage(res)))));

	remote_result_close(res);
}

/*
 * Bootstrap the extension and associated objects.
 */
static bool
data_node_bootstrap_extension(TSConnection *conn)
{
	const char *const username = PQuser(remote_connection_get_pg_conn(conn));
	const char *schema_name = ts_extension_schema_name();
	const char *schema_name_quoted = quote_identifier(schema_name);
	Oid schema_oid = get_namespace_oid(schema_name, true);

	/* We only count the number of tuples in the code below, but having the
	 * name and version are useful for debugging purposes. */
	PGresult *res =
		remote_connection_execf(conn,
								"SELECT extname, extversion FROM pg_extension WHERE extname = %s",
								quote_literal_cstr(EXTENSION_NAME));

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_EXCEPTION), errmsg("%s", PQresultErrorMessage(res))));

	if (PQntuples(res) == 0)
	{
		if (schema_oid != PG_PUBLIC_NAMESPACE)
		{
			PGresult *res = remote_connection_execf(conn,
													"CREATE SCHEMA %s AUTHORIZATION %s",
													schema_name_quoted,
													quote_identifier(username));
			if (PQresultStatus(res) != PGRES_COMMAND_OK)
			{
				const char *const sqlstate = PQresultErrorField(res, PG_DIAG_SQLSTATE);
				bool schema_exists =
					(sqlstate && strcmp(sqlstate, ERRCODE_DUPLICATE_SCHEMA_STR) == 0);
				if (!schema_exists)
					remote_result_elog(res, ERROR);
				/* If the schema already existed on the remote node, we got a
				 * duplicate schema error and the schema was not created. In
				 * that case, we log an error with a hint on how to fix the
				 * issue. */
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_SCHEMA),
						 errmsg("schema \"%s\" already exists in database, aborting", schema_name),
						 errhint("Please make sure that the data node does not contain any "
								 "existing objects prior to adding it.")));
			}
		}

		remote_connection_cmdf_ok(conn,
								  "CREATE EXTENSION " EXTENSION_NAME " WITH SCHEMA %s CASCADE",
								  schema_name_quoted);
		return true;
	}
	else
	{
		ereport(NOTICE,
				(errmsg("extension \"%s\" already exists on data node, skipping",
						PQgetvalue(res, 0, 0)),
				 errdetail("TimescaleDB extension version on %s:%s was %s.",
						   PQhost(remote_connection_get_pg_conn(conn)),
						   PQport(remote_connection_get_pg_conn(conn)),
						   PQgetvalue(res, 0, 1))));
		data_node_validate_extension(conn);
		return false;
	}
}

/* Add dist_uuid on the remote node.
 *
 * If the remote node is set to use the current database, `set_dist_id` will report an error and not
 * set it. */
static void
add_distributed_id_to_data_node(TSConnection *conn)
{
	Datum id_string = DirectFunctionCall1(uuid_out, dist_util_get_id());
	PGresult *res = remote_connection_queryf_ok(conn,
												"SELECT _timescaledb_internal.set_dist_id('%s')",
												DatumGetCString(id_string));
	remote_result_close(res);
}

/*
 * Connect to do bootstrapping.
 *
 * This behaves similar to connectMaintenanceDatabase and will first try to
 * connect to "postgres" database and if that does not exists, to the
 * "template1" database.
 */
static TSConnection *
connect_for_bootstrapping(const char *node_name, const char *const host, int32 port,
						  const char *username)
{
	List *node_options = create_data_node_options(host, port, "postgres", username);
	TSConnection *conn = remote_connection_open_with_options_nothrow(node_name, node_options);

	if (conn)
		return conn;

	node_options = create_data_node_options(host, port, "template1", username);
	return remote_connection_open_with_options_nothrow(node_name, node_options);
}

/**
 * Validate that extension is available and with the correct version.
 *
 * If the extension is not available on the data node, we will get strange
 * errors when we try to use functions, so we check that the extension is
 * available before attempting anything else.
 *
 * Will abort with error if there is an issue, otherwise do nothing.
 */
static void
data_node_validate_extension_availability(TSConnection *conn)
{
	PGresult *res = remote_connection_execf(conn,
											"SELECT default_version, installed_version FROM "
											"pg_available_extensions WHERE name = %s",
											quote_literal_cstr(EXTENSION_NAME));

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		ereport(ERROR,
				(errcode(ERRCODE_CONNECTION_EXCEPTION), errmsg("%s", PQresultErrorMessage(res))));

	if (PQntuples(res) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_TS_DATA_NODE_INVALID_CONFIG),
				 errmsg("TimescaleDB extension not available on remote PostgreSQL instance"),
				 errhint("Install the TimescaleDB extension on the remote PostgresSQL instance.")));

	/* Here we validate the available version, not the installed version */
	remote_validate_extension_version(conn, PQgetvalue(res, 0, 0));
}

/**
 * Get the configured server port for the server as an integer.
 *
 * Returns:
 *   Port number if a port is configured, -1 if it is not able to get
 *   the port number.
 *
 * Note:
 *   We cannot use `inet_server_port()` since that will return NULL if
 *   connecting to a server on localhost since a UNIX socket will be
 *   used. This is the case even if explicitly using a port when
 *   connecting. Regardless of how the user connected, we want to use the same
 *   port as the one that the server listens on.
 */
static int32
get_server_port()
{
	const char *const portstr =
		GetConfigOption("port", /* missing_ok= */ false, /* restrict_privileged= */ false);
	return pg_atoi(portstr, sizeof(int32), 0);
}

/* set_distid may need to be false for some otherwise invalid configurations
 * that are useful for testing */
static Datum
data_node_add_internal(PG_FUNCTION_ARGS, bool set_distid)
{
	Oid userid = GetUserId();
	const char *username = GetUserNameFromId(userid, false);
	const char *node_name = PG_ARGISNULL(0) ? NULL : PG_GETARG_CSTRING(0);
	const char *host = PG_ARGISNULL(1) ? NULL : TextDatumGetCString(PG_GETARG_DATUM(1));
	const char *dbname = PG_ARGISNULL(2) ? get_database_name(MyDatabaseId) : PG_GETARG_CSTRING(2);
	int32 port = PG_ARGISNULL(3) ? get_server_port() : PG_GETARG_INT32(3);
	bool if_not_exists = PG_ARGISNULL(4) ? false : PG_GETARG_BOOL(4);
	bool bootstrap = PG_ARGISNULL(5) ? true : PG_GETARG_BOOL(5);
	bool server_created = false;
	bool database_created = false;
	bool extension_created = false;
	bool PG_USED_FOR_ASSERTS_ONLY result;
	DbInfo database;

	PreventCommandIfReadOnly("add_data_node()");

	namestrcpy(&database.name, dbname);

	if (host == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 (errmsg("a host needs to be specified"),
				  errhint("Provide a host name or IP address of a data node to add."))));

	if (set_distid && dist_util_membership() == DIST_MEMBER_DATA_NODE)
		ereport(ERROR,
				(errcode(ERRCODE_TS_DATA_NODE_ASSIGNMENT_ALREADY_EXISTS),
				 (errmsg("unable to assign data nodes from an existing distributed database"))));

	if (NULL == node_name)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE), (errmsg("invalid data node name"))));

	if (port < 1 || port > PG_UINT16_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 (errmsg("invalid port number %d", port),
				  errhint("The port number must be between 1 and %u", PG_UINT16_MAX))));

	result = get_database_info(MyDatabaseId, &database);
	Assert(result);

	/*
	 * Since this function creates databases on remote nodes, and CREATE DATABASE
	 * cannot run in a transaction block, we cannot run the function in a
	 * transaction block either.
	 */
	PreventInTransactionBlock(true, "add_data_node");

	/* Try to create the foreign server, or get the existing one in case of
	 * if_not_exists = true. */
	if (create_foreign_server(node_name, host, port, dbname, if_not_exists, NULL))
	{
		List *node_options;
		TSConnection *conn;

		server_created = true;

		/* Make the foreign server visible in current transaction. */
		CommandCounterIncrement();

		/* If bootstrapping, we check the extension availability here and
		 * abort if the extension is not available. We should not start
		 * creating databases and other cruft on the datanode unless we know
		 * that the extension is installed.
		 *
		 * We ensure that there is a database if we are bootstrapping. This is
		 * done using a separate connection since the database that is going
		 * to be used for the data node does not exist yet, so we cannot
		 * connect to it. */
		if (bootstrap)
		{
			TSConnection *conn = connect_for_bootstrapping(node_name, host, port, username);
			data_node_validate_extension_availability(conn);
			database_created = data_node_bootstrap_database(conn, &database);
			remote_connection_close(conn);
		}

		/* Connect to the database we are bootstrapping and either install the
		 * extension or validate that the extension is installed. The
		 * following statements are executed inside a transaction so that they
		 * can be rolled back in the event of a failure.
		 *
		 * We could use `remote_dist_txn_get_connection` here, but it is
		 * comparably heavy and make the code more complicated than
		 * necessary. Instead using a more straightforward approach here since
		 * we do not need 2PC support. */
		node_options = create_data_node_options(host, port, dbname, username);
		conn = remote_connection_open_with_options(node_name, node_options, false);
		remote_connection_cmd_ok(conn, "BEGIN");

		if (bootstrap)
			extension_created = data_node_bootstrap_extension(conn);
		else
		{
			data_node_validate_database(conn, &database);
			data_node_validate_extension(conn);
			data_node_validate_as_data_node(conn);
		}

		/* After the node is verified or bootstrapped, we set the `dist_uuid`
		 * using the same connection. We skip this if clustering checks are
		 * disabled, which means that the `dist_uuid` is neither set nor
		 * checked.
		 *
		 * This is done inside a transaction so that we can roll it back if
		 * there are any failures. Note that any failure at this point will
		 * not rollback the creates above. */
		if (set_distid)
		{
			if (dist_util_membership() != DIST_MEMBER_ACCESS_NODE)
				dist_util_set_as_frontend();
			add_distributed_id_to_data_node(conn);
		}

		/* If there were an error before, we will not reach this point to the
		 * transaction will be aborted when the connection is closed. */
		remote_connection_cmd_ok(conn, "COMMIT");
		remote_connection_close(conn);
	}

	PG_RETURN_DATUM(create_data_node_datum(fcinfo,
										   node_name,
										   host,
										   port,
										   dbname,
										   server_created,
										   database_created,
										   extension_created));
}

Datum
data_node_add(PG_FUNCTION_ARGS)
{
	return data_node_add_internal(fcinfo, true);
}

Datum
data_node_add_without_dist_id(PG_FUNCTION_ARGS)
{
	return data_node_add_internal(fcinfo, false);
}

Datum
data_node_attach(PG_FUNCTION_ARGS)
{
	const char *node_name = PG_ARGISNULL(0) ? NULL : PG_GETARG_CSTRING(0);
	Oid table_id = PG_GETARG_OID(1);
	bool if_not_attached = PG_ARGISNULL(2) ? false : PG_GETARG_BOOL(2);
	bool repartition = PG_ARGISNULL(3) ? false : PG_GETARG_BOOL(3);
	ForeignServer *fserver;
	HypertableDataNode *node;
	Cache *hcache;
	Hypertable *ht;
	Dimension *dim;
	List *result;
	int num_nodes;
	ListCell *lc;

	PreventCommandIfReadOnly("attach_data_node()");

	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid hypertable: cannot be NULL")));
	Assert(get_rel_name(table_id));

	ht = ts_hypertable_cache_get_cache_and_entry(table_id, CACHE_FLAG_NONE, &hcache);
	Assert(ht != NULL);

	if (!hypertable_is_distributed(ht))
		ereport(ERROR,
				(errcode(ERRCODE_TS_HYPERTABLE_NOT_DISTRIBUTED),
				 errmsg("hypertable \"%s\" is not distributed", get_rel_name(table_id))));

	/* Must have owner permissions on the hypertable to attach a new data
	   node. Must also have USAGE on the foreign server.  */
	ts_hypertable_permissions_check(table_id, GetUserId());
	fserver = data_node_get_foreign_server(node_name, ACL_USAGE, true, false);

	Assert(NULL != fserver);

	foreach (lc, ht->data_nodes)
	{
		node = lfirst(lc);

		if (node->foreign_server_oid == fserver->serverid)
		{
			ts_cache_release(hcache);

			if (if_not_attached)
			{
				ereport(NOTICE,
						(errcode(ERRCODE_TS_DATA_NODE_ALREADY_ATTACHED),
						 errmsg("data node \"%s\" is already attached to hypertable \"%s\", "
								"skipping",
								node_name,
								get_rel_name(table_id))));
				PG_RETURN_DATUM(create_hypertable_data_node_datum(fcinfo, node));
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_TS_DATA_NODE_ALREADY_ATTACHED),
						 errmsg("data node \"%s\" is already attached to hypertable \"%s\"",
								node_name,
								get_rel_name(table_id))));
		}
	}

	result = hypertable_assign_data_nodes(ht->fd.id, list_make1((char *) node_name));
	Assert(result->length == 1);

	/* Get the first closed (space) dimension, which is the one along which we
	 * partition across data nodes. */
	dim = hyperspace_get_closed_dimension(ht->space, 0);

	num_nodes = list_length(ht->data_nodes) + 1;

	if (num_nodes > MAX_NUM_HYPERTABLE_DATA_NODES)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("max number of data nodes already attached"),
				 errdetail("The number of data nodes in a hypertable cannot exceed %d",
						   MAX_NUM_HYPERTABLE_DATA_NODES)));

	/* If there are less slices (partitions) in the space dimension than there
	 * are data nodes, we'd like to expand the number of slices to be able to
	 * make use of the new data node. */
	if (NULL != dim && num_nodes > dim->fd.num_slices)
	{
		if (repartition)
		{
			ts_dimension_set_number_of_slices(dim, num_nodes & 0xFFFF);

			ereport(NOTICE,
					(errmsg("the number of partitions in dimension \"%s\" was increased to %u",
							NameStr(dim->fd.column_name),
							num_nodes),
					 errdetail("To make use of all attached data nodes, a distributed "
							   "hypertable needs at least as many partitions in the first "
							   "closed (space) dimension as there are attached data nodes.")));
		}
		else
		{
			/* Raise a warning if the number of partitions are too few to make
			 * use of all data nodes. Need to refresh cache first to get the
			 * updated data node list. */
			int dimension_id = dim->fd.id;

			ts_cache_release(hcache);
			hcache = ts_hypertable_cache_pin();
			ht = ts_hypertable_cache_get_entry(hcache, table_id, CACHE_FLAG_NONE);
			ts_hypertable_check_partitioning(ht, dimension_id);
		}
	}

	node = linitial(result);
	ts_cache_release(hcache);

	PG_RETURN_DATUM(create_hypertable_data_node_datum(fcinfo, node));
}

/* Only used for generating proper error message */
typedef enum OperationType
{
	OP_BLOCK,
	OP_DETACH,
	OP_DELETE
} OperationType;

static char *
get_operation_type_message(OperationType op_type)
{
	switch (op_type)
	{
		case OP_BLOCK:
			return "blocking new chunks on";
		case OP_DETACH:
			return "detaching";
		case OP_DELETE:
			return "deleting";
		default:
			return NULL;
	}
}

static void
check_replication_for_new_data(const char *node_name, Hypertable *ht, bool force,
							   OperationType op_type)
{
	List *available_nodes = ts_hypertable_get_available_data_nodes(ht, false);
	char *operation = get_operation_type_message(op_type);

	if (ht->fd.replication_factor < list_length(available_nodes))
		return;

	if (!force)
		ereport(ERROR,
				(errcode(ERRCODE_TS_INTERNAL_ERROR),
				 errmsg("%s data node \"%s\" risks making new data for hypertable \"%s\" "
						"under-replicated",
						operation,
						node_name,
						NameStr(ht->fd.table_name)),
				 errhint("Call function with force => true to force this operation.")));

	ereport(WARNING,
			(errcode(ERRCODE_TS_INTERNAL_ERROR),
			 errmsg("new data for hypertable \"%s\" will be under-replicated due to %s data "
					"node "
					"\"%s\"",
					NameStr(ht->fd.table_name),
					operation,
					node_name)));
}

static bool
data_node_contains_non_replicated_chunks(List *chunk_data_nodes)
{
	ListCell *lc;

	foreach (lc, chunk_data_nodes)
	{
		ChunkDataNode *cdn = lfirst(lc);
		List *replicas =
			ts_chunk_data_node_scan_by_chunk_id(cdn->fd.chunk_id, CurrentMemoryContext);

		if (list_length(replicas) < 2)
			return true;
	}

	return false;
}

static List *
data_node_detach_validate(const char *node_name, Hypertable *ht, bool force, OperationType op_type)
{
	List *chunk_data_nodes =
		ts_chunk_data_node_scan_by_node_name_and_hypertable_id(node_name,
															   ht->fd.id,
															   CurrentMemoryContext);
	bool has_non_replicated_chunks = data_node_contains_non_replicated_chunks(chunk_data_nodes);
	char *operation = get_operation_type_message(op_type);

	if (has_non_replicated_chunks)
		ereport(ERROR,
				(errcode(ERRCODE_TS_INTERNAL_ERROR),
				 errmsg("%s data node \"%s\" would mean a data-loss for hypertable "
						"\"%s\" since data node has the only data replica",
						operation,
						node_name,
						NameStr(ht->fd.table_name)),
				 errhint("Ensure the data node \"%s\" has no non-replicated data before %s it.",
						 node_name,
						 operation)));

	if (list_length(chunk_data_nodes) > 0)
	{
		if (force)
			ereport(WARNING,
					(errcode(ERRCODE_WARNING),
					 errmsg("hypertable \"%s\" has under-replicated chunks due to %s "
							"data node \"%s\"",
							NameStr(ht->fd.table_name),
							operation,
							node_name)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_TS_DATA_NODE_IN_USE),
					 errmsg("%s data node \"%s\" failed because it contains chunks "
							"for hypertable \"%s\"",
							operation,
							node_name,
							NameStr(ht->fd.table_name))));
	}

	check_replication_for_new_data(node_name, ht, force, op_type);

	return chunk_data_nodes;
}

static int
data_node_modify_hypertable_data_nodes(const char *node_name, List *hypertable_data_nodes,
									   bool all_hypertables, OperationType op_type,
									   bool block_chunks, bool force, bool repartition)
{
	Cache *hcache = ts_hypertable_cache_pin();
	ListCell *lc;
	int removed = 0;

	foreach (lc, hypertable_data_nodes)
	{
		HypertableDataNode *node = lfirst(lc);
		Oid relid = ts_hypertable_id_to_relid(node->fd.hypertable_id);
		Hypertable *ht = ts_hypertable_cache_get_entry_by_id(hcache, node->fd.hypertable_id);
		bool has_privs = ts_hypertable_has_privs_of(relid, GetUserId());

		Assert(ht != NULL);

		if (!has_privs)
		{
			/* If the operation is OP_DELETE, we MUST be able to detach the data
			 * node from ALL tables since the foreign server object will be
			 * deleted. Therefore, we fail the operation if we find a table
			 * that we don't have owner permissions on in this case. */
			if (all_hypertables && op_type != OP_DELETE)
				ereport(NOTICE,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("skipping hypertable \"%s\" due to missing permissions",
								get_rel_name(relid))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied for hypertable \"%s\"", get_rel_name(relid)),
						 errdetail("The data node is attached to hypertables that the current "
								   "user lacks permissions for.")));
		}
		else if (op_type == OP_DETACH || op_type == OP_DELETE)
		{
			/* we have permissions to detach */
			List *chunk_data_nodes =
				data_node_detach_validate(NameStr(node->fd.node_name), ht, force, op_type);
			ListCell *cs_lc;

			/* update chunk foreign table server and delete chunk mapping */
			foreach (cs_lc, chunk_data_nodes)
			{
				ChunkDataNode *cdn = lfirst(cs_lc);

				chunk_update_foreign_server_if_needed(cdn->fd.chunk_id, cdn->foreign_server_oid);
				ts_chunk_data_node_delete_by_chunk_id_and_node_name(cdn->fd.chunk_id,
																	NameStr(cdn->fd.node_name));
			}

			/* delete hypertable mapping */
			removed +=
				ts_hypertable_data_node_delete_by_node_name_and_hypertable_id(node_name, ht->fd.id);

			if (repartition)
			{
				Dimension *dim = hyperspace_get_closed_dimension(ht->space, 0);
				int num_nodes = list_length(ht->data_nodes) - 1;

				if (dim != NULL && num_nodes < dim->fd.num_slices && num_nodes > 0)
				{
					ts_dimension_set_number_of_slices(dim, num_nodes & 0xFFFF);

					ereport(NOTICE,
							(errmsg("the number of partitions in dimension \"%s\" was decreased to "
									"%u",
									NameStr(dim->fd.column_name),
									num_nodes),
							 errdetail(
								 "To make efficient use of all attached data nodes, the number of "
								 "space partitions was set to match the number of data nodes.")));
				}
			}
		}
		else
		{
			/*  set block new chunks */
			if (block_chunks)
			{
				if (node->fd.block_chunks)
				{
					ereport(NOTICE,
							(errcode(ERRCODE_TS_INTERNAL_ERROR),
							 errmsg("new chunks already blocked on data node \"%s\" for "
									"hypertable "
									"\"%s\"",
									NameStr(node->fd.node_name),
									get_rel_name(relid))));
					continue;
				}

				check_replication_for_new_data(node_name, ht, force, OP_BLOCK);
			}
			node->fd.block_chunks = block_chunks;
			removed += ts_hypertable_data_node_update(node);
		}
	}
	ts_cache_release(hcache);
	return removed;
}

static int
data_node_block_hypertable_data_nodes(const char *node_name, List *hypertable_data_nodes,
									  bool all_hypertables, bool block_chunks, bool force)
{
	return data_node_modify_hypertable_data_nodes(node_name,
												  hypertable_data_nodes,
												  all_hypertables,
												  OP_BLOCK,
												  block_chunks,
												  force,
												  false);
}

static int
data_node_detach_hypertable_data_nodes(const char *node_name, List *hypertable_data_nodes,
									   bool all_hypertables, bool force, bool repartition,
									   OperationType op_type)
{
	return data_node_modify_hypertable_data_nodes(node_name,
												  hypertable_data_nodes,
												  all_hypertables,
												  op_type,
												  false,
												  force,
												  repartition);
}

static HypertableDataNode *
get_hypertable_data_node(Oid table_id, const char *node_name, bool owner_check, bool attach_check)
{
	HypertableDataNode *hdn = NULL;
	Cache *hcache = ts_hypertable_cache_pin();
	Hypertable *ht = ts_hypertable_cache_get_entry(hcache, table_id, CACHE_FLAG_NONE);
	ListCell *lc;

	if (owner_check)
		ts_hypertable_permissions_check(table_id, GetUserId());

	foreach (lc, ht->data_nodes)
	{
		hdn = lfirst(lc);
		if (namestrcmp(&hdn->fd.node_name, node_name) == 0)
			break;
		else
			hdn = NULL;
	}

	if (hdn == NULL)
	{
		if (attach_check)
			ereport(ERROR,
					(errcode(ERRCODE_TS_DATA_NODE_NOT_ATTACHED),
					 errmsg("data node \"%s\" is not attached to hypertable \"%s\"",
							node_name,
							get_rel_name(table_id))));
		else
			ereport(NOTICE,
					(errcode(ERRCODE_TS_DATA_NODE_NOT_ATTACHED),
					 errmsg("data node \"%s\" is not attached to hypertable \"%s\", "
							"skipping",
							node_name,
							get_rel_name(table_id))));
	}

	ts_cache_release(hcache);

	return hdn;
}

static Datum
data_node_block_or_allow_new_chunks(const char *node_name, Oid const table_id, bool force,
									bool block_chunks)
{
	int affected = 0;
	bool all_hypertables = table_id == InvalidOid ? true : false;
	List *hypertable_data_nodes = NIL;
	ForeignServer *server = data_node_get_foreign_server(node_name, ACL_USAGE, true, false);

	Assert(NULL != server);

	if (OidIsValid(table_id))
	{
		/* Early abort on missing hypertable permissions */
		ts_hypertable_permissions_check(table_id, GetUserId());
		hypertable_data_nodes =
			list_make1(get_hypertable_data_node(table_id, server->servername, true, true));
	}
	else
	{
		/* block or allow for all hypertables */
		hypertable_data_nodes =
			ts_hypertable_data_node_scan_by_node_name(server->servername, CurrentMemoryContext);
	}

	affected = data_node_block_hypertable_data_nodes(server->servername,
													 hypertable_data_nodes,
													 all_hypertables,
													 block_chunks,
													 force);
	return Int32GetDatum(affected);
}

Datum
data_node_allow_new_chunks(PG_FUNCTION_ARGS)
{
	const char *node_name = PG_ARGISNULL(0) ? NULL : NameStr(*PG_GETARG_NAME(0));
	Oid table_id = PG_ARGISNULL(1) ? InvalidOid : PG_GETARG_OID(1);

	PreventCommandIfReadOnly("allow_new_chunks()");

	return data_node_block_or_allow_new_chunks(node_name, table_id, false, false);
}

Datum
data_node_block_new_chunks(PG_FUNCTION_ARGS)
{
	const char *node_name = PG_ARGISNULL(0) ? NULL : NameStr(*PG_GETARG_NAME(0));
	Oid table_id = PG_ARGISNULL(1) ? InvalidOid : PG_GETARG_OID(1);
	bool force = PG_ARGISNULL(2) ? false : PG_GETARG_BOOL(2);

	PreventCommandIfReadOnly("block_new_chunks()");

	return data_node_block_or_allow_new_chunks(node_name, table_id, force, true);
}

Datum
data_node_detach(PG_FUNCTION_ARGS)
{
	const char *node_name = PG_ARGISNULL(0) ? NULL : NameStr(*PG_GETARG_NAME(0));
	Oid table_id = PG_ARGISNULL(1) ? InvalidOid : PG_GETARG_OID(1);
	bool all_hypertables = PG_ARGISNULL(1);
	bool if_attached = PG_ARGISNULL(2) ? false : PG_GETARG_BOOL(2);
	bool force = PG_ARGISNULL(3) ? InvalidOid : PG_GETARG_OID(3);
	bool repartition = PG_ARGISNULL(4) ? false : PG_GETARG_BOOL(4);
	int removed = 0;
	List *hypertable_data_nodes = NIL;
	ForeignServer *server;

	PreventCommandIfReadOnly("detach_data_node()");

	server = data_node_get_foreign_server(node_name, ACL_USAGE, true, false);
	Assert(NULL != server);

	if (OidIsValid(table_id))
	{
		HypertableDataNode *node;

		/* Early abort on missing hypertable permissions */
		ts_hypertable_permissions_check(table_id, GetUserId());

		node = get_hypertable_data_node(table_id, server->servername, true, !if_attached);
		if (node)
			hypertable_data_nodes = list_make1(node);
	}
	else
	{
		/* Detach data node for all hypertables where user has
		 * permissions. Permissions checks done in
		 * data_node_detach_hypertable_data_nodes().  */
		hypertable_data_nodes =
			ts_hypertable_data_node_scan_by_node_name(server->servername, CurrentMemoryContext);
	}

	removed = data_node_detach_hypertable_data_nodes(server->servername,
													 hypertable_data_nodes,
													 all_hypertables,
													 force,
													 repartition,
													 OP_DETACH);

	PG_RETURN_INT32(removed);
}

Datum
data_node_delete(PG_FUNCTION_ARGS)
{
	const char *node_name = PG_ARGISNULL(0) ? NULL : PG_GETARG_CSTRING(0);
	bool if_exists = PG_ARGISNULL(1) ? false : PG_GETARG_BOOL(1);
	bool force = PG_ARGISNULL(2) ? false : PG_GETARG_BOOL(2);
	bool repartition = PG_ARGISNULL(3) ? false : PG_GETARG_BOOL(3);
	List *hypertable_data_nodes = NIL;
	DropStmt stmt;
	ObjectAddress address;
	ObjectAddress secondary_object = {
		.classId = InvalidOid,
		.objectId = InvalidOid,
		.objectSubId = 0,
	};
	Node *parsetree = NULL;
	TSConnectionId cid;
	ForeignServer *server;

	PreventCommandIfReadOnly("delete_data_node()");

	/* Need USAGE to detach. Further owner check done when executing the DROP
	 * statement. */
	server = data_node_get_foreign_server(node_name, ACL_USAGE, true, if_exists);

	Assert(server == NULL ? if_exists : true);

	if (NULL == server)
	{
		elog(NOTICE, "data node \"%s\" does not exist, skipping", node_name);
		PG_RETURN_BOOL(false);
	}

	/* close any pending connections */
	remote_connection_id_set(&cid, server->serverid, GetUserId());
	remote_connection_cache_remove(cid);

	/* detach data node */
	hypertable_data_nodes =
		ts_hypertable_data_node_scan_by_node_name(node_name, CurrentMemoryContext);

	data_node_detach_hypertable_data_nodes(node_name,
										   hypertable_data_nodes,
										   true,
										   force,
										   repartition,
										   OP_DELETE);

	/* clean up persistent transaction records */
	remote_txn_persistent_record_delete_for_data_node(server->serverid);

	stmt = (DropStmt){
		.type = T_DropStmt,
		.objects = list_make1(makeString(pstrdup(node_name))),
		.removeType = OBJECT_FOREIGN_SERVER,
		.behavior = DROP_RESTRICT,
		.missing_ok = if_exists,
	};

	parsetree = (Node *) &stmt;

	/* Make sure event triggers are invoked so that all dropped objects
	 * are collected during a cascading drop. This ensures all dependent
	 * objects get cleaned up. */
	EventTriggerBeginCompleteQuery();

	PG_TRY();
	{
		ObjectAddressSet(address, ForeignServerRelationId, server->serverid);
		EventTriggerDDLCommandStart(parsetree);
		RemoveObjects(&stmt);
		EventTriggerCollectSimpleCommand(address, secondary_object, parsetree);
		EventTriggerSQLDrop(parsetree);
		EventTriggerDDLCommandEnd(parsetree);
	}
	PG_CATCH();
	{
		EventTriggerEndCompleteQuery();
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* Remove self from dist db if no longer have data_nodes */
	if (data_node_get_node_name_list() == NIL)
		dist_util_remove_from_db();

	EventTriggerEndCompleteQuery();
	CommandCounterIncrement();
	CacheInvalidateRelcacheByRelid(ForeignServerRelationId);

	PG_RETURN_BOOL(true);
}

/*
 * Get server list, performing an ACL check on each of them in the process.
 */
List *
data_node_get_node_name_list_with_aclcheck(AclMode mode, bool fail_on_aclcheck)
{
	HeapTuple tuple;
	ScanKeyData scankey[1];
	SysScanDesc scandesc;
	Relation rel;
	ForeignDataWrapper *fdw = GetForeignDataWrapperByName(EXTENSION_FDW_NAME, false);
	List *nodes = NIL;

	rel = table_open(ForeignServerRelationId, AccessShareLock);

	ScanKeyInit(&scankey[0],
				Anum_pg_foreign_server_srvfdw,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(fdw->fdwid));

	scandesc = systable_beginscan(rel, InvalidOid, false, NULL, 1, scankey);

	while (HeapTupleIsValid(tuple = systable_getnext(scandesc)))
	{
		Form_pg_foreign_server form = (Form_pg_foreign_server) GETSTRUCT(tuple);
		ForeignServer *server;

		server =
			data_node_get_foreign_server(NameStr(form->srvname), mode, fail_on_aclcheck, false);

		if (server != NULL)
			nodes = lappend(nodes, pstrdup(NameStr(form->srvname)));
	}

	systable_endscan(scandesc);
	table_close(rel, AccessShareLock);

	return nodes;
}

/*
 * Get server list with optional ACL check.
 *
 * Returns:
 *
 * If nodearr is NULL, returns all system-configured data nodes that fulfill
 * the ACL check.
 *
 * If nodearr is non-NULL, returns all the data nodes in the specified array
 * subject to ACL checks.
 */
List *
data_node_get_filtered_node_name_list(ArrayType *nodearr, AclMode mode, bool fail_on_aclcheck)
{
	ArrayIterator it;
	Datum node_datum;
	bool isnull;
	List *nodes = NIL;

	if (NULL == nodearr)
		return data_node_get_node_name_list_with_aclcheck(mode, fail_on_aclcheck);

	it = array_create_iterator(nodearr, 0, NULL);

	while (array_iterate(it, &node_datum, &isnull))
	{
		if (!isnull)
		{
			const char *node_name = DatumGetCString(node_datum);
			ForeignServer *server =
				data_node_get_foreign_server(node_name, mode, fail_on_aclcheck, false);

			if (NULL != server)
				nodes = lappend(nodes, server->servername);
		}
	}

	array_free_iterator(it);

	return nodes;
}

List *
data_node_get_node_name_list(void)
{
	return data_node_get_node_name_list_with_aclcheck(ACL_NO_CHECK, false);
}

/*
 * Turn an array of data nodes into a list of names.
 *
 * The function will verify that all the servers in the list belong to the
 * TimescaleDB foreign data wrapper. Optionally, perform ACL check on each
 * data node's foreign server. Checks are skipped when specificing
 * ACL_NO_CHECK. If fail_on_aclcheck is false, then no errors will be thrown
 * on ACL check failures. Instead, data nodes that fail ACL checks will simply
 * be filtered.
 */
List *
data_node_array_to_node_name_list_with_aclcheck(ArrayType *nodearr, AclMode mode,
												bool fail_on_aclcheck)
{
	if (NULL == nodearr)
		return NIL;

	Assert(ARR_NDIM(nodearr) <= 1);

	return data_node_get_filtered_node_name_list(nodearr, mode, fail_on_aclcheck);
}

List *
data_node_array_to_node_name_list(ArrayType *nodearr)
{
	return data_node_array_to_node_name_list_with_aclcheck(nodearr, ACL_NO_CHECK, false);
}

Datum
data_node_ping(PG_FUNCTION_ARGS)
{
	const char *node_name = PG_ARGISNULL(0) ? NULL : PG_GETARG_CSTRING(0);
	/* Allow anyone to ping a data node. Otherwise the
	 * timescaledb_information.data_node view won't work for those users. */
	ForeignServer *server = data_node_get_foreign_server(node_name, ACL_NO_CHECK, false, false);
	bool success;

	Assert(NULL != server);

	success = remote_connection_ping(server->servername);

	PG_RETURN_DATUM(BoolGetDatum(success));
}

List *
data_node_oids_to_node_name_list(List *data_node_oids, AclMode mode)
{
	List *node_names = NIL;
	ListCell *lc;
	ForeignServer *fs;

	foreach (lc, data_node_oids)
	{
		Oid foreign_server_oid = lfirst_oid(lc);
		fs = data_node_get_foreign_server_by_oid(foreign_server_oid, mode);
		node_names = lappend(node_names, pstrdup(fs->servername));
	}

	return node_names;
}

void
data_node_name_list_check_acl(List *data_node_names, AclMode mode)
{
	AclResult aclresult;
	Oid curuserid;
	ListCell *lc;

	if (data_node_names == NIL)
		return;

	curuserid = GetUserId();

	foreach (lc, data_node_names)
	{
		ForeignServer *server = GetForeignServerByName(lfirst(lc), false);

		/* Must have permissions on the server object */
		aclresult = pg_foreign_server_aclcheck(server->serverid, curuserid, mode);

		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_FOREIGN_SERVER, server->servername);
	}
}
