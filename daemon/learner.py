import json
import os
from datetime import datetime
import psycopg2

class EWMALearner:
    def __init__(self, idle_config):
        self.alpha = idle_config.ewma_alpha
        self.idle_ratio_threshold = idle_config.idle_ratio_threshold
        self.slots = [{} for _ in range(168)]

    def get_slot_index(self):
        now = datetime.now()
        day = now.weekday()
        hour = now.hour
        return day * 24 + hour

    def update(self, slot_index, metrics):
        current_baseline = self.slots[slot_index]
        if not current_baseline:
            self.slots[slot_index] = {
                'cpu_usage_pct': metrics.cpu_usage_pct,
                'active_backends': metrics.active_backends,
                'replication_lag_bytes': metrics.replication_lag_bytes
            }
        else:
            for k in ['cpu_usage_pct', 'active_backends', 'replication_lag_bytes']:
                val = getattr(metrics, k)
                current_baseline[k] = self.alpha * val + (1 - self.alpha) * current_baseline.get(k, val)

    def is_idle(self, slot_index, metrics):
        baseline = self.slots[slot_index]
        if not baseline:
            return True
        
        if metrics.cpu_usage_pct > baseline.get('cpu_usage_pct', 100) * self.idle_ratio_threshold:
            return False
        if metrics.active_backends > baseline.get('active_backends', 100) * self.idle_ratio_threshold:
            return False
        
        return True

    def save(self, path):
        expanded_path = os.path.expanduser(path)
        try:
            with open(expanded_path, 'w') as f:
                json.dump(self.slots, f)
        except Exception:
            pass

    def load(self, path):
        expanded_path = os.path.expanduser(path)
        if os.path.exists(expanded_path):
            try:
                with open(expanded_path, 'r') as f:
                    data = json.load(f)
                    if len(data) == 168:
                        self.slots = data
            except Exception:
                pass
