import psutil
import os
import psycopg2
from dataclasses import dataclass
from typing import Optional

@dataclass
class SystemMetrics:
    cpu_usage_pct: float
    active_backends: int
    replication_lag_bytes: int
    disk_io_stats: dict

class ResourceSampler:
    def __init__(self, config):
        self.config = config

    def get_cgroup_cpu_usage(self) -> float:
        try:
            if os.path.exists('/sys/fs/cgroup/cpu.stat'):
                return psutil.cpu_percent()
            elif os.path.exists('/sys/fs/cgroup/cpu/cpuacct.usage'):
                return psutil.cpu_percent()
        except Exception:
            pass
        return psutil.cpu_percent()

    def get_active_backends(self, conn) -> int:
        try:
            with conn.cursor() as cur:
                cur.execute("SELECT count(*) FROM pg_stat_activity WHERE state = 'active' AND pid != pg_backend_pid()")
                res = cur.fetchone()
                return res[0] if res else 0
        except Exception:
            return 0

    def get_replication_lag(self, conn) -> int:
        try:
            with conn.cursor() as cur:
                cur.execute("""
                    SELECT max(pg_wal_lsn_diff(pg_current_wal_lsn(), replay_lsn))
                    FROM pg_stat_replication
                """)
                res = cur.fetchone()
                return res[0] if res and res[0] else 0
        except Exception:
            return 0

    def get_disk_io_stats(self) -> dict:
        io_counters = psutil.disk_io_counters()
        if io_counters:
            return {
                'read_bytes': io_counters.read_bytes,
                'write_bytes': io_counters.write_bytes
            }
        return {'read_bytes': 0, 'write_bytes': 0}

    def collect(self, conn) -> SystemMetrics:
        return SystemMetrics(
            cpu_usage_pct=self.get_cgroup_cpu_usage(),
            active_backends=self.get_active_backends(conn),
            replication_lag_bytes=self.get_replication_lag(conn),
            disk_io_stats=self.get_disk_io_stats()
        )
