# pg_auto_reindex v2.0

A PostgreSQL extension + external daemon that autonomously learns system resource usage patterns and automatically performs `REINDEX CONCURRENTLY` on bloated B-Tree indexes during detected idle periods — completely transparent to applications.

## Architecture

`pg_auto_reindex` v2.0 follows the PostgreSQL community's **"Mechanism in Kernel, Policy Outside"** principle:

- **C Extension** (`src/`): Provides precise B-Tree bloat estimation, pre-flight safety checks, shared memory state tracking, and audit history recording. **No background worker** — zero risk of crashing the PostgreSQL main process.
- **External Daemon** (`daemon/`): Python-based control plane handling EWMA idle learning, cgroups-aware resource sampling, multi-database scheduling, circuit breaker protection, and safe `REINDEX CONCURRENTLY` execution via libpq autocommit.

```text
┌───────────────────────────────────────────────────────┐
│         External Control Plane (Python Daemon)        │
│  EWMA Learning │ cgroups Sampling │ Circuit Breaker   │
└───────────────────────┬───────────────────────────────┘
                        │ SQL / libpq (autocommit)
                        ▼
┌───────────────────────────────────────────────────────┐
│          PostgreSQL C Extension (Mechanism Layer)      │
│  Bloat Estimator │ Preflight Checks │ Audit Catalog   │
└───────────────────────────────────────────────────────┘
```

## Features

- **Precision B-Tree Bloat Estimation** — Physical page layout calculations (PageHeader, BTPageOpaque, ItemId, fillfactor, MAXALIGN) with expression index and stale statistics handling
- **Pre-flight Safety Checks** — Long transaction detection, idle-in-transaction guard, concurrent reindex collision prevention
- **cgroups-Aware Idle Detection** — Container CPU/memory sampling (cgroups v1/v2), not just OS `getloadavg()`
- **168h EWMA Learning** — 7×24 time-slot model with persistent state (survives restarts and failover)
- **Circuit Breaker** — Automatic halt when CPU or replication lag exceeds thresholds
- **Multi-Database Support** — Single daemon manages all databases in a PostgreSQL instance
- **Full Audit Trail** — Every reindex operation logged to `pg_auto_reindex_history`
- **Self-Healing** — Automatic cleanup of invalid `_ccnew` indexes from interrupted operations

## Quick Start

### 1. Install the C Extension

```bash
# Build and install
cd pg_auto_reindex
make USE_PGXS=1 PG_CONFIG=/path/to/pg_config install

# In postgresql.conf (only for shared memory initialization)
shared_preload_libraries = 'pg_auto_reindex'

# Restart PostgreSQL, then in each target database:
CREATE EXTENSION pg_auto_reindex;
```

### 2. Start the External Daemon

```bash
cd daemon
pip install -r requirements.txt

# Copy and edit configuration
cp pg_auto_reindex_daemon.yaml.example pg_auto_reindex_daemon.yaml

# Start the daemon
python main.py --config pg_auto_reindex_daemon.yaml

# Dry-run mode (log without executing)
python main.py --config pg_auto_reindex_daemon.yaml --dry-run
```

## SQL Functions

| Function | Description |
|----------|-------------|
| `pg_auto_reindex_bloat_check(index regclass)` | Precise B-Tree bloat estimation for a single index |
| `pg_auto_reindex_bloat_report()` | Scan all user B-Tree indexes and report bloat |
| `pg_auto_reindex_status()` | Current shared memory state and metrics |
| `pg_auto_reindex_preflight_check(index regclass)` | Safety check before reindexing (long transactions, locks) |
| `pg_auto_reindex_record_start(index regclass)` | Mark reindex start in shared memory |
| `pg_auto_reindex_record_finish(index, success, bytes_before, bytes_after, error_msg)` | Record completion + write audit |
| `pg_auto_reindex_cleanup_invalid_indexes()` | Clean up leftover invalid `_ccnew` indexes |

## GUC Parameters (C Extension)

| Parameter | Default | Description |
|-----------|---------|-------------|
| `pg_auto_reindex.lock_timeout_ms` | `5000` | Lock timeout for safety checks (ms) |
| `pg_auto_reindex.max_xact_duration` | `300` | Max transaction age to consider safe for reindex (seconds) |

## Daemon Configuration

See [`daemon/pg_auto_reindex_daemon.yaml.example`](daemon/pg_auto_reindex_daemon.yaml.example) for all options.

## Documentation

- [v2.0 Technical Design (中文)](doc/design_v2.0.md)
- [Architecture Redesign](doc/redesign_architecture.md)
- [Code Review Report](doc/pg_hackers_code_review.md)
- [v1.0 Technical Design (English)](doc/design_en.md)
- [v1.0 技术设计文档 (中文)](doc/design_zh.md)

## Project Structure

```text
pg_auto_reindex/
├── src/                        # C extension source
│   ├── pg_auto_reindex.h       # Header: shared state struct, GUC declarations
│   ├── pg_auto_reindex.c       # Module init: GUC registration, shmem setup
│   ├── bloat_estimator.c       # Precision B-Tree bloat estimation engine
│   ├── shmem_status.c          # Status, preflight checks, history recording
│   └── executor_safe.c         # Invalid index cleanup utility
├── daemon/                     # External control plane (Python)
│   ├── main.py                 # Daemon entry point
│   ├── config.py               # Configuration management
│   ├── sampler.py              # cgroups + DB resource sampling
│   ├── learner.py              # 168h EWMA learning engine
│   ├── scheduler.py            # Multi-DB scheduling + circuit breaker
│   └── requirements.txt        # Python dependencies
├── doc/                        # Documentation
├── pg_auto_reindex--2.0.sql    # Extension SQL (tables + functions)
├── pg_auto_reindex--1.0--2.0.sql # Upgrade path from v1.0
├── pg_auto_reindex.control     # Extension control file
├── Makefile                    # PGXS build
└── meson.build                 # Meson build
```

## Upgrading from v1.0

```sql
ALTER EXTENSION pg_auto_reindex UPDATE TO '2.0';
```

Then start the external daemon. The old background worker is no longer needed.

## License

PostgreSQL License
