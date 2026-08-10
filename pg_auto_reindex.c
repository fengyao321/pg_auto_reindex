/*-------------------------------------------------------------------------
 *
 * pg_auto_reindex.c
 *		Autonomous idle learning & background concurrent reindexing.
 *
 * Copyright (c) 2026, fengyao <fengyao0087@gmail.com>
 *
 * IDENTIFICATION
 *	  contrib/pg_auto_reindex/pg_auto_reindex.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include "access/xact.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/timestamp.h"

#include "pg_auto_reindex.h"

PG_MODULE_MAGIC;

void _PG_init(void);
void pg_auto_reindex_main(Datum main_arg);

/* GUC Definition Declarations */
bool   guc_enabled = true;
char  *guc_database = "postgres";
int    guc_naptime = 60;
double guc_idle_ratio_threshold = 0.70;
double guc_max_idle_load = 2.0;
int    guc_max_idle_backends = 15;
double guc_min_bloat_ratio = 0.30;
int64  guc_min_bloat_bytes = 67108864; /* 64MB */
int    guc_lock_timeout_ms = 5000;      /* 5s */
int    guc_max_reindexes_per_idle = 2;

static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
AutoReindexSharedState *AutoReindexShared = NULL;

static void
auto_reindex_shmem_startup(void)
{
    bool found;

    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();

    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
    AutoReindexShared = ShmemInitStruct("pg_auto_reindex_shmem",
                                        sizeof(AutoReindexSharedState),
                                        &found);
    if (!found)
    {
        memset(AutoReindexShared, 0, sizeof(AutoReindexSharedState));
        AutoReindexShared->lock = &(GetNamedLWLockTranche("pg_auto_reindex"))->lock;
    }
    LWLockRelease(AddinShmemInitLock);
}

void
_PG_init(void)
{
    BackgroundWorker worker;

    /* Register GUC parameters unconditionally so SHOW commands work */
    DefineCustomBoolVariable("pg_auto_reindex.enabled",
                             "Enable or disable automatic index reindexing",
                             NULL, &guc_enabled, true,
                             PGC_SIGHUP, 0, NULL, NULL, NULL);

    DefineCustomStringVariable("pg_auto_reindex.database",
                               "Target database for auto reindex background worker",
                               NULL, &guc_database, "postgres",
                               PGC_SIGHUP, 0, NULL, NULL, NULL);

    DefineCustomIntVariable("pg_auto_reindex.naptime",
                            "Sampling interval in seconds",
                            NULL, &guc_naptime, 60, 1, 3600,
                            PGC_SIGHUP, 0, NULL, NULL, NULL);

    DefineCustomRealVariable("pg_auto_reindex.idle_ratio_threshold",
                             "Threshold ratio of current load vs historical EWMA to consider idle",
                             NULL, &guc_idle_ratio_threshold, 0.70, 0.10, 1.00,
                             PGC_SIGHUP, 0, NULL, NULL, NULL);

    DefineCustomRealVariable("pg_auto_reindex.max_idle_load",
                             "Maximum 1-min CPU load average allowed for idle state",
                             NULL, &guc_max_idle_load, 2.0, 0.1, 100.0,
                             PGC_SIGHUP, 0, NULL, NULL, NULL);

    DefineCustomIntVariable("pg_auto_reindex.max_idle_backends",
                            "Maximum active backends allowed for idle state",
                            NULL, &guc_max_idle_backends, 15, 0, 1000,
                            PGC_SIGHUP, 0, NULL, NULL, NULL);

    DefineCustomRealVariable("pg_auto_reindex.min_bloat_ratio",
                             "Minimum estimated bloat ratio to trigger reindex",
                             NULL, &guc_min_bloat_ratio, 0.30, 0.01, 0.99,
                             PGC_SIGHUP, 0, NULL, NULL, NULL);

    DefineCustomIntVariable("pg_auto_reindex.min_bloat_bytes",
                            "Minimum index size in bytes to trigger reindex",
                            NULL, (int *) &guc_min_bloat_bytes, 1048576, 0, INT_MAX,
                            PGC_SIGHUP, GUC_UNIT_BYTE, NULL, NULL, NULL);

    DefineCustomIntVariable("pg_auto_reindex.lock_timeout_ms",
                            "Lock timeout in milliseconds during REINDEX CONCURRENTLY",
                            NULL, &guc_lock_timeout_ms, 5000, 100, 300000,
                            PGC_SIGHUP, 0, NULL, NULL, NULL);

    DefineCustomIntVariable("pg_auto_reindex.max_reindexes_per_idle",
                            "Maximum indexes reindexed per idle window",
                            NULL, &guc_max_reindexes_per_idle, 2, 1, 100,
                            PGC_SIGHUP, 0, NULL, NULL, NULL);

    if (!process_shared_preload_libraries_in_progress)
        return;

    /* Shared Memory Request */
    RequestAddinShmemSpace(sizeof(AutoReindexSharedState));
    RequestNamedLWLockTranche("pg_auto_reindex", 1);

    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = auto_reindex_shmem_startup;

    /* Register Background Worker */
    memset(&worker, 0, sizeof(worker));
    snprintf(worker.bgw_name, BGW_MAXLEN, "pg_auto_reindex worker");
    snprintf(worker.bgw_type, BGW_MAXLEN, "pg_auto_reindex");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = 30;
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "pg_auto_reindex");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "pg_auto_reindex_main");
    worker.bgw_main_arg = (Datum) 0;
    worker.bgw_notify_pid = 0;

    RegisterBackgroundWorker(&worker);
}

