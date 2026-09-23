#include "coordinator_store_internal.h"
#include "wal_replay_internal.h"

#include <stdlib.h>
#include <string.h>

static enum faultline_store_result fail(struct faultline_coordinator_store *store,
                                       enum faultline_store_failure failure)
{
    if (store->failure == FAULTLINE_STORE_FAILURE_NONE) { store->failure = failure; }
    return FAULTLINE_STORE_FATAL;
}

static int usable(const struct faultline_coordinator_store *store)
{
    return store->opened && store->failure == FAULTLINE_STORE_FAILURE_NONE;
}

void faultline_store_init(struct faultline_coordinator_store *store)
{
    memset(store, 0, sizeof(*store));
    store->wal = (struct faultline_wal_writer)FAULTLINE_WAL_WRITER_INIT;
    faultline_scheduler_init(&store->jobs);
    faultline_worker_registry_init(&store->workers);
}

static int job_time(struct faultline_coordinator_store *store, int64_t raw, int64_t *time)
{
    if (raw < store->session_start_ms || raw < 0 ||
        raw - store->session_start_ms > INT64_MAX - store->time_base_ms) {
        (void)fail(store, FAULTLINE_STORE_FAILURE_CLOCK);
        return -1;
    }
    *time = store->time_base_ms + (raw - store->session_start_ms);
    if (*time < store->last_job_time_ms) {
        (void)fail(store, FAULTLINE_STORE_FAILURE_CLOCK);
        return -1;
    }
    return 0;
}

static enum faultline_store_result append(struct faultline_coordinator_store *store,
                                         struct faultline_wal_record *record)
{
    record->sequence = store->wal.next_sequence;
    store->write_result = faultline_wal_writer_append(&store->wal, record);
    if (store->write_result != FAULTLINE_WAL_WRITE_OK) {
        return fail(store, FAULTLINE_STORE_FAILURE_WAL);
    }
    return FAULTLINE_STORE_OK;
}

static enum faultline_store_result commit_job(struct faultline_coordinator_store *store,
                                             uint64_t id, enum faultline_wal_record_type type)
{
    const struct faultline_job *job = faultline_scheduler_find(&store->scratch, id);
    if (job == NULL) { return fail(store, FAULTLINE_STORE_FAILURE_INTERNAL); }
    struct faultline_wal_record record = {.type = type, .payload.job = *job};
    if (append(store, &record) != FAULTLINE_STORE_OK) { return FAULTLINE_STORE_FATAL; }
    /* All validation and queue operations already succeeded on scratch. This
     * copy cannot allocate or reject after the durable record exists. */
    store->jobs = store->scratch;
    store->last_job_time_ms = job->updated_at_ms;
    return FAULTLINE_STORE_OK;
}

enum faultline_store_result faultline_store_register(
    struct faultline_coordinator_store *store, int fd, int64_t now_ms, uint32_t *worker_id)
{
    if (store == NULL || worker_id == NULL) { return FAULTLINE_STORE_REJECTED; }
    if (!usable(store)) { return FAULTLINE_STORE_FATAL; }
    if (now_ms < 0) { return fail(store, FAULTLINE_STORE_FAILURE_CLOCK); }
    struct faultline_worker_registry candidate = store->workers;
    uint32_t id;
    if (faultline_worker_register(&candidate, fd, now_ms, &id) != FAULTLINE_REGISTRY_OK) {
        return FAULTLINE_STORE_REJECTED;
    }
    struct faultline_wal_record record = {.type = FAULTLINE_WAL_WORKER_ID_ALLOCATED, .payload.worker_id = id};
    if (append(store, &record) != FAULTLINE_STORE_OK) { return FAULTLINE_STORE_FATAL; }
    store->workers = candidate;
    *worker_id = id;
    return FAULTLINE_STORE_OK;
}

enum faultline_store_result faultline_store_submit(
    struct faultline_coordinator_store *store, const struct faultline_job_submit_payload *submit,
    int64_t now_ms, uint64_t *job_id)
{
    if (store == NULL || submit == NULL || job_id == NULL) { return FAULTLINE_STORE_REJECTED; }
    if (!usable(store)) { return FAULTLINE_STORE_FATAL; }
    int64_t time;
    if (job_time(store, now_ms, &time) < 0) { return FAULTLINE_STORE_FATAL; }
    store->scratch = store->jobs;
    uint64_t id;
    enum faultline_scheduler_result result = faultline_scheduler_submit(&store->scratch, submit, time, &id);
    if (result == FAULTLINE_SCHEDULER_QUEUE_ERROR) { return fail(store, FAULTLINE_STORE_FAILURE_INTERNAL); }
    if (result != FAULTLINE_SCHEDULER_OK) { return FAULTLINE_STORE_REJECTED; }
    if (commit_job(store, id, FAULTLINE_WAL_JOB_CREATED) != FAULTLINE_STORE_OK) { return FAULTLINE_STORE_FATAL; }
    *job_id = id;
    return FAULTLINE_STORE_OK;
}

