/*-------------------------------------------------------------------------
 *
 * bloat_estimator.c
 *		Metadata-based low-overhead B-Tree index bloat estimation.
 *
 * Copyright (c) 2026, fengyao <fengyao0087@gmail.com>
 *
 * IDENTIFICATION
 *	  contrib/pg_auto_reindex/bloat_estimator.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"

#include "pg_auto_reindex.h"

static const char *bloat_sql =
    "SELECT "
    "  c.oid AS index_oid, "
    "  n.nspname AS schemaname, "
    "  c.relname AS indexname, "
    "  pg_relation_size(c.oid) AS current_bytes, "
    "  ROUND( "
    "    (CASE WHEN c.relpages > 0 THEN "
    "      ((c.relpages - GREATEST(CEIL(c.reltuples * (COALESCE(s.avg_width, 8) + 8) / (8192 - 64)), 1)) / c.relpages::numeric) "
    "    ELSE 0 END)::numeric, 2)::float8 AS estimated_bloat_ratio, "
    "  (pg_relation_size(c.oid) - (GREATEST(CEIL(c.reltuples * (COALESCE(s.avg_width, 8) + 8) / (8192 - 64)), 1) * 8192))::bigint AS estimated_bloat_bytes "
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
    "ORDER BY estimated_bloat_bytes DESC;";

/*
 * SQL Function: pg_auto_reindex_bloat_report()
 */
PG_FUNCTION_INFO_V1(pg_auto_reindex_bloat_report);
Datum
pg_auto_reindex_bloat_report(PG_FUNCTION_ARGS)
{
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc   tupdesc;
    Tuplestorestate *tupstore;
    MemoryContext per_query_ctx;
    MemoryContext oldcontext;
    int ret;
    Oid argtypes[2] = {INT8OID, FLOAT8OID};
    Datum argvals[2];

    bool started_xact = false;

    InitMaterializedSRF(fcinfo, 0);

    per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
    tupdesc = rsinfo->expectedDesc;

    oldcontext = MemoryContextSwitchTo(per_query_ctx);
    tupstore = tuplestore_begin_heap(true, false, work_mem);
    rsinfo->setResult = tupstore;
    rsinfo->setDesc = tupdesc;
    MemoryContextSwitchTo(oldcontext);

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

    ret = SPI_execute_with_args(bloat_sql, 2, argtypes, argvals, NULL, true, 0);
    if (ret == SPI_OK_SELECT && SPI_tuptable != NULL)
    {
        TupleDesc spi_tupdesc = SPI_tuptable->tupdesc;
        uint64 proc = SPI_processed;

        for (uint64 i = 0; i < proc; i++)
        {
            HeapTuple tuple = SPI_tuptable->vals[i];
            Datum values[6];
            bool nulls[6] = {false};

            values[0] = SPI_getbinval(tuple, spi_tupdesc, 1, &nulls[0]);
            values[1] = SPI_getbinval(tuple, spi_tupdesc, 2, &nulls[1]);
            values[2] = SPI_getbinval(tuple, spi_tupdesc, 3, &nulls[2]);
            values[3] = SPI_getbinval(tuple, spi_tupdesc, 4, &nulls[3]);
            values[4] = SPI_getbinval(tuple, spi_tupdesc, 5, &nulls[4]);
            values[5] = SPI_getbinval(tuple, spi_tupdesc, 6, &nulls[5]);

            tuplestore_putvalues(tupstore, tupdesc, values, nulls);
        }
    }

    SPI_finish();
    PopActiveSnapshot();

    if (started_xact)
        CommitTransactionCommand();

    return (Datum) 0;
}
