# pg_auto_reindex

A PostgreSQL extension that autonomously learns system resource usage patterns and automatically performs `REINDEX CONCURRENTLY` on bloated indexes during detected idle periods — completely transparent to applications.

## Features

- **Idle Learning Engine** — 168 time-slot (7 days × 24 hours) EWMA model that learns your system's resource usage patterns
- **Metadata-based Bloat Estimation** — Fast B-Tree index bloat detection using `pg_statistic`, no page-level scanning required
- **Safe Concurrent Reindexing** — Uses `REINDEX CONCURRENTLY` with configurable lock timeout protection
- **Self-Healing** — Automatically cleans up invalid indexes left by interrupted operations
- **Full Audit Trail** — Every reindex operation is logged to `pg_auto_reindex_history`
- **Observable** — SQL functions to inspect EWMA stats, bloat reports, and worker status

## Quick Start

```bash
# Build and install
make USE_PGXS=1 PG_CONFIG=/path/to/pg_config install

# In postgresql.conf
shared_preload_libraries = 'pg_auto_reindex'
pg_auto_reindex.database = 'your_database'
pg_auto_reindex.enabled = on

# Restart PostgreSQL, then:
CREATE EXTENSION pg_auto_reindex;
```

## SQL Functions

| Function | Description |
|----------|-------------|
| `pg_auto_reindex_stats()` | View 168 time-slot EWMA learning matrix |
| `pg_auto_reindex_bloat_report()` | Scan and report B-Tree index bloat |
| `pg_auto_reindex_status()` | Current background worker status |
| `pg_auto_reindex_trigger()` | Manually trigger a reindex evaluation cycle |

## GUC Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `pg_auto_reindex.enabled` | `on` | Enable/disable the worker |
| `pg_auto_reindex.database` | `postgres` | Target database |
| `pg_auto_reindex.naptime` | `60` | Sampling interval (seconds) |
| `pg_auto_reindex.min_bloat_ratio` | `0.30` | Minimum bloat ratio to trigger reindex |
| `pg_auto_reindex.max_idle_load` | `2.0` | Maximum CPU load to consider system idle |
| `pg_auto_reindex.max_idle_backends` | `15` | Maximum active backends to consider idle |
| `pg_auto_reindex.lock_timeout_ms` | `5000` | Lock timeout for REINDEX (ms) |
| `pg_auto_reindex.max_reindexes_per_idle` | `2` | Max indexes to reindex per idle window |

## Documentation

- [Technical Design (English)](doc/design_en.md)
- [技术设计文档 (中文)](doc/design_zh.md)
- [Test Design (English)](doc/test_design_en.md)
- [测试设计文档 (中文)](doc/test_design_zh.md)

## Testing

```bash
# Regression test
make USE_PGXS=1 PG_CONFIG=/path/to/pg_config installcheck

# Production simulation (long-running)
bash test/production_simulation.sh --duration 30
```

## License

PostgreSQL License