enum faultline_store_result faultline_store_assign(
    struct faultline_coordinator_store *store, uint32_t worker_id, int64_t now_ms,
    struct faultline_message *assignment)
{
    if (store == NULL || assignment == NULL) { return FAULTLINE_STORE_REJECTED; }
    if (!usable(store)) { return FAULTLINE_STORE_FATAL; }
    const struct faultline_worker *worker = faultline_worker_find(&store->workers, worker_id);
    if (worker == NULL || worker->state != FAULTLINE_WORKER_ALIVE) { return FAULTLINE_STORE_REJECTED; }
    int64_t time;
    if (job_time(store, now_ms, &time) < 0) { return FAULTLINE_STORE_FATAL; }
    store->scratch = store->jobs;
    struct faultline_message candidate;
    enum faultline_scheduler_result result = faultline_scheduler_assign(&store->scratch, worker_id, time, &candidate);
    if (result == FAULTLINE_SCHEDULER_EMPTY) { return FAULTLINE_STORE_EMPTY; }
    if (result == FAULTLINE_SCHEDULER_WORKER_BUSY) { return FAULTLINE_STORE_REJECTED; }
    if (result != FAULTLINE_SCHEDULER_OK) { return fail(store, FAULTLINE_STORE_FAILURE_INTERNAL); }
    if (commit_job(store, candidate.payload.job_assign.identity.job_id,
                   FAULTLINE_WAL_JOB_ASSIGNED) != FAULTLINE_STORE_OK) { return FAULTLINE_STORE_FATAL; }
    *assignment = candidate;
    return FAULTLINE_STORE_OK;
}

enum faultline_store_result faultline_store_report(
    struct faultline_coordinator_store *store, uint32_t worker_id,
    const struct faultline_message *report, int64_t now_ms)
{
    if (store == NULL || report == NULL) { return FAULTLINE_STORE_REJECTED; }
    if (!usable(store)) { return FAULTLINE_STORE_FATAL; }
    const struct faultline_worker *worker = faultline_worker_find(&store->workers, worker_id);
    const struct faultline_job *active = faultline_scheduler_active(&store->jobs, worker_id);
    if (worker == NULL || worker->state != FAULTLINE_WORKER_ALIVE || active == NULL) {
        return FAULTLINE_STORE_REJECTED;
    }
    int64_t time;
    if (job_time(store, now_ms, &time) < 0) { return FAULTLINE_STORE_FATAL; }
    store->scratch = store->jobs;
    enum faultline_scheduler_result result = faultline_scheduler_report(&store->scratch, worker_id, report, time);
    if (result == FAULTLINE_SCHEDULER_QUEUE_ERROR) { return fail(store, FAULTLINE_STORE_FAILURE_INTERNAL); }
    if (result != FAULTLINE_SCHEDULER_OK) { return FAULTLINE_STORE_REJECTED; }
    const struct faultline_job *candidate = faultline_scheduler_find(&store->scratch, active->id);
    enum faultline_wal_record_type type = candidate->state == FAULTLINE_JOB_RUNNING ? FAULTLINE_WAL_JOB_STARTED :
        candidate->state == FAULTLINE_JOB_DONE ? FAULTLINE_WAL_JOB_COMPLETED :
        candidate->state == FAULTLINE_JOB_QUEUED ? FAULTLINE_WAL_JOB_REQUEUED : FAULTLINE_WAL_JOB_FAILED;
    return commit_job(store, active->id, type);
}

/* Shared durable outcome for a live disconnect or a recovered historical owner.
 * Startup has no live worker registry and must not enable the public operations
 * merely to commit these interrupted-attempt records. */
static enum faultline_store_result commit_worker_loss(
    struct faultline_coordinator_store *store, uint32_t worker_id, int64_t now_ms)
{
    const struct faultline_job *active = faultline_scheduler_active(&store->jobs, worker_id);
    if (active == NULL) { return FAULTLINE_STORE_EMPTY; }
    int64_t time;
    if (job_time(store, now_ms, &time) < 0) { return FAULTLINE_STORE_FATAL; }
    store->scratch = store->jobs;
    if (faultline_scheduler_worker_lost(&store->scratch, worker_id, time) != FAULTLINE_SCHEDULER_OK) {
        return fail(store, FAULTLINE_STORE_FAILURE_INTERNAL);
    }
    const struct faultline_job *candidate = faultline_scheduler_find(&store->scratch, active->id);
    return commit_job(store, active->id, candidate->state == FAULTLINE_JOB_QUEUED ?
                      FAULTLINE_WAL_JOB_REQUEUED : FAULTLINE_WAL_JOB_FAILED);
}

