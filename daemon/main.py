import argparse
import time
import logging
import signal
import sys
import psycopg2

from config import load_config
from sampler import ResourceSampler
from learner import EWMALearner
from scheduler import ReindexScheduler

logger = logging.getLogger(__name__)
shutdown_requested = False

def handle_sigterm(signum, frame):
    global shutdown_requested
    logger.info("Graceful shutdown requested...")
    shutdown_requested = True

def get_connection(config_pg):
    return psycopg2.connect(
        host=config_pg.host,
        port=config_pg.port,
        user=config_pg.user,
        password=config_pg.password,
        dbname='postgres',
        sslmode=config_pg.sslmode
    )

def main():
    parser = argparse.ArgumentParser(description='pg_auto_reindex Daemon')
    parser.add_argument('--config', type=str, default='pg_auto_reindex_daemon.yaml', help='Path to config file')
    parser.add_argument('--log-level', type=str, help='Override log level')
    parser.add_argument('--dry-run', action='store_true', help='Dry run without executing reindex')
    args = parser.parse_args()

    config = load_config(args.config)
    log_level = args.log_level or config.log_level
    logging.basicConfig(level=getattr(logging, log_level.upper(), logging.INFO), 
                        format='%(asctime)s - %(name)s - %(levelname)s - %(message)s')

    signal.signal(signal.SIGTERM, handle_sigterm)
    signal.signal(signal.SIGINT, handle_sigterm)

    sampler = ResourceSampler(config)
    learner = EWMALearner(config.idle)
    scheduler = ReindexScheduler(config)

    learner.load(config.learning_stats_file)
    
    conn = None
    while not shutdown_requested:
        try:
            if not conn or conn.closed:
                conn = get_connection(config.postgres)
            
            metrics = sampler.collect(conn)
            slot = learner.get_slot_index()
            learner.update(slot, metrics)

            if not scheduler.circuit_breaker_check(metrics):
                logger.debug("Circuit breaker triggered")
            elif not learner.is_idle(slot, metrics):
                logger.debug("System not idle")
            else:
                dbs = scheduler.discover_databases(conn)
                for db in dbs:
                    if not config.postgres.all_databases and db not in config.postgres.databases:
                        continue
                    
                    try:
                        db_conn = psycopg2.connect(
                            host=config.postgres.host,
                            port=config.postgres.port,
                            user=config.postgres.user,
                            password=config.postgres.password,
                            dbname=db,
                            sslmode=config.postgres.sslmode
                        )
                        candidates = scheduler.scan_bloated_indexes(db_conn, db)
                        reindexed_count = 0
                        for row in candidates:
                            if reindexed_count >= config.scheduler.max_reindexes_per_idle:
                                break
                            
                            idx_oid, schema_name, idx_name, b_bytes, b_ratio = row
                            
                            if b_bytes < config.scheduler.min_bloat_bytes or b_ratio < config.scheduler.min_bloat_ratio:
                                continue
                                
                            if scheduler.preflight_check(db_conn, idx_oid):
                                if scheduler.execute_reindex(db_conn, schema_name, idx_name, idx_oid, config.scheduler.lock_timeout_ms, args.dry_run):
                                    reindexed_count += 1
                        db_conn.close()
                    except Exception as e:
                        logger.error(f"Error processing db {db}: {e}")

            learner.save(config.learning_stats_file)

        except psycopg2.Error as e:
            logger.error(f"Database connection error: {e}")
            if conn and not conn.closed:
                conn.close()
        except Exception as e:
            logger.error(f"Unexpected error: {e}")

        naptime = config.scheduler.naptime_seconds
        start_sleep = time.time()
        while time.time() - start_sleep < naptime and not shutdown_requested:
            time.sleep(1)

    if conn and not conn.closed:
        conn.close()
    logger.info("Daemon exited gracefully.")

if __name__ == '__main__':
    main()
