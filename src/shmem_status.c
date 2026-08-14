/*-------------------------------------------------------------------------
 *
 * shmem_status.c
 *		Shared memory state exposure, pre-flight safety checks, and
 *		reindex lifecycle recording for daemon coordination.
 *
 * Copyright (c) 2026, fengyao <fengyao0087@gmail.com>
 *
 * IDENTIFICATION
 *	  contrib/pg_auto_reindex/src/shmem_status.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/lwlock.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"

#include "pg_auto_reindex.h"

/* ----------------------------------------------------------------
 * pg_auto_reindex_status()
 *
 * Returns current shared memory state and global metrics.
 * ----------------------------------------------------------------
 */
PG_FUNCTION_INFO_V1(pg_auto_reindex_status);

Datum
pg_auto_reindex_status(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		nulls[4] = {false};
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context "
						"that cannot accept type record")));

	if (AutoReindexShared)
	{
		LWLockAcquire(AutoReindexShared->lock, LW_SHARED);

		values[0] = ObjectIdGetDatum(AutoReindexShared->current_reindexing_index);
		values[1] = TimestampTzGetDatum(AutoReindexShared->last_reindex_time);
		values[2] = Int64GetDatum(AutoReindexShared->total_reindexed_count);
		values[3] = Int64GetDatum(AutoReindexShared->total_bytes_saved);

		LWLockRelease(AutoReindexShared->lock);
	}
	else
	{
		values[0] = ObjectIdGetDatum(InvalidOid);
		nulls[1] = true;
		values[2] = Int64GetDatum(0);
		values[3] = Int64GetDatum(0);
	}

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/* ----------------------------------------------------------------
 * pg_auto_reindex_preflight_check(regclass)
 *
 * Check whether it is safe to run REINDEX CONCURRENTLY on a given
 * index by detecting long-running transactions that touch the
 * underlying table.
 *
 * Returns (safe bool, blocking_pids int[], reason text).
 * ----------------------------------------------------------------
 */
PG_FUNCTION_INFO_V1(pg_auto_reindex_preflight_check);

Datum
pg_auto_reindex_preflight_check(PG_FUNCTION_ARGS)
{
	Oid			index_oid = PG_GETARG_OID(0);
	TupleDesc	tupdesc;
	Datum		values[3];
	bool		nulls[3] = {false};
	HeapTuple	tuple;
	bool		safe = true;
	StringInfoData reason_buf;
	Datum	   *pid_datums = NULL;
	int			npids = 0;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context "
						"that cannot accept type record")));

	initStringInfo(&reason_buf);

	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());

	/*
	 * Check 1: Find long-running transactions that hold locks on the
	 * index's parent table. These would block REINDEX CONCURRENTLY
	 * from completing (it needs to wait for all old snapshots).
	 */
	{
		StringInfoData query;
		int			ret;

		initStringInfo(&query);
		appendStringInfo(&query,
			"SELECT DISTINCT a.pid "
			"FROM pg_stat_activity a "
			"WHERE a.state IN ('active', 'idle in transaction') "
			"  AND a.pid != pg_backend_pid() "
			"  AND EXTRACT(EPOCH FROM (now() - a.xact_start)) > %d "
			"  AND EXISTS ("
			"    SELECT 1 FROM pg_locks l "
			"    JOIN pg_index i ON i.indrelid = l.relation "
			"    WHERE i.indexrelid = %u "
			"      AND l.pid = a.pid"
			"  ) "
			"LIMIT 20",
			guc_max_xact_duration, index_oid);

		ret = SPI_execute(query.data, true, 0);
		if (ret == SPI_OK_SELECT && SPI_processed > 0)
		{
			safe = false;
			npids = (int) SPI_processed;
			pid_datums = (Datum *) palloc(sizeof(Datum) * npids);

			for (int i = 0; i < npids; i++)
			{
				bool	isnull;
				int32	pid;

				pid = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[i],
												  SPI_tuptable->tupdesc, 1,
												  &isnull));
				pid_datums[i] = Int32GetDatum(pid);
			}

			appendStringInfo(&reason_buf,
				"Found %d long-running transaction(s) (> %ds) holding locks "
				"on the parent table of index %u",
				npids, guc_max_xact_duration, index_oid);
		}

		pfree(query.data);
	}

	/*
	 * Check 2: Verify the index is not already being reindexed
	 * by another worker.
	 */
	if (safe && AutoReindexShared)
	{
		LWLockAcquire(AutoReindexShared->lock, LW_SHARED);
		if (AutoReindexShared->current_reindexing_index == index_oid)
		{
			safe = false;
			appendStringInfo(&reason_buf,
				"Index %u is already being reindexed by another worker",
				index_oid);
		}
		LWLockRelease(AutoReindexShared->lock);
	}

	/*
	 * Check 3: Verify no idle-in-transaction sessions exceeding threshold.
	 */
	if (safe)
	{
		StringInfoData query;
		int			ret;

		initStringInfo(&query);
		appendStringInfo(&query,
			"SELECT count(*) FROM pg_stat_activity "
			"WHERE state = 'idle in transaction' "
			"  AND EXTRACT(EPOCH FROM (now() - xact_start)) > %d",
			guc_max_xact_duration);

		ret = SPI_execute(query.data, true, 1);
		if (ret == SPI_OK_SELECT && SPI_processed > 0)
		{
			bool	isnull;
			int64	idle_xact_count;

			idle_xact_count = DatumGetInt64(SPI_getbinval(
				SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));

			if (idle_xact_count > 0)
			{
				safe = false;
				appendStringInfo(&reason_buf,
					"Found %ld idle-in-transaction session(s) exceeding %ds; "
					"REINDEX CONCURRENTLY may hang waiting for snapshot completion",
					(long) idle_xact_count, guc_max_xact_duration);
			}
		}

		pfree(query.data);
	}

	PopActiveSnapshot();
	SPI_finish();

	/* Build result */
	if (safe)
		appendStringInfoString(&reason_buf, "All pre-flight checks passed");

	values[0] = BoolGetDatum(safe);

	if (npids > 0)
	{
		ArrayType *arr = construct_array(pid_datums, npids, INT4OID,
										 sizeof(int32), true, TYPALIGN_INT);
		values[1] = PointerGetDatum(arr);
	}
	else
	{
		values[1] = PointerGetDatum(construct_empty_array(INT4OID));
	}

	values[2] = PointerGetDatum(cstring_to_text(reason_buf.data));

	pfree(reason_buf.data);
	if (pid_datums)
		pfree(pid_datums);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/* ----------------------------------------------------------------
 * pg_auto_reindex_record_start(regclass)
 *
 * Mark an index as currently being reindexed in shared memory.
 * Called by the daemon before issuing REINDEX CONCURRENTLY.
 * ----------------------------------------------------------------
 */
