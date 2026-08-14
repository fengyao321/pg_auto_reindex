-- Regression test for pg_auto_reindex v2.0 extension

CREATE EXTENSION pg_auto_reindex;

-- 1. Check GUC defaults
SHOW pg_auto_reindex.lock_timeout_ms;
SHOW pg_auto_reindex.max_xact_duration;

-- 2. Check 168 time-slots stats count (should be exactly 168)
SELECT count(*) FROM pg_auto_reindex_learning_stats;

-- 3. Check status function (should be idle)
SELECT current_reindexing_index, total_reindexed_count, total_bytes_saved
FROM pg_auto_reindex_status();

-- 4. Create test table and B-Tree index
CREATE TABLE test_bloat_table (
    id serial PRIMARY KEY,
    val text,
    created_at timestamptz DEFAULT now()
);

CREATE INDEX idx_test_bloat_val ON test_bloat_table (val);
CREATE INDEX idx_test_bloat_created ON test_bloat_table (created_at);

-- Populate table with 20,000 rows
INSERT INTO test_bloat_table (val)
SELECT md5(i::text) FROM generate_series(1, 20000) i;

-- Analyze to update stats
ANALYZE test_bloat_table;

-- 5. Check bloat check on a specific index (sanity check)
SELECT is_reliable, current_pages > 0 AS has_pages 
FROM pg_auto_reindex_bloat_check('idx_test_bloat_val'::regclass);

-- 6. Check bloat report
SELECT count(*) > 0 AS has_bloat_report FROM pg_auto_reindex_bloat_report();

-- 7. Preflight check
SELECT safe FROM pg_auto_reindex_preflight_check('idx_test_bloat_val'::regclass);

-- 8. Lifecycle functions
SELECT pg_auto_reindex_record_start('idx_test_bloat_val'::regclass);
SELECT current_reindexing_index::regclass FROM pg_auto_reindex_status();
SELECT pg_auto_reindex_record_finish('idx_test_bloat_val'::regclass, true, 8192000, 4096000, NULL);
SELECT total_reindexed_count, total_bytes_saved FROM pg_auto_reindex_status();

-- 9. Check audit history table structure
SELECT indexname, bytes_saved, status FROM pg_auto_reindex_history WHERE indexname = 'idx_test_bloat_val';

-- 10. Cleanup
SELECT pg_auto_reindex_cleanup_invalid_indexes();

-- Clean up
DROP TABLE test_bloat_table;
DROP EXTENSION pg_auto_reindex;
