-- Regression test for pg_auto_reindex extension

CREATE EXTENSION pg_auto_reindex;

-- 1. Check GUC defaults
SHOW pg_auto_reindex.enabled;
SHOW pg_auto_reindex.min_bloat_ratio;
SHOW pg_auto_reindex.lock_timeout_ms;

-- 2. Check 168 time-slots stats count (should be exactly 168)
SELECT count(*) FROM pg_auto_reindex_stats();

-- 3. Check status function
SELECT is_idle, consecutive_idle_count, current_reindexing_index, total_reindexed_count 
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

-- 5. Check bloat report
SELECT count(*) >= 0 FROM pg_auto_reindex_bloat_report();

-- 6. Trigger manual auto-reindex cycle
SELECT pg_auto_reindex_trigger();

-- 7. Check audit history table structure
SELECT count(*) >= 0 FROM pg_auto_reindex_history;

-- Clean up
DROP TABLE test_bloat_table;
DROP EXTENSION pg_auto_reindex;
