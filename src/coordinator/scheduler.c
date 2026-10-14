#include "scheduler.h"

#include <string.h>

/* An active job always leaves room for its retry in the pending queue. */
_Static_assert(FAULTLINE_JOB_STORE_CAPACITY <= FAULTLINE_JOB_QUEUE_CAPACITY,
               "job store must leave space for every active job to requeue");

void faultline_scheduler_init(struct faultline_scheduler *scheduler)
{
    *scheduler = (struct faultline_scheduler){.next_job_id = 1};
    faultline_job_queue_init(&scheduler->pending);
}

static size_t find_slot(const struct faultline_scheduler *scheduler, uint64_t job_id)
{
    for (size_t i = 0; i < scheduler->count; ++i) {
        if (scheduler->jobs[i].id == job_id) {
            return i;
        }
    }
    return scheduler->count;
}

const struct faultline_job *faultline_scheduler_find(
    const struct faultline_scheduler *scheduler, uint64_t job_id)
{
    if (scheduler == NULL || job_id == 0) {
        return NULL;
    }
    size_t slot = find_slot(scheduler, job_id);
    return slot == scheduler->count ? NULL : &scheduler->jobs[slot];
}

const struct faultline_job *faultline_scheduler_active(
    const struct faultline_scheduler *scheduler, uint32_t worker_id)
{
    if (scheduler == NULL || worker_id == 0) {
        return NULL;
    }
    for (size_t i = 0; i < scheduler->count; ++i) {
        const struct faultline_job *job = &scheduler->jobs[i];
        if (job->worker_id == worker_id &&
            (job->state == FAULTLINE_JOB_ASSIGNED || job->state == FAULTLINE_JOB_RUNNING)) {
            return job;
        }
    }
    return NULL;
}

enum faultline_scheduler_result faultline_scheduler_submit(
    struct faultline_scheduler *scheduler, const struct faultline_job_submit_payload *submit,
    int64_t now_ms, uint64_t *job_id)
{
    struct faultline_job job;
    if (scheduler == NULL || submit == NULL || job_id == NULL || now_ms < 0) {
        return FAULTLINE_SCHEDULER_INVALID_ARGUMENT;
    }
    if (scheduler->count == FAULTLINE_JOB_STORE_CAPACITY) {
        return FAULTLINE_SCHEDULER_FULL;
    }
    if (scheduler->next_job_id == 0) {
        return FAULTLINE_SCHEDULER_ID_EXHAUSTED;
    }
    if (faultline_job_init(&job, scheduler->next_job_id,
                           (enum faultline_task_type)submit->task_type,
                           submit->arguments, submit->argument_size, submit->max_retries,
                           now_ms) != FAULTLINE_JOB_OK) {
        return FAULTLINE_SCHEDULER_INVALID_JOB;
    }
    memcpy(job.request_key, submit->request_key, sizeof(job.request_key));
    if (faultline_job_queue_push(&scheduler->pending, &job) != FAULTLINE_JOB_QUEUE_OK) {
        return FAULTLINE_SCHEDULER_QUEUE_ERROR;
    }
    /* No failing operations after enqueue: acceptance publishes the whole record. */
    scheduler->jobs[scheduler->count++] = job;
    scheduler->next_job_id = job.id == UINT64_MAX ? 0 : job.id + UINT64_C(1);
    *job_id = job.id;
    return FAULTLINE_SCHEDULER_OK;
}

enum faultline_scheduler_result faultline_scheduler_assign(
    struct faultline_scheduler *scheduler, uint32_t worker_id, int64_t now_ms,
    struct faultline_message *assignment)
{
    uint64_t id, removed;
    if (scheduler == NULL || assignment == NULL || worker_id == 0 || now_ms < 0) {
        return FAULTLINE_SCHEDULER_INVALID_ARGUMENT;
    }
    if (faultline_scheduler_active(scheduler, worker_id) != NULL) {
        return FAULTLINE_SCHEDULER_WORKER_BUSY;
    }
    if (faultline_job_queue_peek(&scheduler->pending, &id) != FAULTLINE_JOB_QUEUE_OK) {
        return FAULTLINE_SCHEDULER_EMPTY;
    }
    size_t slot = find_slot(scheduler, id);
    if (slot == scheduler->count) {
        return FAULTLINE_SCHEDULER_INVALID_JOB;
    }
    struct faultline_job candidate = scheduler->jobs[slot];
    if (faultline_job_assign(&candidate, worker_id, now_ms) != FAULTLINE_JOB_OK) {
        return FAULTLINE_SCHEDULER_INVALID_JOB;
    }
    struct faultline_message message = {
        .message_type = FAULTLINE_MSG_JOB_ASSIGN,
        .payload.job_assign = {
            .identity = {candidate.id, worker_id, candidate.attempt},
            .task_type = (uint16_t)candidate.task_type,
            .argument_size = candidate.argument_size
        }
    };
    memcpy(message.payload.job_assign.arguments, candidate.arguments, candidate.argument_size);
    /* No concurrent queue mutation: pop removes the ID we just peeked. */
    if (faultline_job_queue_pop(&scheduler->pending, &removed) != FAULTLINE_JOB_QUEUE_OK) {
        return FAULTLINE_SCHEDULER_QUEUE_ERROR;
    }
    scheduler->jobs[slot] = candidate;
    *assignment = message;
    return FAULTLINE_SCHEDULER_OK;
}

