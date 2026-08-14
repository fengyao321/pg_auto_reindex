#ifndef PG_AUTO_REINDEX_H
#define PG_AUTO_REINDEX_H

#include "postgres.h"
#include "fmgr.h"
#include "storage/lwlock.h"
#include "utils/timestamp.h"

typedef struct AutoReindexSharedState
{
	LWLock	   *lock;
	Oid			current_reindexing_index;  /* Index OID currently being reindexed */
	TimestampTz last_reindex_time;		   /* Last completed reindex timestamp */
	uint64		total_reindexed_count;	   /* Total count of reindexed indexes */
	uint64		total_bytes_saved;		   /* Total bytes reclaimed */
} AutoReindexSharedState;

extern AutoReindexSharedState *AutoReindexShared;

/* GUC Variables - only lock_timeout and max_xact_duration remain in C */
extern int guc_lock_timeout_ms;
extern int guc_max_xact_duration;

#endif
