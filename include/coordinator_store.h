#ifndef FAULTLINE_COORDINATOR_STORE_H
#define FAULTLINE_COORDINATOR_STORE_H

#include "wal_replay.h"

enum faultline_store_result {
    FAULTLINE_STORE_OK = 0,
    FAULTLINE_STORE_REJECTED,
    FAULTLINE_STORE_EMPTY,
    FAULTLINE_STORE_FATAL
};

enum faultline_store_failure {
    FAULTLINE_STORE_FAILURE_NONE = 0,
    FAULTLINE_STORE_FAILURE_OPEN,
    FAULTLINE_STORE_FAILURE_WAL,
    FAULTLINE_STORE_FAILURE_CLOCK,
    FAULTLINE_STORE_FAILURE_INTERNAL
};

/* Single event-loop owner; allocate on the heap. Treat fields as read-only,
 * except live heartbeat/death updates through the registry API. The scratch
 * scheduler prepares a complete transaction without changing published state. */
struct faultline_coordinator_store {
    struct faultline_scheduler jobs;
    struct faultline_worker_registry workers;
    struct faultline_scheduler scratch;
    struct faultline_wal_writer wal;
    struct faultline_wal_replay_report replay_report;
    enum faultline_wal_replay_result replay_result;
    enum faultline_wal_writer_result write_result;
    enum faultline_store_failure failure;
    int opened; /* Live operations enabled only after all startup recovery succeeds. */
    int64_t time_base_ms;
    int64_t session_start_ms;
    int64_t last_job_time_ms;
    size_t interrupted_jobs;
};

/* Initialize before each open; close a live handle before reinitializing. */
void faultline_store_init(struct faultline_coordinator_store *store);

/* Explicit create or existing-file recovery; never fall back to an empty log.
 * Reconcile recovered active jobs in ascending ID order before success. now_ms
 * is a fresh raw monotonic clock reading; job timestamps use a separate logical
 * timeline. Public mutations remain disabled throughout recovery. QUEUED and
 * terminal records are preserved; ASSIGNED/RUNNING consume a retry or fail.
 * On failure close the handle, even if opening did not finish. */
enum faultline_store_result faultline_store_open(
    struct faultline_coordinator_store *store, const char *path, int initialize, int64_t now_ms);
enum faultline_store_result faultline_store_close(struct faultline_coordinator_store *store);

/* Prepare -> append and sync -> publish. Outputs and live jobs/allocators are
 * unchanged on rejection/failure. Any fatal failure disables all later changes;
 * cleanup must only close connections, without logging further job transitions.
 * Callers may ACK, dispatch, or announce results only after OK. No result ACK is
 * introduced. Required pointers are valid and non-overlapping. */
enum faultline_store_result faultline_store_register(
    struct faultline_coordinator_store *store, int fd, int64_t now_ms, uint32_t *worker_id);
enum faultline_store_result faultline_store_submit(
    struct faultline_coordinator_store *store, const struct faultline_job_submit_payload *submit,
    int64_t now_ms, uint64_t *job_id);
enum faultline_store_result faultline_store_assign(
    struct faultline_coordinator_store *store, uint32_t worker_id, int64_t now_ms,
    struct faultline_message *assignment);
enum faultline_store_result faultline_store_report(
    struct faultline_coordinator_store *store, uint32_t worker_id,
    const struct faultline_message *report, int64_t now_ms);
enum faultline_store_result faultline_store_worker_lost(
    struct faultline_coordinator_store *store, uint32_t worker_id, int64_t now_ms);

#endif
