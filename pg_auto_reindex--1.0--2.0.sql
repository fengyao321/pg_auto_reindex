/* contrib/pg_auto_reindex/pg_auto_reindex--1.0--2.0.sql */

-- complain if script is sourced in psql rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_auto_reindex" to load this file. \quit

-- Drop old functions
DROP FUNCTION IF EXISTS pg_auto_reindex_stats(regclass);
DROP FUNCTION IF EXISTS pg_auto_reindex_trigger();

-- Alter history table
ALTER TABLE pg_auto_reindex_history ADD COLUMN dbname name NOT NULL DEFAULT current_database();
ALTER TABLE pg_auto_reindex_history ADD COLUMN error_message text;

-- EWMA learning stats persistence table (used by external daemon)
CREATE TABLE IF NOT EXISTS pg_auto_reindex_learning_stats (
    slot_id         int NOT NULL PRIMARY KEY CHECK (slot_id >= 0 AND slot_id < 168),
    day_of_week     int NOT NULL CHECK (day_of_week >= 0 AND day_of_week <= 6),
    hour_of_day     int NOT NULL CHECK (hour_of_day >= 0 AND hour_of_day <= 23),
    ewma_cpu_usage      double precision NOT NULL DEFAULT 0.0,
    ewma_active_backends double precision NOT NULL DEFAULT 0.0,
    ewma_io_read_bytes  double precision NOT NULL DEFAULT 0.0,
    ewma_io_write_bytes double precision NOT NULL DEFAULT 0.0,
    sample_count    bigint NOT NULL DEFAULT 0,
    updated_at      timestamptz NOT NULL DEFAULT now()
);

SELECT pg_catalog.pg_extension_config_dump('pg_auto_reindex_learning_stats', '');

-- Initialize 168 learning slots
INSERT INTO pg_auto_reindex_learning_stats (slot_id, day_of_week, hour_of_day)
SELECT s, s / 24, s % 24
FROM generate_series(0, 167) AS s
ON CONFLICT (slot_id) DO NOTHING;

-- ============================================================
-- C Extension SQL Functions
-- ============================================================

-- Check bloat for a specific index (precise B-Tree physical page estimation)
CREATE FUNCTION pg_auto_reindex_bloat_check(
    IN relation regclass,
    OUT bloat_bytes bigint,
    OUT bloat_ratio double precision,
    OUT expected_pages bigint,
    OUT current_pages bigint,
    OUT is_reliable boolean
)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_auto_reindex_bloat_check'
LANGUAGE C STRICT PARALLEL SAFE;

COMMENT ON FUNCTION pg_auto_reindex_bloat_check(regclass) IS
'Estimate B-Tree index bloat using precise physical page layout calculations.';

-- Scan all B-Tree indexes and report bloat
CREATE FUNCTION pg_auto_reindex_bloat_report(
    OUT index_oid oid,
    OUT schemaname name,
    OUT indexname name,
    OUT current_bytes bigint,
    OUT bloat_ratio double precision,
    OUT bloat_bytes bigint,
    OUT is_reliable boolean
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_auto_reindex_bloat_report'
LANGUAGE C STRICT PARALLEL SAFE;

COMMENT ON FUNCTION pg_auto_reindex_bloat_report() IS
'Scan all user B-Tree indexes and report estimated bloat using physical page layout analysis.';

-- View current extension status from shared memory
CREATE FUNCTION pg_auto_reindex_status(
    OUT current_reindexing_index oid,
    OUT last_reindex_time timestamptz,
    OUT total_reindexed_count bigint,
    OUT total_bytes_saved bigint
)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_auto_reindex_status'
LANGUAGE C STRICT PARALLEL SAFE;

COMMENT ON FUNCTION pg_auto_reindex_status() IS
'View current pg_auto_reindex shared memory state and global metrics.';

-- Pre-flight safety check before reindexing
CREATE FUNCTION pg_auto_reindex_preflight_check(
    IN relation regclass,
    OUT safe boolean,
    OUT blocking_pids int[],
    OUT reason text
)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_auto_reindex_preflight_check'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_auto_reindex_preflight_check(regclass) IS
'Check if it is safe to run REINDEX CONCURRENTLY on the given index (no long transactions, no lock conflicts).';

-- Mark an index as being reindexed (called by daemon before REINDEX)
CREATE FUNCTION pg_auto_reindex_record_start(
    IN relation regclass
)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_auto_reindex_record_start'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_auto_reindex_record_start(regclass) IS
'Record in shared memory that a reindex operation has started on the given index.';

-- Record reindex completion (called by daemon after REINDEX)
CREATE FUNCTION pg_auto_reindex_record_finish(
    IN relation regclass,
    IN success boolean,
    IN bytes_before bigint,
    IN bytes_after bigint,
    IN error_msg text DEFAULT NULL
)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_auto_reindex_record_finish'
LANGUAGE C;

COMMENT ON FUNCTION pg_auto_reindex_record_finish(regclass, boolean, bigint, bigint, text) IS
'Record reindex completion in shared memory and write audit trail to pg_auto_reindex_history.';
