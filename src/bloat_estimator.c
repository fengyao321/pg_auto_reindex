/*-------------------------------------------------------------------------
 *
 * bloat_estimator.c
 *		Precision B-Tree index bloat estimation using physical page
 *		layout calculations.
 *
 *		Accounts for PageHeaderData, BTPageOpaqueData, ItemIdData,
 *		IndexTupleData, MAXALIGN padding, fillfactor, and handles
 *		expression indexes and stale statistics gracefully.
 *
 * Copyright (c) 2026, fengyao <fengyao0087@gmail.com>
 *
 * IDENTIFICATION
 *	  contrib/pg_auto_reindex/src/bloat_estimator.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/tuplestore.h"

#include "pg_auto_reindex.h"

/*
 * B-Tree physical page layout constants.
 *
 * A standard 8KB page has the following overhead:
 *   - PageHeaderData:     24 bytes (SizeOfPageHeaderData)
 *   - BTPageOpaqueData:   16 bytes (after MAXALIGN)
 *   - Each tuple also needs an ItemIdData (4 bytes) in the line pointer array
 *   - Each index tuple has an IndexTupleData header (8 bytes)
 *
 * Default B-Tree leaf page fillfactor is 90%.
 */
#define BT_PAGE_OVERHEAD	(SizeOfPageHeaderData + MAXALIGN(sizeof(BTPageOpaqueData)))
#define BT_USABLE_PAGE_SIZE (BLCKSZ - BT_PAGE_OVERHEAD)
#define BT_DEFAULT_FILLFACTOR	90

/*
 * Estimate the average tuple width for a given B-Tree index by querying
 * pg_statistic for the widths of the indexed columns. For expression
 * indexes (indkey[i] == 0), fall back to the data type's default width.
 *
 * Returns the estimated average data width in bytes (excluding tuple header).
 */
static int
estimate_index_tuple_width(Oid index_oid)
{
	StringInfoData buf;
	int			ret;
	int			avg_width = 8;	/* fallback default */

	initStringInfo(&buf);
	appendStringInfo(&buf,
		"SELECT COALESCE(SUM("
		"  CASE WHEN a.attnum > 0 THEN "
		"    COALESCE((SELECT stawidth FROM pg_statistic "
		"              WHERE starelid = i.indrelid "
		"                AND staattnum = a.attnum LIMIT 1), "
		"             a.atttypmod) "
		"  ELSE "
		"    pg_catalog.pg_column_size(NULL::int) "  /* expression index fallback */
		"  END"
		"), 8)::int AS avg_width "
		"FROM pg_index i "
		"JOIN LATERAL unnest(i.indkey) WITH ORDINALITY AS k(attnum, ord) ON true "
		"LEFT JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = k.attnum "
		"WHERE i.indexrelid = %u", index_oid);

	ret = SPI_execute(buf.data, true, 1);
	if (ret == SPI_OK_SELECT && SPI_processed > 0)
	{
		bool	isnull;
		Datum	val = SPI_getbinval(SPI_tuptable->vals[0],
								   SPI_tuptable->tupdesc, 1, &isnull);
		if (!isnull)
		{
			int w = DatumGetInt32(val);
			if (w > 0)
				avg_width = w;
		}
	}

	pfree(buf.data);
	return avg_width;
}

/*
 * Retrieve the fillfactor for the given index. Returns the default
 * B-Tree fillfactor (90) if none is explicitly set.
 */
static int
get_index_fillfactor(Oid index_oid)
{
	StringInfoData buf;
	int			ret;
	int			fillfactor = BT_DEFAULT_FILLFACTOR;

	initStringInfo(&buf);
	appendStringInfo(&buf,
		"SELECT COALESCE("
		"  (SELECT (option_value::int) "
		"   FROM pg_options_to_table(c.reloptions) "
		"   WHERE option_name = 'fillfactor'), %d) "
		"FROM pg_class c WHERE c.oid = %u",
		BT_DEFAULT_FILLFACTOR, index_oid);

	ret = SPI_execute(buf.data, true, 1);
	if (ret == SPI_OK_SELECT && SPI_processed > 0)
	{
		bool	isnull;
		Datum	val = SPI_getbinval(SPI_tuptable->vals[0],
								   SPI_tuptable->tupdesc, 1, &isnull);
		if (!isnull)
			fillfactor = DatumGetInt32(val);
	}

	pfree(buf.data);
	return fillfactor;
}