PG_FUNCTION_INFO_V1(pg_auto_reindex_record_start);

Datum
pg_auto_reindex_record_start(PG_FUNCTION_ARGS)
{
	Oid		index_oid = PG_GETARG_OID(0);

	if (!AutoReindexShared)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pg_auto_reindex shared memory not initialized"),
				 errhint("Add pg_auto_reindex to shared_preload_libraries.")));

	LWLockAcquire(AutoReindexShared->lock, LW_EXCLUSIVE);
	AutoReindexShared->current_reindexing_index = index_oid;
	LWLockRelease(AutoReindexShared->lock);

	elog(LOG, "pg_auto_reindex: recording reindex start for index %u", index_oid);

	PG_RETURN_VOID();
}

/* ----------------------------------------------------------------
 * pg_auto_reindex_record_finish(regclass, success, bytes_before,
 *                                bytes_after, error_msg)
 *
 * Record reindex completion: update shared memory metrics and
 * write an audit trail entry to pg_auto_reindex_history.
 * ----------------------------------------------------------------
 */
PG_FUNCTION_INFO_V1(pg_auto_reindex_record_finish);

Datum
pg_auto_reindex_record_finish(PG_FUNCTION_ARGS)
{
	Oid			index_oid = PG_GETARG_OID(0);
	bool		success = PG_GETARG_BOOL(1);
	int64		bytes_before = PG_GETARG_INT64(2);
	int64		bytes_after = PG_GETARG_INT64(3);
	text	   *error_msg = PG_ARGISNULL(4) ? NULL : PG_GETARG_TEXT_PP(4);

	/* Update shared memory state */
	if (AutoReindexShared)
	{
		LWLockAcquire(AutoReindexShared->lock, LW_EXCLUSIVE);

		if (AutoReindexShared->current_reindexing_index == index_oid)
			AutoReindexShared->current_reindexing_index = InvalidOid;

		AutoReindexShared->last_reindex_time = GetCurrentTimestamp();

		if (success && bytes_before > bytes_after)
		{
			AutoReindexShared->total_reindexed_count++;
			AutoReindexShared->total_bytes_saved += (bytes_before - bytes_after);
		}

		LWLockRelease(AutoReindexShared->lock);
	}

	/* Write audit record to pg_auto_reindex_history */
	{
		int			ret;
		Oid			argtypes[5] = {NAMEOID, NAMEOID, INT8OID, INT8OID, TEXTOID};
		Datum		argvals[5];
		char		nulls_str[5] = {' ', ' ', ' ', ' ', ' '};
		StringInfoData names_query;

		SPI_connect();
		PushActiveSnapshot(GetTransactionSnapshot());

		/*
		 * Look up the schema and index name from the OID so the history
		 * record is human-readable.
		 */
		initStringInfo(&names_query);
		appendStringInfo(&names_query,
			"SELECT n.nspname, c.relname "
			"FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
			"WHERE c.oid = %u", index_oid);

		ret = SPI_execute(names_query.data, true, 1);
		if (ret == SPI_OK_SELECT && SPI_processed > 0)
		{
			char   *schema = SPI_getvalue(SPI_tuptable->vals[0],
										  SPI_tuptable->tupdesc, 1);
			char   *idxname = SPI_getvalue(SPI_tuptable->vals[0],
										   SPI_tuptable->tupdesc, 2);

			argvals[0] = CStringGetDatum(schema);
			argvals[1] = CStringGetDatum(idxname);
			argvals[2] = Int64GetDatum(bytes_before);
			argvals[3] = Int64GetDatum(bytes_after);

			if (error_msg)
				argvals[4] = PointerGetDatum(error_msg);
			else
				nulls_str[4] = 'n';

			ret = SPI_execute_with_args(
				"INSERT INTO pg_auto_reindex_history "
				"(schemaname, indexname, start_time, end_time, "
				" bytes_before, bytes_after, status, error_message) "
				"VALUES ($1, $2, now(), now(), $3, $4, "
				"  CASE WHEN $3 > $4 THEN 'SUCCESS' ELSE 'FAILED' END, "
				"  $5)",
				5, argtypes, argvals, nulls_str, false, 0);

			if (ret != SPI_OK_INSERT)
				elog(WARNING, "pg_auto_reindex: failed to insert history record");

			if (schema)
				pfree(schema);
			if (idxname)
				pfree(idxname);
		}

		pfree(names_query.data);
		PopActiveSnapshot();
		SPI_finish();
	}

	elog(LOG, "pg_auto_reindex: recorded reindex finish for index %u (success=%s, saved=%ld bytes)",
		 index_oid, success ? "true" : "false",
		 success ? (long)(bytes_before - bytes_after) : 0L);

	PG_RETURN_VOID();
}
