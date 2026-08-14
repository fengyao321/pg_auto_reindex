/*-------------------------------------------------------------------------
 *
 * pg_auto_reindex.c
 *		Module initialization: GUC registration and shared memory setup.
 *
 *		v2.0: NO background worker. The external daemon handles all
 *		scheduling, learning, and REINDEX CONCURRENTLY execution.
 *
 * Copyright (c) 2026, fengyao <fengyao0087@gmail.com>
 *
 * IDENTIFICATION
 *	  contrib/pg_auto_reindex/src/pg_auto_reindex.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "pg_auto_reindex.h"

PG_MODULE_MAGIC;

AutoReindexSharedState *AutoReindexShared = NULL;

int guc_lock_timeout_ms = 5000;
int guc_max_xact_duration = 300;

static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;

void _PG_init(void);
static void pg_auto_reindex_shmem_request(void);
static void pg_auto_reindex_shmem_startup(void);

/*
 * shmem_request_hook: Request shared memory and LWLock tranche.
 * PostgreSQL 15+ requires this to be done in the request hook,
 * not directly in _PG_init().
 */
static void
pg_auto_reindex_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(sizeof(AutoReindexSharedState));
	RequestNamedLWLockTranche("pg_auto_reindex_lock", 1);
}

/*
 * shmem_startup_hook: Initialize the shared memory struct.
 */
static void
pg_auto_reindex_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	AutoReindexShared = (AutoReindexSharedState *)
		ShmemInitStruct("pg_auto_reindex_shared",
						sizeof(AutoReindexSharedState),
						&found);

	if (!found)
	{
		AutoReindexShared->lock = &(GetNamedLWLockTranche("pg_auto_reindex_lock"))->lock;
		AutoReindexShared->current_reindexing_index = InvalidOid;
		AutoReindexShared->last_reindex_time = 0;
		AutoReindexShared->total_reindexed_count = 0;
		AutoReindexShared->total_bytes_saved = 0;
	}

	LWLockRelease(AddinShmemInitLock);
}

void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		return;

	/* Register GUC parameters */
	DefineCustomIntVariable("pg_auto_reindex.lock_timeout_ms",
							"Lock timeout for safety checks in milliseconds.",
							NULL,
							&guc_lock_timeout_ms,
							5000,
							100,
							300000,
							PGC_SIGHUP,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("pg_auto_reindex.max_xact_duration",
							"Maximum transaction duration in seconds before it is considered long running.",
							NULL,
							&guc_max_xact_duration,
							300,
							10,
							86400,
							PGC_SIGHUP,
							0,
							NULL,
							NULL,
							NULL);

	/* Hook into shmem_request for PG15+ compatibility */
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = pg_auto_reindex_shmem_request;

	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pg_auto_reindex_shmem_startup;
}
