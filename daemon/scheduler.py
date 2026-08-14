import psycopg2
import logging

logger = logging.getLogger(__name__)

class ReindexScheduler:
    def __init__(self, config):
        self.config = config

    def discover_databases(self, conn):
        try:
            with conn.cursor() as cur:
                cur.execute("SELECT datname FROM pg_database WHERE datistemplate = false AND datallowconn = true;")
                return [row[0] for row in cur.fetchall()]
        except Exception as e:
            logger.error(f"Error discovering databases: {e}")
            return []

    def scan_bloated_indexes(self, conn, db):
        try:
            with conn.cursor() as cur:
                cur.execute("SELECT index_oid, schema_name, index_name, bloat_bytes, bloat_ratio FROM pg_auto_reindex_bloat_report()")
                return cur.fetchall()
        except Exception as e:
            logger.error(f"Error scanning bloated indexes on {db}: {e}")
            return []

    def preflight_check(self, conn, index_oid):
        try:
            with conn.cursor() as cur:
                cur.execute("SELECT pg_auto_reindex_preflight_check(%s::regclass)", (index_oid,))
                res = cur.fetchone()
                return res[0] if res else False
        except Exception as e:
            logger.error(f"Preflight check failed for {index_oid}: {e}")
            return False

    def execute_reindex(self, conn, schema, index, index_oid, lock_timeout_ms, dry_run=False):
        if dry_run:
            logger.info(f"[DRY-RUN] Would reindex {schema}.{index}")
            return True

        old_autocommit = conn.autocommit
        try:
            conn.autocommit = True
            with conn.cursor() as cur:
                cur.execute("SELECT pg_auto_reindex_record_start(%s)", (index_oid,))
                cur.execute(f"SET lock_timeout = {lock_timeout_ms}")
                
                logger.info(f"Starting REINDEX CONCURRENTLY {schema}.{index}")
                cur.execute(f'REINDEX INDEX CONCURRENTLY "{schema}"."{index}"')
                
                cur.execute("SELECT pg_auto_reindex_record_finish(%s, true, 0, 0, NULL)", (index_oid,))
                return True
        except Exception as e:
            logger.error(f"Error reindexing {schema}.{index}: {e}")
            try:
                with conn.cursor() as cur:
                    cur.execute("SELECT pg_auto_reindex_record_finish(%s, false, 0, 0, %s)", (index_oid, str(e)))
            except:
                pass
            return False
        finally:
            conn.autocommit = old_autocommit

    def circuit_breaker_check(self, metrics):
        if metrics.cpu_usage_pct > self.config.idle.cgroup_cpu_max_pct:
            return False
        if metrics.replication_lag_bytes > self.config.idle.max_replication_lag_bytes:
            return False
        return True