/*
 * Core bloat estimation logic for a single B-Tree index.
 *
 * Populates the output parameters with estimated bloat metrics.
 */
static void
estimate_btree_bloat(Oid index_oid,
					 int64 *bloat_bytes, float8 *bloat_ratio,
					 int64 *expected_pages, int64 *current_pages,
					 bool *is_reliable)
{
	StringInfoData buf;
	int			ret;

	*bloat_bytes = 0;
	*bloat_ratio = 0.0;
	*expected_pages = 0;
	*current_pages = 0;
	*is_reliable = true;

	/* Fetch relpages and reltuples from pg_class */
	initStringInfo(&buf);
	appendStringInfo(&buf,
		"SELECT c.relpages, c.reltuples "
		"FROM pg_class c WHERE c.oid = %u AND c.relkind = 'i'",
		index_oid);

	ret = SPI_execute(buf.data, true, 1);
	if (ret != SPI_OK_SELECT || SPI_processed == 0)
	{
		*is_reliable = false;
		pfree(buf.data);
		return;
	}

	{
		bool		isnull;
		int32		relpages;
		float4		reltuples;
		int			avg_width;
		int			fillfactor;
		int			tuple_size;
		int			effective_capacity;
		int			tuples_per_page;

		relpages = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[0],
											   SPI_tuptable->tupdesc, 1, &isnull));
		reltuples = DatumGetFloat4(SPI_getbinval(SPI_tuptable->vals[0],
												 SPI_tuptable->tupdesc, 2, &isnull));

		*current_pages = relpages;

		/* Stale statistics check */
		if (reltuples < 0 || relpages <= 0)
		{
			*is_reliable = false;
			pfree(buf.data);
			return;
		}

		/* Zero tuples means empty index - no bloat */
		if (reltuples < 1.0)
		{
			*expected_pages = 1;	/* at least the metapage */
			pfree(buf.data);
			return;
		}

		/* Get average tuple width from pg_statistic */
		avg_width = estimate_index_tuple_width(index_oid);

		/* Get fillfactor (default 90 for B-Tree) */
		fillfactor = get_index_fillfactor(index_oid);

		/*
		 * Calculate tuple size with MAXALIGN:
		 *   IndexTupleData header (8 bytes) + data width, then MAXALIGN
		 */
		tuple_size = MAXALIGN(sizeof(IndexTupleData) + avg_width);

		/*
		 * Effective page capacity considering fillfactor:
		 *   UsablePageSize * (fillfactor / 100.0)
		 */
		effective_capacity = (int)(BT_USABLE_PAGE_SIZE * (fillfactor / 100.0));

		/*
		 * Tuples per page:
		 *   Each tuple needs tuple_size + sizeof(ItemIdData) for line pointer
		 */
		tuples_per_page = effective_capacity / (tuple_size + sizeof(ItemIdData));
		if (tuples_per_page < 1)
			tuples_per_page = 1;

		/* Expected pages (add 1 for metapage) */
		*expected_pages = (int64)ceil((double)reltuples / tuples_per_page) + 1;

		/* Calculate bloat */
		if (*current_pages > *expected_pages)
		{
			*bloat_bytes = (*current_pages - *expected_pages) * BLCKSZ;
			*bloat_ratio = (float8)(*current_pages - *expected_pages) / *current_pages;
		}

		/*
		 * Reliability check: if ratio of expected to actual is wildly off
		 * (e.g., expected > actual * 2), statistics may be severely stale.
		 */
		if (*expected_pages > *current_pages * 2)
			*is_reliable = false;
	}

	pfree(buf.data);
}

/*
 * SQL Function: pg_auto_reindex_bloat_check(regclass)
 *
 * Estimate B-Tree index bloat for a single index using precise
 * physical page layout calculations.
 */
PG_FUNCTION_INFO_V1(pg_auto_reindex_bloat_check);

