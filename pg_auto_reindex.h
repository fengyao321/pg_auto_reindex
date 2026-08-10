/*-------------------------------------------------------------------------
 *
 * pg_auto_reindex.h
 *		Autonomous idle learning & background concurrent reindexing.
 *
 * Copyright (c) 2026, fengyao <fengyao0087@gmail.com>
 *
 * IDENTIFICATION
 *	  contrib/pg_auto_reindex/pg_auto_reindex.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_AUTO_REINDEX_H
#define PG_AUTO_REINDEX_H

#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "utils/timestamp.h"

#define NUM_TIME_SLOTS (7 * 24) /* 168 time slots (7 days * 24 hours) */
#define EWMA_ALPHA     0.20     /* Exponential weight factor */

typedef struct SlotStats
{
    double   ewma_active_backends;
    double   ewma_loadavg;
    double   ewma_wal_bytes_per_sec;
    uint64   sample_count;
} SlotStats;

typedef struct AutoReindexSharedState
{
    LWLock     *lock;                  /* Shared memory LWLock */
    bool        is_idle;               /* Current system idle status */
    int         consecutive_idle_count;/* Consecutive idle sampling counter */
    
    SlotStats   slots[NUM_TIME_SLOTS]; /* 168 time-slots EWMA matrix */
    
    /* Global status metrics */
    Oid         current_reindexing_index; /* Index OID currently being reindexed */
    TimestampTz last_reindex_time;        /* Last completed reindex timestamp */
    uint64      total_reindexed_count;    /* Total count of reindexed indexes */
    uint64      total_bytes_saved;        /* Total bytes reclaimed */
} AutoReindexSharedState;

/* External shared state reference */
extern AutoReindexSharedState *AutoReindexShared;

/* GUC Variables */
extern bool  guc_enabled;
extern char *guc_database;
extern int   guc_naptime;
extern double guc_idle_ratio_threshold;
extern double guc_max_idle_load;
extern int   guc_max_idle_backends;
extern double guc_min_bloat_ratio;
extern int64 guc_min_bloat_bytes;
extern int   guc_lock_timeout_ms;
extern int   guc_max_reindexes_per_idle;

/* Module Function Prototypes */
extern void CollectSystemMetricsAndLearnerUpdate(void);
extern bool IsSystemIdle(void);
extern void ExecuteAutoReindexCycle(void);
extern void CleanupInvalidIndexes(void);

#endif /* PG_AUTO_REINDEX_H */
