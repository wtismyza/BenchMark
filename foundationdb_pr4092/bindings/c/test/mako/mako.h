#ifndef MAKO_H
#define MAKO_H
#pragma once

#ifndef FDB_API_VERSION
#define FDB_API_VERSION 630
#endif

#include <foundationdb/fdb_c.h>
#include <pthread.h>
#include <sys/types.h>
#if defined(__linux__)
#include <linux/limits.h>
#elif defined(__APPLE__)
#include <sys/syslimits.h>
#else
#include <limits.h>
#endif

#define VERBOSE_NONE 0
#define VERBOSE_DEFAULT 1
#define VERBOSE_ANNOYING 2
#define VERBOSE_DEBUG 3

#define MODE_INVALID -1
#define MODE_CLEAN 0
#define MODE_BUILD 1
#define MODE_RUN 2

#define FDB_SUCCESS 0
#define FDB_ERROR_RETRY -1
#define FDB_ERROR_ABORT -2
#define FDB_ERROR_CONFLICT -3

/* transaction specification */
enum Operations {
	OP_GETREADVERSION,
	OP_GET,
	OP_GETRANGE,
	OP_SGET,
	OP_SGETRANGE,
	OP_UPDATE,
	OP_INSERT,
	OP_INSERTRANGE,
	OP_CLEAR,
	OP_SETCLEAR,
	OP_CLEARRANGE,
	OP_SETCLEARRANGE,
	OP_COMMIT,
	MAX_OP /* must be the last item */
};

#define OP_COUNT 0
#define OP_RANGE 1
#define OP_REVERSE 2

/* for long arguments */
enum Arguments {
	ARG_KEYLEN,
	ARG_VALLEN,
	ARG_TPS,
	ARG_COMMITGET,
	ARG_SAMPLING,
	ARG_VERSION,
	ARG_KNOBS,
	ARG_FLATBUFFERS,
	ARG_TRACE,
	ARG_TRACEPATH,
	ARG_TRACEFORMAT,
	ARG_TPSMAX,
	ARG_TPSMIN,
	ARG_TPSINTERVAL,
	ARG_TPSCHANGE,
	ARG_TXNTRACE
};

enum TPSChangeTypes { TPS_SIN, TPS_SQUARE, TPS_PULSE };

#define KEYPREFIX "mako"
#define KEYPREFIXLEN 4

/* we set mako_txnspec_t and mako_args_t only once in the master process,
 * and won't be touched by child processes.
 */

typedef struct {
	/* for each operation, it stores "count", "range" and "reverse" */
	int ops[MAX_OP][3];
} mako_txnspec_t;

#define KNOB_MAX 256

/* benchmark parameters */
typedef struct {
	int api_version;
	int json;
	int num_processes;
	int num_threads;
	int mode;
	int rows; /* is 2 billion enough? */
	int seconds;
	int iteration;
	int tpsmax;
	int tpsmin;
	int tpsinterval;
	int tpschange;
	int sampling;
	int key_length;
	int value_length;
	int zipf;
	int commit_get;
	int verbose;
	mako_txnspec_t txnspec;
	char cluster_file[PATH_MAX];
	int trace;
	char tracepath[PATH_MAX];
	int traceformat; /* 0 - XML, 1 - JSON */
	char knobs[KNOB_MAX];
	uint8_t flatbuffers;
	int txntrace;
} mako_args_t;

/* shared memory */
#define SIGNAL_RED 0
#define SIGNAL_GREEN 1
#define SIGNAL_OFF 2

typedef struct {
	int signal;
	int readycount;
	double throttle_factor;
} mako_shmhdr_t;

typedef struct {
	uint64_t xacts;
	uint64_t conflicts;
	uint64_t ops[MAX_OP];
	uint64_t errors[MAX_OP];
	uint64_t latency_samples[MAX_OP];
	uint64_t latency_us_total[MAX_OP];
	uint64_t latency_us_min[MAX_OP];
	uint64_t latency_us_max[MAX_OP];
} mako_stats_t;

/* per-process information */
typedef struct {
	int worker_id;
	FDBDatabase* database;
	mako_args_t* args;
	mako_shmhdr_t* shm;
} process_info_t;

/* args for threads */
typedef struct {
	int thread_id;
	process_info_t* process;
} thread_args_t;

/* process type */
typedef enum { proc_master = 0, proc_worker, proc_stats } proc_type_t;

#endif /* MAKO_H */
