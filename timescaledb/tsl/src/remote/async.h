/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */
#ifndef TIMESCALEDB_TSL_REMOTE_ASYNC_H
#define TIMESCALEDB_TSL_REMOTE_ASYNC_H

#include <postgres.h>
#include <libpq-fe.h>
#include <utils/timestamp.h>

#include "stmt_params.h"

#define DEFAULT_TIMEOUT_MS (SECS_PER_HOUR * 1000)

typedef struct AsyncRequest AsyncRequest;

typedef struct TSConnection TSConnection;

typedef enum AsyncResponseType
{
	/* We got an entire result */
	RESPONSE_RESULT = 0,
	/* We got one row */
	RESPONSE_ROW,
	/* There was some kind of communication error */
	RESPONSE_COMMUNICATION_ERROR,
	/* Timeout while waiting for response */
	RESPONSE_TIMEOUT,
	/* Unexpected event or other error */
	RESPONSE_ERROR,
} AsyncResponseType;

/* Base type for all responses */
typedef struct AsyncResponse AsyncResponse;
typedef struct AsyncResponseResult AsyncResponseResult;
typedef struct AsyncResponseCommunicationError AsyncResponseCommunicationError;
typedef struct AsyncResponseError AsyncResponseError;
typedef struct AsyncRequestSet AsyncRequestSet;

typedef struct PreparedStmt PreparedStmt;

/* Notes on naming conventions used in functions:
 *
 * Parameters named sql_statement take a string with single sql statement (as opposed to multiple
 * statements)
 *
 * function endings:
 * * _any_response return any responses including errors and timeouts
 * * _any_result return a result which may be an error or success, but throw errors for
 * communication errors and timeouts
 * * _ok_result return successful results, throwing errors otherwise.
 * * _ok_command returns void on successful commands, throwing errors otherwise.
 *
 */

/* Async Request */

typedef void (*async_response_callback)(AsyncRequest *m, AsyncResponse *, void *data);

#define TS_NO_TIMEOUT DT_NOBEGIN

extern AsyncRequest *async_request_send_with_stmt_params_elevel_res_format(
	TSConnection *conn, const char *sql_statement, StmtParams *params, int elevel, int res_format);

#define async_request_send_with_params(conn, sql_statement, params, res_format)                    \
	async_request_send_with_stmt_params_elevel_res_format(conn,                                    \
														  sql_statement,                           \
														  params,                                  \
														  ERROR,                                   \
														  res_format)
#define async_request_send_with_params_elevel(conn, sql_statement, params, elevel)                 \
	async_request_send_with_stmt_params_elevel_res_format(conn,                                    \
														  sql_statement,                           \
														  params,                                  \
														  elevel,                                  \
														  FORMAT_TEXT)
#define async_request_send_binary(conn, sql_statement)                                             \
	async_request_send_with_stmt_params_elevel_res_format(conn,                                    \
														  sql_statement,                           \
														  NULL,                                    \
														  ERROR,                                   \
														  FORMAT_BINARY)
#define async_request_send_with_error(conn, sql_statement, elevel)                                 \
	async_request_send_with_stmt_params_elevel_res_format(conn,                                    \
														  sql_statement,                           \
														  NULL,                                    \
														  elevel,                                  \
														  FORMAT_TEXT)
#define async_request_send(conn, sql_statement)                                                    \
	async_request_send_with_error(conn, sql_statement, ERROR)

extern AsyncRequest *async_request_send_prepare(TSConnection *conn, const char *sql_statement,
												int n_params);
extern AsyncRequest *async_request_send_prepared_stmt(PreparedStmt *stmt,
													  const char *const *paramValues);
extern AsyncRequest *async_request_send_prepared_stmt_with_params(PreparedStmt *stmt,
																  StmtParams *params,
																  int res_format);

extern void async_request_attach_user_data(AsyncRequest *req, void *user_data);
extern void async_request_set_response_callback(AsyncRequest *req, async_response_callback cb,
												void *user_data);
extern bool async_request_set_single_row_mode(AsyncRequest *req);
extern TSConnection *async_request_get_connection(AsyncRequest *req);
extern AsyncResponseResult *async_request_wait_ok_result(AsyncRequest *request);
extern AsyncResponseResult *async_request_wait_any_result(AsyncRequest *request);

/* Returns on successful commands, throwing errors otherwise */
extern void async_request_wait_ok_command(AsyncRequest *set);
extern PreparedStmt *async_request_wait_prepared_statement(AsyncRequest *request);

/* Async Response */
extern void async_response_close(AsyncResponse *res);
extern void async_response_report_error(AsyncResponse *res, int elevel);
extern void async_response_report_error_or_close(AsyncResponse *res, int elevel);

extern AsyncResponseType async_response_get_type(AsyncResponse *res);
extern void async_response_result_close(AsyncResponseResult *res);
extern PGresult *async_response_result_get_pg_result(AsyncResponseResult *res);
extern void *async_response_result_get_user_data(AsyncResponseResult *res);
extern AsyncRequest *async_response_result_get_request(AsyncResponseResult *res);
extern PreparedStmt *async_response_result_generate_prepared_stmt(AsyncResponseResult *res);

/* Async Request Set */
extern AsyncRequestSet *async_request_set_create(void);
extern void async_request_set_add(AsyncRequestSet *set, AsyncRequest *req);
#define async_request_set_add_sql(set, conn, sql)                                                  \
	async_request_set_add(set, async_request_send(conn, sql))

/* Return any response, including communication errors and timeouts */
extern AsyncResponse *async_request_set_wait_any_response_deadline(AsyncRequestSet *set,
																   TimestampTz endtime);

#define async_request_set_wait_any_response_timeout(set, timeout_ms)                               \
	async_request_set_wait_any_response_deadline(                                                  \
		set, TimestampTzPlusMilliseconds(GetCurrentTimestamp(), timeout_ms))

#define async_request_set_wait_any_response(set)                                                   \
	async_request_set_wait_any_response_timeout(set, DEFAULT_TIMEOUT_MS)

/* Return only successful results, throwing errors otherwise */
extern AsyncResponseResult *async_request_set_wait_ok_result(AsyncRequestSet *set);

extern void async_request_set_wait_all_ok_commands(AsyncRequestSet *set);

/* Return any results (success or failure), but throw errors on communication failures and timeouts
 */
extern AsyncResponseResult *async_request_set_wait_any_result(AsyncRequestSet *set);

/* Consume any pending response and throw it away */
extern void async_request_discard_response(AsyncRequest *req);

/* Prepared Statements */
extern void prepared_stmt_close(PreparedStmt *stmt);

#endif /* TIMESCALEDB_TSL_REMOTE_ASYNC_H */