enum faultline_store_result faultline_store_worker_lost(
    struct faultline_coordinator_store *store, uint32_t worker_id, int64_t now_ms)
{
    if (store == NULL) { return FAULTLINE_STORE_REJECTED; }
    if (!usable(store)) { return FAULTLINE_STORE_FATAL; }
    return commit_worker_loss(store, worker_id, now_ms);
}

static enum faultline_store_result recover_interrupted_jobs(
    struct faultline_coordinator_store *store, int64_t now_ms)
{
    /* Replay stores jobs in increasing creation-ID order. Leave QUEUED, DONE,
     * and FAILED untouched. Existing pending work stays ahead of these retries.
     * Every outcome is synced before publication; a later restart sees a durable
     * QUEUED/FAILED outcome and cannot charge that same interruption again. */
    for (size_t i = 0; i < store->jobs.count; ++i) {
        const struct faultline_job *job = &store->jobs.jobs[i];
        if (job->state == FAULTLINE_JOB_ASSIGNED || job->state == FAULTLINE_JOB_RUNNING) {
            if (commit_worker_loss(store, job->worker_id, now_ms) != FAULTLINE_STORE_OK) {
                return FAULTLINE_STORE_FATAL;
            }
            ++store->interrupted_jobs;
        }
    }
    return FAULTLINE_STORE_OK;
}

enum faultline_store_result faultline_store_open_with_io(
    struct faultline_coordinator_store *store, const char *path, int initialize, int64_t now_ms,
    const struct faultline_wal_writer_io *io, void *context)
{
    if (store == NULL || path == NULL || (initialize != 0 && initialize != 1)) { return FAULTLINE_STORE_REJECTED; }
    if (store->opened || store->wal.io != NULL || store->failure != FAULTLINE_STORE_FAILURE_NONE) { return FAULTLINE_STORE_FATAL; }
    if (now_ms < 0) { return fail(store, FAULTLINE_STORE_FAILURE_CLOCK); }
    if (initialize) {
        store->write_result = faultline_wal_writer_create_with_io(&store->wal, path, io, context);
        if (store->write_result != FAULTLINE_WAL_WRITE_OK) { return fail(store, FAULTLINE_STORE_FAILURE_OPEN); }
    } else {
        struct faultline_wal_replay_state *recovered = malloc(sizeof(*recovered));
        if (recovered == NULL) {
            store->replay_result = FAULTLINE_WAL_REPLAY_NO_MEMORY;
            return fail(store, FAULTLINE_STORE_FAILURE_OPEN);
        }
        store->replay_result = faultline_wal_replay_open_with_io(&store->wal, path, recovered,
                                                                &store->replay_report, io, context);
        if (store->replay_result == FAULTLINE_WAL_REPLAY_OK) {
            store->jobs = recovered->scheduler;
            store->workers = recovered->workers;
            store->time_base_ms = recovered->job_time_base_ms;
        }
        free(recovered);
        if (store->replay_result != FAULTLINE_WAL_REPLAY_OK) { return fail(store, FAULTLINE_STORE_FAILURE_OPEN); }
    }
    store->session_start_ms = now_ms;
    store->last_job_time_ms = store->time_base_ms;
    if (recover_interrupted_jobs(store, now_ms) != FAULTLINE_STORE_OK) { return FAULTLINE_STORE_FATAL; }
    /* A ready WAL alone does not authorize registration, submission or dispatch. */
    store->opened = 1;
    return FAULTLINE_STORE_OK;
}

enum faultline_store_result faultline_store_open(
    struct faultline_coordinator_store *store, const char *path, int initialize, int64_t now_ms)
{
    return faultline_store_open_with_io(store, path, initialize, now_ms, faultline_wal_writer_system_io(), NULL);
}

enum faultline_store_result faultline_store_close(struct faultline_coordinator_store *store)
{
    if (store == NULL) { return FAULTLINE_STORE_REJECTED; }
    store->opened = 0;
    if (faultline_wal_writer_close(&store->wal) != FAULTLINE_WAL_WRITE_OK) {
        return fail(store, FAULTLINE_STORE_FAILURE_WAL);
    }
    return store->failure == FAULTLINE_STORE_FAILURE_NONE ? FAULTLINE_STORE_OK : FAULTLINE_STORE_FATAL;
}
