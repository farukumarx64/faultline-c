#ifndef FAULTLINE_COORDINATOR_STATS_H
#define FAULTLINE_COORDINATOR_STATS_H

#include "coordinator_store.h"

/* Captured once after successful startup recovery, before serving requests.
 * Runtime totals are differences from this baseline, never replayed events. */
struct faultline_stats_session {
    int64_t ready_at_ms;
    uint64_t startup_duration_ms;
    uint64_t startup_interrupted_jobs;
    uint64_t submitted_at_start;
    uint64_t completed_at_start;
    uint64_t failed_at_start;
    uint64_t retries_at_start;
};

/* Read-only helpers: no WAL, clock calls, allocation, or liveness changes.
 * Return 0 on success, -1 for invalid input/time/state; outputs stay unchanged
 * on error. Required pointers must be valid and non-overlapping. */
int faultline_stats_begin(struct faultline_stats_session *session,
                          const struct faultline_coordinator_store *store, int64_t now_ms);
int faultline_stats_snapshot(const struct faultline_stats_session *session,
                             const struct faultline_coordinator_store *store,
                             int64_t now_ms, int heartbeat_timeout_ms,
                             struct faultline_stats_payload *output);

#endif
