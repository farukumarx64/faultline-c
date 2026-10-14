#ifndef FAULTLINE_JOB_H
#define FAULTLINE_JOB_H

#include <stddef.h>
#include <stdint.h>
#include "request_key.h"

/* Model byte limits are also enforced by the job message codec. */
#define FAULTLINE_JOB_MAX_ARGUMENT_SIZE 1024u
#define FAULTLINE_JOB_MAX_RESULT_SIZE 1024u
#define FAULTLINE_JOB_TIME_UNSET INT64_C(-1)

enum faultline_task_type {
    FAULTLINE_TASK_SLEEP = 1,
    FAULTLINE_TASK_PRIME_COUNT = 2,
    FAULTLINE_TASK_FIBONACCI = 3,
    FAULTLINE_TASK_HASH = 4
};

enum faultline_job_state {
    FAULTLINE_JOB_QUEUED = 1,
    FAULTLINE_JOB_ASSIGNED,
    FAULTLINE_JOB_RUNNING,
    FAULTLINE_JOB_DONE,
    FAULTLINE_JOB_FAILED
};

enum faultline_job_failure {
    FAULTLINE_JOB_FAILURE_NONE = 0,
    FAULTLINE_JOB_FAILURE_TASK = 1,
    FAULTLINE_JOB_FAILURE_WORKER_LOST = 2
};

/*
 * Coordinator-owned host representation; never send or persist this struct raw.
 * Initialize before use and treat fields as read-only outside this module.
 * Admission/replay sets request_key once before publishing a fresh job;
 * all later transitions preserve it.
 * The caller provides a unique nonzero job ID; this module owns no ID registry.
 */
struct faultline_job {
    uint64_t id;
    uint8_t request_key[FAULTLINE_REQUEST_KEY_SIZE]; /* Immutable admission metadata. */
    enum faultline_task_type task_type;
    uint8_t arguments[FAULTLINE_JOB_MAX_ARGUMENT_SIZE];
    size_t argument_size;
    enum faultline_job_state state;
    uint32_t worker_id;  /* Zero while queued; terminal states retain the last owner. */
    uint64_t attempt;    /* Zero before assignment, incremented on every assignment. */
    uint32_t retry_count;
    uint32_t max_retries; /* Additional attempts allowed after the initial attempt. */
    int64_t created_at_ms;
    int64_t updated_at_ms;
    int64_t assigned_at_ms;
    int64_t started_at_ms;
    int64_t finished_at_ms;
    uint8_t result[FAULTLINE_JOB_MAX_RESULT_SIZE];
    size_t result_size;
    enum faultline_job_failure failure;
};

enum faultline_job_result {
    FAULTLINE_JOB_OK = 0,
    FAULTLINE_JOB_INVALID_ARGUMENT,
    FAULTLINE_JOB_PAYLOAD_TOO_LARGE,
    FAULTLINE_JOB_INVALID_TRANSITION,
    FAULTLINE_JOB_TIME_REVERSED,
    FAULTLINE_JOB_WRONG_WORKER,
    FAULTLINE_JOB_STALE_ATTEMPT
};

/* State-graph check only; operation APIs also enforce identity, time, and retry rules. */
int faultline_job_can_transition(enum faultline_job_state from,
                                enum faultline_job_state to);

/*
 * Initialize fresh storage as QUEUED; now_ms is coordinator monotonic time >= 0.
 * Copies opaque argument bytes; task-specific interpretation belongs to worker executors.
 * NULL data is allowed only for size zero. Source bytes must not overlap *job.
 * max_retries=0 permits one attempt. Every failing API call leaves *job unchanged.
 */
enum faultline_job_result faultline_job_init(
    struct faultline_job *job, uint64_t id, enum faultline_task_type task_type,
    const uint8_t *arguments, size_t argument_size, uint32_t max_retries,
    int64_t now_ms);

/* QUEUED -> ASSIGNED; requires a nonzero worker ID. Read job->attempt after success. */
enum faultline_job_result faultline_job_assign(
    struct faultline_job *job, uint32_t worker_id, int64_t now_ms);

/* ASSIGNED -> RUNNING. Reports must match both current worker and attempt. */
enum faultline_job_result faultline_job_start(
    struct faultline_job *job, uint32_t worker_id, uint64_t attempt, int64_t now_ms);

/* RUNNING -> DONE; copies result bytes, with the same size/pointer rules as init. */
enum faultline_job_result faultline_job_complete(
    struct faultline_job *job, uint32_t worker_id, uint64_t attempt,
    const uint8_t *result, size_t result_size, int64_t now_ms);

/*
 * ASSIGNED/RUNNING -> QUEUED if retry_count < max_retries, otherwise -> FAILED.
 * TASK includes failures before execution starts; WORKER_LOST is coordinator-driven.
 * Requeue increments retry_count, clears owner and attempt timestamps, and retains
 * the attempt number/failure reason. The next assignment clears the failure reason.
 * Only changes this record: no queue insertion, worker lookup, I/O, or automatic retry.
 */
enum faultline_job_result faultline_job_fail(
    struct faultline_job *job, uint32_t worker_id, uint64_t attempt,
    enum faultline_job_failure failure, int64_t now_ms);

#endif
