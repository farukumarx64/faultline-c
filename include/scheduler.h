#ifndef FAULTLINE_SCHEDULER_H
#define FAULTLINE_SCHEDULER_H

#include "job_queue.h"
#include "protocol.h"

/* Includes pending, active, and retained terminal jobs; no eviction yet. */
#define FAULTLINE_JOB_STORE_CAPACITY 256u

struct faultline_scheduler {
    struct faultline_job jobs[FAULTLINE_JOB_STORE_CAPACITY];
    size_t count;
    uint64_t next_job_id;
    struct faultline_job_queue pending;
};

enum faultline_scheduler_result {
    FAULTLINE_SCHEDULER_OK = 0,
    FAULTLINE_SCHEDULER_INVALID_ARGUMENT,
    FAULTLINE_SCHEDULER_FULL,
    FAULTLINE_SCHEDULER_ID_EXHAUSTED,
    FAULTLINE_SCHEDULER_EMPTY,
    FAULTLINE_SCHEDULER_WORKER_BUSY,
    FAULTLINE_SCHEDULER_INVALID_JOB,
    FAULTLINE_SCHEDULER_QUEUE_ERROR
};

/* Single event-loop owner. No clocks, sockets, allocation, or persistence here. */
void faultline_scheduler_init(struct faultline_scheduler *scheduler);
const struct faultline_job *faultline_scheduler_find(
    const struct faultline_scheduler *scheduler, uint64_t job_id);
const struct faultline_job *faultline_scheduler_active(
    const struct faultline_scheduler *scheduler, uint32_t worker_id);

/* Atomically own/queue a job and issue a fresh ID. The caller sends its ACK afterward. */
enum faultline_scheduler_result faultline_scheduler_submit(
    struct faultline_scheduler *scheduler, const struct faultline_job_submit_payload *submit,
    int64_t now_ms, uint64_t *job_id);

/*
 * Reserve the oldest pending job and produce its assignment. The caller must
 * first verify the registered worker is alive, unexpired, and ready for output.
 * This API checks that it has no active job. Reservation happens before sending.
 */
enum faultline_scheduler_result faultline_scheduler_assign(
    struct faultline_scheduler *scheduler, uint32_t worker_id, int64_t now_ms,
    struct faultline_message *assignment);

/* The caller supplies the sending connection's registered worker ID. */
enum faultline_scheduler_result faultline_scheduler_report(
    struct faultline_scheduler *scheduler, uint32_t worker_id,
    const struct faultline_message *report, int64_t now_ms);

/* Fail/requeue an active assignment after transport loss; EMPTY means no active job. */
enum faultline_scheduler_result faultline_scheduler_worker_lost(
    struct faultline_scheduler *scheduler, uint32_t worker_id, int64_t now_ms);

/*
 * Initialize once; fields and returned records are read-only outside scheduler
 * operations and the WAL replay module, which builds an isolated recovery store.
 * Required pointers must reference valid, non-overlapping storage. Rejected
 * operations preserve the scheduler and outputs. now_ms is monotonic and >= 0.
 * Retained records keep their addresses until reinitialization. IDs never wrap.
 */
#endif