Datum
pg_auto_reindex_bloat_check(PG_FUNCTION_ARGS)
{
	Oid			index_oid = PG_GETARG_OID(0);
	TupleDesc	tupdesc;
	Datum		values[5];
	bool		nulls[5] = {false};
	HeapTuple	tuple;
	int64		bloat_bytes;
	float8		bloat_ratio;
	int64		expected_pages;
	int64		current_pages;
	bool		is_reliable;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context "
						"that cannot accept type record")));

	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());

	estimate_btree_bloat(index_oid,
						 &bloat_bytes, &bloat_ratio,
						 &expected_pages, &current_pages,
						 &is_reliable);

	PopActiveSnapshot();
	SPI_finish();

	values[0] = Int64GetDatum(bloat_bytes);
	values[1] = Float8GetDatum(bloat_ratio);
	values[2] = Int64GetDatum(expected_pages);
	values[3] = Int64GetDatum(current_pages);
	values[4] = BoolGetDatum(is_reliable);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/*
 * SQL Function: pg_auto_reindex_bloat_report()
 *
 * Scan all user B-Tree indexes and return bloat estimates as a set.
 * Uses InitMaterializedSRF for PostgreSQL 15+ style SRF implementation.
 */
PG_FUNCTION_INFO_V1(pg_auto_reindex_bloat_report);

Datum
pg_auto_reindex_bloat_report(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	int			ret;

	InitMaterializedSRF(fcinfo, 0);

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	tupdesc = rsinfo->expectedDesc;

	oldcontext = MemoryContextSwitchTo(per_query_ctx);
	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;
	MemoryContextSwitchTo(oldcontext);

	SPI_connect();
	PushActiveSnapshot(GetTransactionSnapshot());

	/* Find all user B-Tree indexes (relam = 403 is btree) */
	ret = SPI_execute(
		"SELECT c.oid, n.nspname, c.relname, "
		"       pg_relation_size(c.oid) AS current_bytes "
		"FROM pg_class c "
		"JOIN pg_index i ON i.indexrelid = c.oid "
		"JOIN pg_namespace n ON n.oid = c.relnamespace "
		"WHERE c.relkind = 'i' "
		"  AND c.relam = 403 "
		"  AND i.indisvalid = true "
		"  AND n.nspname NOT IN ('pg_catalog', 'information_schema', 'pg_toast') "
		"ORDER BY pg_relation_size(c.oid) DESC",
		true, 0);

	if (ret == SPI_OK_SELECT && SPI_tuptable != NULL)
	{
		uint64		proc = SPI_processed;
		SPITupleTable *tuptable = SPI_tuptable;
		TupleDesc	spi_tupdesc = tuptable->tupdesc;

		for (uint64 i = 0; i < proc; i++)
		{
			HeapTuple	spi_tuple = tuptable->vals[i];
			bool		isnull;
			Oid			idx_oid;
			char	   *schemaname;
			char	   *indexname;
			int64		current_bytes;
			int64		bloat_bytes;
			float8		bloat_ratio;
			int64		expected_pages;
			int64		current_pages;
			bool		is_reliable;
			Datum		out_values[7];
			bool		out_nulls[7] = {false};

			idx_oid = DatumGetObjectId(SPI_getbinval(spi_tuple, spi_tupdesc, 1, &isnull));
			schemaname = SPI_getvalue(spi_tuple, spi_tupdesc, 2);
			indexname = SPI_getvalue(spi_tuple, spi_tupdesc, 3);
			current_bytes = DatumGetInt64(SPI_getbinval(spi_tuple, spi_tupdesc, 4, &isnull));

			/* Estimate bloat for this index */
			estimate_btree_bloat(idx_oid,
								&bloat_bytes, &bloat_ratio,
								&expected_pages, &current_pages,
								&is_reliable);

			out_values[0] = ObjectIdGetDatum(idx_oid);
			out_values[1] = CStringGetDatum(schemaname);
			out_values[2] = CStringGetDatum(indexname);
			out_values[3] = Int64GetDatum(current_bytes);
			out_values[4] = Float8GetDatum(bloat_ratio);
			out_values[5] = Int64GetDatum(bloat_bytes);
			out_values[6] = BoolGetDatum(is_reliable);

			tuplestore_putvalues(tupstore, tupdesc, out_values, out_nulls);

			if (schemaname)
				pfree(schemaname);
			if (indexname)
				pfree(indexname);
		}
	}

	PopActiveSnapshot();
	SPI_finish();

	return (Datum) 0;
}