static enum faultline_scheduler_result publish_outcome(
    struct faultline_scheduler *scheduler, size_t slot, const struct faultline_job *candidate)
{
    if (candidate->state == FAULTLINE_JOB_QUEUED &&
        faultline_job_queue_push(&scheduler->pending, candidate) != FAULTLINE_JOB_QUEUE_OK) {
        return FAULTLINE_SCHEDULER_QUEUE_ERROR;
    }
    scheduler->jobs[slot] = *candidate;
    return FAULTLINE_SCHEDULER_OK;
}

enum faultline_scheduler_result faultline_scheduler_report(
    struct faultline_scheduler *scheduler, uint32_t worker_id,
    const struct faultline_message *report, int64_t now_ms)
{
    const struct faultline_job_identity *identity;
    enum faultline_job_result result;
    if (scheduler == NULL || report == NULL || worker_id == 0 || now_ms < 0) {
        return FAULTLINE_SCHEDULER_INVALID_ARGUMENT;
    }
    switch (report->message_type) {
    case FAULTLINE_MSG_JOB_STARTED: identity = &report->payload.job_started; break;
    case FAULTLINE_MSG_JOB_COMPLETED: identity = &report->payload.job_completed.identity; break;
    case FAULTLINE_MSG_JOB_FAILED: identity = &report->payload.job_failed.identity; break;
    default: return FAULTLINE_SCHEDULER_INVALID_ARGUMENT;
    }
    size_t slot = find_slot(scheduler, identity->job_id);
    if (identity->worker_id != worker_id || slot == scheduler->count) {
        return FAULTLINE_SCHEDULER_INVALID_JOB;
    }
    struct faultline_job candidate = scheduler->jobs[slot];
    if (report->message_type == FAULTLINE_MSG_JOB_STARTED) {
        result = faultline_job_start(&candidate, worker_id, identity->attempt, now_ms);
    } else if (report->message_type == FAULTLINE_MSG_JOB_COMPLETED) {
        result = faultline_job_complete(&candidate, worker_id, identity->attempt,
                    report->payload.job_completed.result, report->payload.job_completed.result_size, now_ms);
    } else {
        if (report->payload.job_failed.failure != FAULTLINE_JOB_FAILURE_TASK) {
            return FAULTLINE_SCHEDULER_INVALID_JOB;
        }
        result = faultline_job_fail(&candidate, worker_id, identity->attempt,
                                    FAULTLINE_JOB_FAILURE_TASK, now_ms);
    }
    if (result != FAULTLINE_JOB_OK) {
        return FAULTLINE_SCHEDULER_INVALID_JOB;
    }
    return publish_outcome(scheduler, slot, &candidate);
}

enum faultline_scheduler_result faultline_scheduler_worker_lost(
    struct faultline_scheduler *scheduler, uint32_t worker_id, int64_t now_ms)
{
    if (scheduler == NULL || worker_id == 0 || now_ms < 0) {
        return FAULTLINE_SCHEDULER_INVALID_ARGUMENT;
    }
    const struct faultline_job *active = faultline_scheduler_active(scheduler, worker_id);
    if (active == NULL) {
        return FAULTLINE_SCHEDULER_EMPTY;
    }
    struct faultline_job candidate = *active;
    if (faultline_job_fail(&candidate, worker_id, candidate.attempt,
                           FAULTLINE_JOB_FAILURE_WORKER_LOST, now_ms) != FAULTLINE_JOB_OK) {
        return FAULTLINE_SCHEDULER_INVALID_JOB;
    }
    return publish_outcome(scheduler, find_slot(scheduler, candidate.id), &candidate);
}
