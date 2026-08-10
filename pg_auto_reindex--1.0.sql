/* contrib/pg_auto_reindex/pg_auto_reindex--1.0.sql */

-- complain if script is sourced in psql rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_auto_reindex" to load this file. \quit

-- Audit history table for reindex events
CREATE TABLE IF NOT EXISTS pg_auto_reindex_history (
    id              bigserial PRIMARY KEY,
    schemaname      name NOT NULL,
    indexname       name NOT NULL,
    start_time      timestamptz NOT NULL,
    end_time        timestamptz NOT NULL,
    bytes_before    bigint NOT NULL,
    bytes_after     bigint NOT NULL,
    bytes_saved     bigint GENERATED ALWAYS AS (bytes_before - bytes_after) STORED,
    status          text NOT NULL
);

SELECT pg_catalog.pg_extension_config_dump('pg_auto_reindex_history', '');
SELECT pg_catalog.pg_extension_config_dump('pg_auto_reindex_history_id_seq', '');

-- View 168 time-slot EWMA statistics
CREATE FUNCTION pg_auto_reindex_stats(
    OUT slot_id int,
    OUT day_of_week int,
    OUT hour_of_day int,
    OUT ewma_loadavg float8,
    OUT ewma_active_backends float8,
    OUT ewma_wal_bytes_per_sec float8,
    OUT sample_count bigint,
    OUT is_current_slot bool
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_auto_reindex_stats'
LANGUAGE C STRICT PARALLEL RESTRICTED;

-- View estimated B-Tree index bloat across the database
CREATE FUNCTION pg_auto_reindex_bloat_report(
    OUT index_oid oid,
    OUT schemaname name,
    OUT indexname name,
    OUT current_bytes bigint,
    OUT estimated_bloat_ratio float8,
    OUT estimated_bloat_bytes bigint
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_auto_reindex_bloat_report'
LANGUAGE C STRICT PARALLEL RESTRICTED;

-- View current background worker status and metrics
CREATE FUNCTION pg_auto_reindex_status(
    OUT is_idle bool,
    OUT consecutive_idle_count int,
    OUT current_reindexing_index oid,
    OUT last_reindex_time timestamptz,
    OUT total_reindexed_count bigint,
    OUT total_bytes_saved bigint
)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_auto_reindex_status'
LANGUAGE C STRICT PARALLEL RESTRICTED;

-- Manually trigger a check and reindex run
CREATE FUNCTION pg_auto_reindex_trigger()
RETURNS bool
AS 'MODULE_PATHNAME', 'pg_auto_reindex_trigger'
LANGUAGE C STRICT PARALLEL RESTRICTED;
