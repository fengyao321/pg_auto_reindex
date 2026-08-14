/*-------------------------------------------------------------------------
 *
 * executor_safe.c
 *		Safe cleanup of invalid indexes left by interrupted
 *		REINDEX CONCURRENTLY operations.
 *
 *		NOTE: The actual REINDEX CONCURRENTLY command is issued by the
 *		external control plane daemon via libpq in autocommit mode.
 *		This file only provides the cleanup utility function.
 *
 * Copyright (c) 2026, fengyao <fengyao0087@gmail.com>
 *
 * IDENTIFICATION
 *	  contrib/pg_auto_reindex/src/executor_safe.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"

#include "pg_auto_reindex.h"

/*
 * SQL Function: pg_auto_reindex_cleanup_invalid_indexes()
 *
 * Scan for invalid indexes with the '_ccnew' suffix (leftovers from
 * interrupted REINDEX CONCURRENTLY operations) and drop them.
 *
 * Returns the number of invalid indexes cleaned up.
 *
 * IMPORTANT: This function uses plain DROP INDEX (not CONCURRENTLY)
 * because DROP INDEX CONCURRENTLY cannot run inside a transaction block,
 * and SQL functions always execute within one. The caller (daemon) should
 * ensure the database is not under heavy load when invoking this.
 */
PG_FUNCTION_INFO_V1(pg_auto_reindex_cleanup_invalid_indexes);

Datum
pg_auto_reindex_cleanup_invalid_indexes(PG_FUNCTION_ARGS)
{
	int			ret;
	int			cleaned = 0;

	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());

	ret = SPI_execute(
		"SELECT n.nspname, c.relname "
		"FROM pg_class c "
		"JOIN pg_index i ON i.indexrelid = c.oid "
		"JOIN pg_namespace n ON n.oid = c.relnamespace "
		"WHERE i.indisvalid = false "
		"  AND c.relname LIKE '%_ccnew' "
		"  AND n.nspname NOT IN ('pg_catalog', 'information_schema') "
		"  AND NOT EXISTS ("
		"    SELECT 1 FROM pg_locks l "
		"    WHERE l.relation = c.oid "
		"      AND l.pid != pg_backend_pid()"
		"  ) "
		"LIMIT 10",
		true, 0);

	if (ret == SPI_OK_SELECT && SPI_processed > 0)
	{
		uint64		count = SPI_processed;
		SPITupleTable *tuptable = SPI_tuptable;
		TupleDesc	tupdesc = tuptable->tupdesc;

		for (uint64 i = 0; i < count; i++)
		{
			HeapTuple	spi_tuple = tuptable->vals[i];
			char	   *schema = SPI_getvalue(spi_tuple, tupdesc, 1);
			char	   *index = SPI_getvalue(spi_tuple, tupdesc, 2);
			char		drop_cmd[512];

			snprintf(drop_cmd, sizeof(drop_cmd),
					 "DROP INDEX IF EXISTS %s.%s",
					 quote_identifier(schema),
					 quote_identifier(index));

			elog(LOG, "pg_auto_reindex: cleaning up invalid index: %s.%s",
				 schema, index);

			PG_TRY();
			{
				SPI_exec(drop_cmd, 0);
				cleaned++;
			}
			PG_CATCH();
			{
				ErrorData  *edata;

				MemoryContextSwitchTo(TopMemoryContext);
				edata = CopyErrorData();
				FlushErrorState();
				ereport(WARNING,
						(errmsg("pg_auto_reindex: failed to drop invalid index %s.%s: %s",
								schema, index, edata->message)));
				FreeErrorData(edata);
			}
			PG_END_TRY();

			if (schema)
				pfree(schema);
			if (index)
				pfree(index);
		}
	}

	PopActiveSnapshot();
	SPI_finish();

	PG_RETURN_INT32(cleaned);
}
