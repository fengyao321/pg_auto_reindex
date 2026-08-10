/*-------------------------------------------------------------------------
 *
 * idle_learner.c
 *		Resource metric sampling and 168 time-slots EWMA learning.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pg_auto_reindex/idle_learner.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <stdlib.h>
#include <time.h>
#include "access/xact.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#include "pg_auto_reindex.h"

static int
GetTimeSlotIndex(void)
{
    pg_time_t   now = (pg_time_t) time(NULL);
    struct pg_tm *tm = pg_localtime(&now, log_timezone);
    int         dow = tm->tm_wday; /* 0 = Sunday, 1 = Monday, ..., 6 = Saturday */
    int         hour = tm->tm_hour;/* 0..23 */

    return (dow * 24) + hour;
}

static double
GetCpuLoadAvg(void)
{
    double loadavg[1] = {0.0};
    if (getloadavg(loadavg, 1) != -1)
        return loadavg[0];
    return 0.0;
}

static int
GetActiveBackendsCount(void)
{
    int ret;
    int active_cnt = 0;
    bool started_xact = false;

    if (!IsTransactionState())
    {
        SetCurrentStatementStartTimestamp();
        StartTransactionCommand();
        started_xact = true;
    }

    SPI_connect();
    PushActiveSnapshot(GetTransactionSnapshot());

    ret = SPI_exec("SELECT count(*) FROM pg_stat_activity "
                   "WHERE state = 'active' AND pid != pg_backend_pid();", 0);
    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        bool isnull;
        Datum val = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
        if (!isnull)
            active_cnt = DatumGetInt64(val);
    }

    SPI_finish();
    PopActiveSnapshot();

    if (started_xact)
        CommitTransactionCommand();

    return active_cnt;
}

void
CollectSystemMetricsAndLearnerUpdate(void)
{
    int slot_idx;
    double current_load;
    int current_backends;
    SlotStats *slot;

    if (!AutoReindexShared)
        return;

    slot_idx = GetTimeSlotIndex();
    if (slot_idx < 0 || slot_idx >= NUM_TIME_SLOTS)
        return;

    current_load = GetCpuLoadAvg();
    current_backends = GetActiveBackendsCount();

    LWLockAcquire(AutoReindexShared->lock, LW_EXCLUSIVE);
    slot = &AutoReindexShared->slots[slot_idx];

    if (slot->sample_count == 0)
    {
        slot->ewma_loadavg = current_load;
        slot->ewma_active_backends = (double) current_backends;
    }
    else
    {
        slot->ewma_loadavg = EWMA_ALPHA * current_load + (1.0 - EWMA_ALPHA) * slot->ewma_loadavg;
        slot->ewma_active_backends = EWMA_ALPHA * (double) current_backends + (1.0 - EWMA_ALPHA) * slot->ewma_active_backends;
    }
    slot->sample_count++;

    LWLockRelease(AutoReindexShared->lock);

    elog(DEBUG2, "pg_auto_reindex: slot [%d] load=%.2f (ewma=%.2f), backends=%d (ewma=%.2f)",
         slot_idx, current_load, slot->ewma_loadavg, current_backends, slot->ewma_active_backends);
}

bool
IsSystemIdle(void)
{
    int slot_idx;
    double current_load;
    int current_backends;
    SlotStats *slot;
    bool meets_absolute;
    bool meets_relative;
    bool is_idle;

    if (!AutoReindexShared)
        return false;

    slot_idx = GetTimeSlotIndex();
    current_load = GetCpuLoadAvg();
    current_backends = GetActiveBackendsCount();

    LWLockAcquire(AutoReindexShared->lock, LW_EXCLUSIVE);
    slot = &AutoReindexShared->slots[slot_idx];

    /* Absolute threshold check */
    meets_absolute = (current_load < guc_max_idle_load) &&
                     (current_backends < guc_max_idle_backends);

    /* Relative baseline check (load <= historical EWMA * ratio) */
    if (slot->sample_count > 0)
    {
        meets_relative = (current_load <= slot->ewma_loadavg * guc_idle_ratio_threshold) &&
                         ((double) current_backends <= slot->ewma_active_backends * guc_idle_ratio_threshold);
    }
    else
    {
        meets_relative = true; /* Baseline uninitialized, fallback to absolute */
    }

    if (meets_absolute && meets_relative)
    {
        AutoReindexShared->consecutive_idle_count++;
    }
    else
    {
        AutoReindexShared->consecutive_idle_count = 0;
    }

    /* Require at least 3 consecutive idle samples (3 minutes) to confirm IDLE */
    is_idle = (AutoReindexShared->consecutive_idle_count >= 3);
    AutoReindexShared->is_idle = is_idle;

    LWLockRelease(AutoReindexShared->lock);

    return is_idle;
}

/*
 * SQL Function: pg_auto_reindex_stats()
 */
PG_FUNCTION_INFO_V1(pg_auto_reindex_stats);
Datum
pg_auto_reindex_stats(PG_FUNCTION_ARGS)
{
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc   tupdesc;
    Tuplestorestate *tupstore;
    MemoryContext per_query_ctx;
    MemoryContext oldcontext;
    int current_slot;

    InitMaterializedSRF(fcinfo, 0);

    per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
    tupdesc = rsinfo->expectedDesc;

    oldcontext = MemoryContextSwitchTo(per_query_ctx);
    tupstore = tuplestore_begin_heap(true, false, work_mem);
    rsinfo->setResult = tupstore;
    rsinfo->setDesc = tupdesc;
    MemoryContextSwitchTo(oldcontext);

    current_slot = GetTimeSlotIndex();

    if (AutoReindexShared)
        LWLockAcquire(AutoReindexShared->lock, LW_SHARED);

    for (int i = 0; i < NUM_TIME_SLOTS; i++)
    {
        Datum values[8];
        bool nulls[8] = {false};
        SlotStats *slot = AutoReindexShared ? &AutoReindexShared->slots[i] : NULL;

        values[0] = Int32GetDatum(i);
        values[1] = Int32GetDatum(i / 24); /* day of week */
        values[2] = Int32GetDatum(i % 24); /* hour of day */
        values[3] = Float8GetDatum(slot ? slot->ewma_loadavg : 0.0);
        values[4] = Float8GetDatum(slot ? slot->ewma_active_backends : 0.0);
        values[5] = Float8GetDatum(slot ? slot->ewma_wal_bytes_per_sec : 0.0);
        values[6] = Int64GetDatum(slot ? slot->sample_count : 0);
        values[7] = BoolGetDatum(i == current_slot);

        tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }

    if (AutoReindexShared)
        LWLockRelease(AutoReindexShared->lock);

    return (Datum) 0;
}
