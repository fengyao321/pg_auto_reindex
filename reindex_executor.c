/*-------------------------------------------------------------------------
 *
 * reindex_executor.c
 *		Safe background concurrent reindexing with lock timeout protection
 *		and invalid index self-healing.
 *
 * Copyright (c) 2026, fengyao <fengyao0087@gmail.com>
 *
 * IDENTIFICATION
 *	  contrib/pg_auto_reindex/reindex_executor.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/time.h>
#include "access/xact.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

#include "pg_auto_reindex.h"

static void
RecordReindexHistory(const char *schema, const char *index,
                      TimestampTz start_time, TimestampTz end_time,
                      int64 bytes_before, int64 bytes_after, const char *status)
{
    int ret;
    Oid argtypes[7] = {NAMEOID, NAMEOID, TIMESTAMPTZOID, TIMESTAMPTZOID, INT8OID, INT8OID, TEXTOID};
    Datum argvals[7];
    char nulls[7] = {' ', ' ', ' ', ' ', ' ', ' ', ' '};
    bool started_xact = false;
    const char *insert_sql =
        "INSERT INTO pg_auto_reindex_history "
        "(schemaname, indexname, start_time, end_time, bytes_before, bytes_after, status) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7);";

    if (!IsTransactionState())
    {
        SetCurrentStatementStartTimestamp();
        StartTransactionCommand();
        started_xact = true;
    }

    SPI_connect();
    PushActiveSnapshot(GetTransactionSnapshot());

    argvals[0] = CStringGetDatum(schema);
    argvals[1] = CStringGetDatum(index);
    argvals[2] = TimestampTzGetDatum(start_time);
    argvals[3] = TimestampTzGetDatum(end_time);
    argvals[4] = Int64GetDatum(bytes_before);
    argvals[5] = Int64GetDatum(bytes_after);
    argvals[6] = CStringGetTextDatum(status);

    ret = SPI_execute_with_args(insert_sql, 7, argtypes, argvals, nulls, false, 0);
    if (ret != SPI_OK_INSERT)
        elog(WARNING, "pg_auto_reindex: failed to record history for %s.%s", schema, index);

    SPI_finish();
    PopActiveSnapshot();

    if (started_xact)
        CommitTransactionCommand();
}

void
CleanupInvalidIndexes(void)
{
    int ret;
    bool started_xact = false;

    if (!IsTransactionState())
    {
        SetCurrentStatementStartTimestamp();
        StartTransactionCommand();
        started_xact = true;
    }

    SPI_connect();
    PushActiveSnapshot(GetTransactionSnapshot());

    ret = SPI_exec("SELECT n.nspname, c.relname "
                   "FROM pg_class c "
                   "JOIN pg_index i ON i.indexrelid = c.oid "
                   "JOIN pg_namespace n ON n.oid = c.relnamespace "
                   "WHERE i.indisvalid = false "
                   "  AND c.relname LIKE '%_ccnew' "
                   "LIMIT 5;", 0);

    if (ret == SPI_OK_SELECT && SPI_processed > 0)
    {
        uint64 count = SPI_processed;
        SPITupleTable *tuptable = SPI_tuptable;

        for (uint64 i = 0; i < count; i++)
        {
            HeapTuple tuple = tuptable->vals[i];
            TupleDesc tupdesc = tuptable->tupdesc;
            char *schema = SPI_getvalue(tuple, tupdesc, 1);
            char *index = SPI_getvalue(tuple, tupdesc, 2);
            char drop_cmd[512];

            bool is_in_xact = IsTransactionState();
            snprintf(drop_cmd, sizeof(drop_cmd),
                     is_in_xact ? "DROP INDEX IF EXISTS %s.%s;" : "DROP INDEX CONCURRENTLY IF EXISTS %s.%s;",
                     quote_identifier(schema),
                     quote_identifier(index));

            elog(LOG, "pg_auto_reindex: Cleaning up invalid leftover index: %s.%s", schema, index);

            PG_TRY();
            {
                SPI_exec(drop_cmd, 0);
            }
            PG_CATCH();
            {
                ErrorData *edata;
                MemoryContextSwitchTo(TopMemoryContext);
                edata = CopyErrorData();
                FlushErrorState();
                ereport(WARNING, (errmsg("pg_auto_reindex invalid index cleanup failed: %s", edata->message)));
                FreeErrorData(edata);
            }
            PG_END_TRY();
        }
    }

    SPI_finish();
    PopActiveSnapshot();

    if (started_xact)
        CommitTransactionCommand();
}

void
ExecuteAutoReindexCycle(void)
{
    int ret;
    uint64 candidate_count = 0;
    Oid argtypes[2] = {INT8OID, FLOAT8OID};
    Datum argvals[2];
    bool started_xact = false;

    typedef struct CandidateIndex
    {
        Oid oid;
        char *schema;
        char *name;
        int64 current_bytes;
    } CandidateIndex;

    CandidateIndex candidates[16];

    const char *query_sql =
        "SELECT "
        "  c.oid, "
        "  n.nspname, "
        "  c.relname, "
        "  pg_relation_size(c.oid) AS current_bytes "
        "FROM pg_class c "
        "JOIN pg_index i ON i.indexrelid = c.oid "
        "JOIN pg_namespace n ON n.oid = c.relnamespace "
        "LEFT JOIN ( "
        "  SELECT s.starelid, i.indexrelid, SUM(s.stawidth) AS avg_width "
        "  FROM pg_statistic s "
        "  JOIN pg_index i ON s.starelid = i.indrelid AND s.staattnum = ANY(i.indkey) "
        "  GROUP BY s.starelid, i.indexrelid "
        ") s ON s.indexrelid = c.oid "
        "WHERE c.relkind = 'i' "
        "  AND i.indisvalid = true "
        "  AND n.nspname NOT IN ('pg_catalog', 'information_schema', 'pg_toast') "
        "  AND pg_relation_size(c.oid) >= $1 "
        "  AND ROUND( "
        "    (CASE WHEN c.relpages > 0 THEN "
        "      ((c.relpages - GREATEST(CEIL(c.reltuples * (COALESCE(s.avg_width, 8) + 8) / (8192 - 64)), 1)) / c.relpages::numeric) "
        "    ELSE 0 END)::numeric, 2)::float8 >= $2 "
        "ORDER BY (pg_relation_size(c.oid) - (GREATEST(CEIL(c.reltuples * (COALESCE(s.avg_width, 8) + 8) / (8192 - 64)), 1) * 8192)) DESC "
        "LIMIT 16;";

    if (!IsTransactionState())
    {
        SetCurrentStatementStartTimestamp();
        StartTransactionCommand();
        started_xact = true;
    }

    SPI_connect();
    PushActiveSnapshot(GetTransactionSnapshot());

    argvals[0] = Int64GetDatum(guc_min_bloat_bytes);
    argvals[1] = Float8GetDatum(guc_min_bloat_ratio);

    ret = SPI_execute_with_args(query_sql, 2, argtypes, argvals, NULL, true, 0);
    if (ret == SPI_OK_SELECT && SPI_tuptable != NULL)
    {
        uint64 proc = SPI_processed;
        if (proc > (uint64) guc_max_reindexes_per_idle)
            proc = (uint64) guc_max_reindexes_per_idle;

        candidate_count = proc;
        for (uint64 i = 0; i < proc; i++)
        {
            HeapTuple tuple = SPI_tuptable->vals[i];
            TupleDesc tupdesc = SPI_tuptable->tupdesc;
            bool isnull;

            candidates[i].oid = DatumGetObjectId(SPI_getbinval(tuple, tupdesc, 1, &isnull));
            candidates[i].schema = pstrdup(SPI_getvalue(tuple, tupdesc, 2));
            candidates[i].name = pstrdup(SPI_getvalue(tuple, tupdesc, 3));
            candidates[i].current_bytes = DatumGetInt64(SPI_getbinval(tuple, tupdesc, 4, &isnull));
        }
    }

    SPI_finish();
    PopActiveSnapshot();
    if (started_xact)
        CommitTransactionCommand();

    /* Process Candidates Outside Query Transaction */
    for (uint64 i = 0; i < candidate_count; i++)
    {
        char set_lock_timeout[128];
        char reindex_cmd[512];
        char size_sql[256];
        TimestampTz start_t, end_t;
        int64 bytes_before, bytes_after;
        bool success = false;
        bool started_reindex_xact = false;
        bool is_in_xact = IsTransactionState();

        snprintf(set_lock_timeout, sizeof(set_lock_timeout),
                 "SET lock_timeout = '%dms';", guc_lock_timeout_ms);

        snprintf(reindex_cmd, sizeof(reindex_cmd),
                 is_in_xact ? "REINDEX INDEX %s.%s;" : "REINDEX INDEX CONCURRENTLY %s.%s;",
                 quote_identifier(candidates[i].schema),
                 quote_identifier(candidates[i].name));

        bytes_before = candidates[i].current_bytes;
        start_t = GetCurrentTimestamp();

        if (AutoReindexShared)
        {
            LWLockAcquire(AutoReindexShared->lock, LW_EXCLUSIVE);
            AutoReindexShared->current_reindexing_index = candidates[i].oid;
            LWLockRelease(AutoReindexShared->lock);
        }

        elog(LOG, "pg_auto_reindex: Starting REINDEX %s.%s (Size: %ld MB)",
             candidates[i].schema, candidates[i].name, bytes_before / (1024 * 1024));

        if (!IsTransactionState())
        {
            SetCurrentStatementStartTimestamp();
            StartTransactionCommand();
            started_reindex_xact = true;
        }

        SPI_connect();
        PushActiveSnapshot(GetTransactionSnapshot());

        PG_TRY();
        {
            SPI_exec(set_lock_timeout, 0);
            ret = SPI_exec(reindex_cmd, 0);
            if (ret == SPI_OK_UTILITY)
                success = true;
        }
        PG_CATCH();
        {
            ErrorData *edata;
            MemoryContextSwitchTo(TopMemoryContext);
            edata = CopyErrorData();
            FlushErrorState();
            success = false;
            elog(WARNING, "pg_auto_reindex: REINDEX %s.%s failed: %s",
                 candidates[i].schema, candidates[i].name, edata->message);
            FreeErrorData(edata);
        }
        PG_END_TRY();

        SPI_finish();
        PopActiveSnapshot();
        if (started_reindex_xact)
            CommitTransactionCommand();

        end_t = GetCurrentTimestamp();

        /* Measure After Size */
        bytes_after = bytes_before;
        if (success)
        {
            bool started_size_xact = false;
            if (!IsTransactionState())
            {
                SetCurrentStatementStartTimestamp();
                StartTransactionCommand();
                started_size_xact = true;
            }

            SPI_connect();
            PushActiveSnapshot(GetTransactionSnapshot());

            snprintf(size_sql, sizeof(size_sql),
                     "SELECT pg_relation_size('%s.%s'::regclass);",
                     quote_identifier(candidates[i].schema),
                     quote_identifier(candidates[i].name));

            if (SPI_exec(size_sql, 0) == SPI_OK_SELECT && SPI_processed > 0)
            {
                bool isnull;
                bytes_after = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
            }

            SPI_finish();
            PopActiveSnapshot();
            if (started_size_xact)
                CommitTransactionCommand();
        }

        /* Record History & Update Stats */
        RecordReindexHistory(candidates[i].schema, candidates[i].name,
                             start_t, end_t, bytes_before, bytes_after,
                             success ? "SUCCESS" : "TIMEOUT");

        if (AutoReindexShared)
        {
            LWLockAcquire(AutoReindexShared->lock, LW_EXCLUSIVE);
            AutoReindexShared->current_reindexing_index = InvalidOid;
            AutoReindexShared->last_reindex_time = end_t;
            if (success && bytes_before > bytes_after)
            {
                AutoReindexShared->total_reindexed_count++;
                AutoReindexShared->total_bytes_saved += (bytes_before - bytes_after);
            }
            LWLockRelease(AutoReindexShared->lock);
        }
    }
}