void
pg_auto_reindex_main(Datum main_arg)
{
    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnection(guc_database, NULL, 0);

    elog(LOG, "pg_auto_reindex worker initialized on database: %s", guc_database);

    while (!ShutdownRequestPending)
    {
        if (guc_enabled)
        {
            /* 1. Metric Sampling and EWMA Learning */
            CollectSystemMetricsAndLearnerUpdate();

            /* 2. Check if System is Idle */
            if (IsSystemIdle())
            {
                elog(DEBUG1, "pg_auto_reindex: System confirmed IDLE. Initiating bloat evaluation...");

                /* 3. Cleanup any interrupted invalid indexes first */
                CleanupInvalidIndexes();

                /* 4. Execute Bloat Evaluation & Reindex Cycle */
                ExecuteAutoReindexCycle();
            }
        }

        /* Sleep for naptime seconds or until signaled */
        (void) WaitLatch(MyLatch,
                       WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                       guc_naptime * 1000L,
                       PG_WAIT_EXTENSION);
        ResetLatch(MyLatch);
    }

    elog(LOG, "pg_auto_reindex worker exiting.");
}

/*
 * SQL Function: pg_auto_reindex_status()
 */
PG_FUNCTION_INFO_V1(pg_auto_reindex_status);
Datum
pg_auto_reindex_status(PG_FUNCTION_ARGS)
{
    TupleDesc   tupdesc;
    Datum       values[6];
    bool        nulls[6] = {false};
    HeapTuple   tuple;

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("function returning record called in context that cannot accept type record")));

    if (AutoReindexShared)
    {
        LWLockAcquire(AutoReindexShared->lock, LW_SHARED);

        values[0] = BoolGetDatum(AutoReindexShared->is_idle);
        values[1] = Int32GetDatum(AutoReindexShared->consecutive_idle_count);
        values[2] = ObjectIdGetDatum(AutoReindexShared->current_reindexing_index);
        values[3] = TimestampTzGetDatum(AutoReindexShared->last_reindex_time);
        values[4] = Int64GetDatum(AutoReindexShared->total_reindexed_count);
        values[5] = Int64GetDatum(AutoReindexShared->total_bytes_saved);

        LWLockRelease(AutoReindexShared->lock);
    }
    else
    {
        values[0] = BoolGetDatum(false);
        values[1] = Int32GetDatum(0);
        values[2] = ObjectIdGetDatum(InvalidOid);
        values[3] = TimestampTzGetDatum((TimestampTz) 0);
        values[4] = Int64GetDatum(0);
        values[5] = Int64GetDatum(0);
    }

    tuple = heap_form_tuple(tupdesc, values, nulls);
    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * SQL Function: pg_auto_reindex_trigger()
 */
PG_FUNCTION_INFO_V1(pg_auto_reindex_trigger);
Datum
pg_auto_reindex_trigger(PG_FUNCTION_ARGS)
{
    elog(LOG, "pg_auto_reindex: Manual trigger invoked.");
    CleanupInvalidIndexes();
    ExecuteAutoReindexCycle();
    PG_RETURN_BOOL(true);
}
