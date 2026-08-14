import yaml
import os
from dataclasses import dataclass, field
from typing import Optional

@dataclass
class PostgresConfig:
    host: str = '127.0.0.1'
    port: int = 5432
    user: str = 'postgres'
    password: str = ''
    sslmode: str = 'disable'
    all_databases: bool = True
    databases: list = field(default_factory=list)

@dataclass
class SchedulerConfig:
    naptime_seconds: int = 60
    max_reindexes_per_idle: int = 2
    min_bloat_bytes: int = 67108864  # 64MB
    min_bloat_ratio: float = 0.30
    lock_timeout_ms: int = 5000
    consecutive_idle_required: int = 3

@dataclass
class IdleConfig:
    cgroup_cpu_max_pct: float = 40.0
    max_active_backends: int = 15
    max_replication_lag_bytes: int = 67108864  # 64MB
    ewma_alpha: float = 0.20
    idle_ratio_threshold: float = 0.70

@dataclass
class DaemonConfig:
    postgres: PostgresConfig = field(default_factory=PostgresConfig)
    scheduler: SchedulerConfig = field(default_factory=SchedulerConfig)
    idle: IdleConfig = field(default_factory=IdleConfig)
    log_level: str = 'INFO'
    learning_stats_file: str = '~/.pg_auto_reindex_learning.json'

def load_config(path: str) -> DaemonConfig:
    if not os.path.exists(path):
        return DaemonConfig()
    with open(path, 'r') as f:
        data = yaml.safe_load(f) or {}
    
    pg_data = data.get('postgres', {})
    sched_data = data.get('scheduler', {})
    idle_data = data.get('idle', {})
    
    postgres = PostgresConfig(**pg_data)
    scheduler = SchedulerConfig(**sched_data)
    idle = IdleConfig(**idle_data)
    
    return DaemonConfig(
        postgres=postgres,
        scheduler=scheduler,
        idle=idle,
        log_level=data.get('log_level', 'INFO'),
        learning_stats_file=data.get('learning_stats_file', '~/.pg_auto_reindex_learning.json')
    )
