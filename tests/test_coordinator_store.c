#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "coordinator_store.h"
#include "../src/coordinator/coordinator_store_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (errno=%d)\n", \
    __FILE__, __LINE__, #x, errno); return EXIT_FAILURE; } } while (0)

struct outputs { uint32_t worker; uint64_t job; struct faultline_message assignment; };
struct fixture {
    char directory[64], path[96];
    struct faultline_coordinator_store store;
    struct faultline_scheduler before_jobs;
    struct faultline_worker_registry before_workers;
    struct outputs out, before_out;
    uint64_t before_sequence;
    int64_t before_time;
    struct faultline_wal_writer_io io;
    unsigned writes, syncs, observed;
    unsigned fail_sync;
    int armed, partial_error, partial_written, bad_observation;
};

static void observe(struct fixture *f)
{
    if (!f->armed) { return; }
    ++f->observed;
    if (memcmp(&f->store.jobs, &f->before_jobs, sizeof(f->before_jobs)) != 0 ||
        memcmp(&f->store.workers, &f->before_workers, sizeof(f->before_workers)) != 0 ||
        memcmp(&f->out, &f->before_out, sizeof(f->out)) != 0 ||
        f->store.last_job_time_ms != f->before_time ||
        f->store.wal.synced_sequence != f->before_sequence) { f->bad_observation = 1; }
}

static ssize_t injected_write(void *context, int fd, const void *bytes, size_t size)
{
    struct fixture *f = context;
    ++f->writes;
    observe(f);
    if (f->partial_error) {
        if (f->partial_written) { errno = ENOSPC; return -1; }
        f->partial_written = 1;
        return write(fd, bytes, size < 7 ? size : 7);
    }
    return write(fd, bytes, size);
}

static int injected_sync(void *context, int fd)
{
    struct fixture *f = context;
    ++f->syncs;
    observe(f);
    if (f->syncs == f->fail_sync) { errno = EIO; return -1; }
    return fsync(fd);
}

static struct fixture *setup(void)
{
    struct fixture *f = calloc(1, sizeof(*f));
    if (f == NULL) { return NULL; }
    (void)snprintf(f->directory, sizeof(f->directory), "/tmp/faultline-store-XXXXXX");
    if (mkdtemp(f->directory) == NULL) { free(f); return NULL; }
    (void)snprintf(f->path, sizeof(f->path), "%s/state.wal", f->directory);
    f->io = *faultline_wal_writer_system_io();
    f->io.write = injected_write;
    f->io.sync = injected_sync;
    faultline_store_init(&f->store);
    if (faultline_store_open_with_io(&f->store, f->path, 1, 100, &f->io, f) != FAULTLINE_STORE_OK) {
        (void)faultline_store_close(&f->store); (void)unlink(f->path); (void)rmdir(f->directory); free(f); return NULL;
    }
    return f;
}

static int cleanup(struct fixture *f)
{
    (void)faultline_store_close(&f->store);
    CHECK(unlink(f->path) == 0 && rmdir(f->directory) == 0);
    free(f);
    return EXIT_SUCCESS;
}

static const struct faultline_job_submit_payload sample = {
    .task_type = FAULTLINE_TASK_HASH, .max_retries = 1,
    .argument_size = 3, .arguments = {'a', 0, 'b'}
};

/* Every externally meaningful mutation has a separate publication boundary. */
enum operation { REGISTER, SUBMIT, ASSIGN, START, COMPLETE, TASK_RETRY, TASK_EXHAUSTED, LOST_RETRY, LOST_EXHAUSTED };

static enum faultline_store_result perform(struct fixture *f, enum operation operation)
{
    struct faultline_message report = {.message_type = FAULTLINE_MSG_JOB_STARTED, .payload.job_started = {1, 1, 1}};
    switch (operation) {
    case REGISTER: return faultline_store_register(&f->store, 10, 110, &f->out.worker);
    case SUBMIT: return faultline_store_submit(&f->store, &sample, 110, &f->out.job);
    case ASSIGN: return faultline_store_assign(&f->store, 1, 110, &f->out.assignment);
    case START: return faultline_store_report(&f->store, 1, &report, 110);
    case COMPLETE:
        report = (struct faultline_message){.message_type = FAULTLINE_MSG_JOB_COMPLETED,
            .payload.job_completed = {.identity = {1, 1, 1}, .result_size = 3, .result = {1, 0, 255}}};
        return faultline_store_report(&f->store, 1, &report, 110);
    case TASK_RETRY: case TASK_EXHAUSTED:
        report = (struct faultline_message){.message_type = FAULTLINE_MSG_JOB_FAILED,
            .payload.job_failed = {.identity = {1, 1, 1}, .failure = FAULTLINE_JOB_FAILURE_TASK}};
        return faultline_store_report(&f->store, 1, &report, 110);
    case LOST_RETRY: case LOST_EXHAUSTED: return faultline_store_worker_lost(&f->store, 1, 110);
    }
    return FAULTLINE_STORE_REJECTED;
}

static int prepare(struct fixture *f, enum operation operation)
{
    if (operation == REGISTER) { return EXIT_SUCCESS; }
    CHECK(faultline_store_register(&f->store, 10, 101, &f->out.worker) == FAULTLINE_STORE_OK);
    if (operation == SUBMIT) { return EXIT_SUCCESS; }
    struct faultline_job_submit_payload submit = sample;
    if (operation == TASK_EXHAUSTED || operation == LOST_EXHAUSTED) { submit.max_retries = 0; }
    CHECK(faultline_store_submit(&f->store, &submit, 102, &f->out.job) == FAULTLINE_STORE_OK);
    if (operation == ASSIGN) { return EXIT_SUCCESS; }
    CHECK(faultline_store_assign(&f->store, 1, 103, &f->out.assignment) == FAULTLINE_STORE_OK);
    if (operation == START || operation == TASK_RETRY || operation == TASK_EXHAUSTED) { return EXIT_SUCCESS; }
    struct faultline_message started = {.message_type = FAULTLINE_MSG_JOB_STARTED, .payload.job_started = {1, 1, 1}};
    CHECK(faultline_store_report(&f->store, 1, &started, 104) == FAULTLINE_STORE_OK);
    return EXIT_SUCCESS;
}

static void arm(struct fixture *f)
{
    memset(&f->out, 0xa5, sizeof(f->out));
    memcpy(&f->before_out, &f->out, sizeof(f->out));
    memcpy(&f->before_jobs, &f->store.jobs, sizeof(f->before_jobs));
    memcpy(&f->before_workers, &f->store.workers, sizeof(f->before_workers));
    f->before_sequence = f->store.wal.synced_sequence;
    f->before_time = f->store.last_job_time_ms;
    f->armed = 1;
}

static int test_publication(void)
{
    for (enum operation op = REGISTER; op <= LOST_EXHAUSTED; ++op) {
        struct fixture *f = setup();
        CHECK(f != NULL && prepare(f, op) == EXIT_SUCCESS);
        arm(f);
        unsigned syncs = f->syncs;
        CHECK(perform(f, op) == FAULTLINE_STORE_OK);
        CHECK(f->observed >= 2 && !f->bad_observation && f->syncs == syncs + 1);
        CHECK(f->store.wal.synced_sequence == f->before_sequence + 1);
        if (op == REGISTER) {
            CHECK(f->out.worker == 1 && f->store.workers.next_worker_id == 2);
            CHECK(faultline_worker_find(&f->store.workers, 1)->last_heartbeat_ms == 110);
        } else {
            const struct faultline_job *job = faultline_scheduler_find(&f->store.jobs, 1);
            CHECK(job != NULL && job->updated_at_ms == 10);
            if (op == SUBMIT) { CHECK(f->out.job == 1 && f->store.jobs.pending.count == 1); }
            if (op == ASSIGN) { CHECK(f->out.assignment.payload.job_assign.identity.attempt == 1 && job->state == FAULTLINE_JOB_ASSIGNED); }
            if (op == START) { CHECK(job->state == FAULTLINE_JOB_RUNNING); }
            if (op == COMPLETE) { CHECK(job->state == FAULTLINE_JOB_DONE && job->result_size == 3 && job->result[2] == 255); }
            if (op == TASK_RETRY || op == LOST_RETRY) { CHECK(job->state == FAULTLINE_JOB_QUEUED && job->retry_count == 1); }
            if (op == TASK_EXHAUSTED || op == LOST_EXHAUSTED) { CHECK(job->state == FAULTLINE_JOB_FAILED && job->retry_count == 0); }
        }
        CHECK(cleanup(f) == EXIT_SUCCESS);
    }
    return EXIT_SUCCESS;
}

static int test_storage_failures(void)
{
    for (int mode = 0; mode < 2; ++mode) {
        for (enum operation op = REGISTER; op <= LOST_EXHAUSTED; ++op) {
            struct fixture *f = setup();
            CHECK(f != NULL && prepare(f, op) == EXIT_SUCCESS);
            arm(f);
            if (mode == 0) { f->partial_error = 1; } else { f->fail_sync = f->syncs + 1; }
            CHECK(perform(f, op) == FAULTLINE_STORE_FATAL);
            observe(f);
            CHECK(!f->bad_observation && f->store.failure == FAULTLINE_STORE_FAILURE_WAL);
            CHECK(f->store.wal.state == FAULTLINE_WAL_WRITER_FAILED);
            unsigned writes = f->writes, syncs = f->syncs;
            for (enum operation later = REGISTER; later <= LOST_EXHAUSTED; ++later) {
                CHECK(perform(f, later) == FAULTLINE_STORE_FATAL);
            }
            CHECK(faultline_store_close(&f->store) == FAULTLINE_STORE_FATAL);
            CHECK(f->writes == writes && f->syncs == syncs);
            /* A complete record whose sync failed may survive; partial writes
             * are repaired. Neither case authorizes success in the old process. */
            f->armed = f->partial_error = 0; f->fail_sync = 0;
            faultline_store_init(&f->store);
            CHECK(faultline_store_open_with_io(&f->store, f->path, 0, 0, &f->io, f) == FAULTLINE_STORE_OK);
            CHECK(f->store.replay_report.tail_bytes == (mode == 0 ? 7u : 0u));
            CHECK(f->store.replay_report.last_sequence == f->before_sequence + (mode == 1 ? 1u : 0u));
            CHECK(cleanup(f) == EXIT_SUCCESS);
        }
    }
    return EXIT_SUCCESS;
}

static int test_rejections(void)
{
    struct fixture *f = setup();
    CHECK(f != NULL && prepare(f, START) == EXIT_SUCCESS);
    arm(f);
    struct faultline_message stale = {.message_type = FAULTLINE_MSG_JOB_STARTED, .payload.job_started = {1, 1, 2}};
    struct faultline_job_submit_payload bad = sample; bad.task_type = 999;
    unsigned writes = f->writes, syncs = f->syncs;
    CHECK(faultline_store_register(&f->store, 10, 110, &f->out.worker) == FAULTLINE_STORE_REJECTED);
    CHECK(faultline_store_submit(&f->store, &bad, 110, &f->out.job) == FAULTLINE_STORE_REJECTED);
    CHECK(faultline_store_assign(&f->store, 99, 110, &f->out.assignment) == FAULTLINE_STORE_REJECTED);
    CHECK(faultline_store_assign(&f->store, 1, 110, &f->out.assignment) == FAULTLINE_STORE_REJECTED);
    CHECK(faultline_store_report(&f->store, 1, &stale, 110) == FAULTLINE_STORE_REJECTED);
    CHECK(faultline_store_worker_lost(&f->store, 99, 110) == FAULTLINE_STORE_EMPTY);
    CHECK(faultline_store_submit(&f->store, NULL, 110, &f->out.job) == FAULTLINE_STORE_REJECTED);
    observe(f);
    CHECK(!f->bad_observation && f->writes == writes && f->syncs == syncs);
    CHECK(f->store.failure == FAULTLINE_STORE_FAILURE_NONE);
    CHECK(cleanup(f) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static int seed_interrupted(struct fixture *f)
{
    uint32_t worker;
    uint64_t id;
    struct faultline_message assignment;
    CHECK(faultline_store_register(&f->store, 10, 101, &worker) == FAULTLINE_STORE_OK);
    CHECK(faultline_store_register(&f->store, 11, 102, &worker) == FAULTLINE_STORE_OK);
    CHECK(faultline_store_submit(&f->store, &sample, 200, &id) == FAULTLINE_STORE_OK);
    CHECK(faultline_store_assign(&f->store, 1, 201, &assignment) == FAULTLINE_STORE_OK);
    struct faultline_message started = {.message_type = FAULTLINE_MSG_JOB_STARTED, .payload.job_started = {1, 1, 1}};
    CHECK(faultline_store_report(&f->store, 1, &started, 202) == FAULTLINE_STORE_OK);
    struct faultline_job_submit_payload no_retries = sample; no_retries.max_retries = 0;
    CHECK(faultline_store_submit(&f->store, &no_retries, 203, &id) == FAULTLINE_STORE_OK);
    CHECK(faultline_store_assign(&f->store, 2, 204, &assignment) == FAULTLINE_STORE_OK);
    CHECK(faultline_store_submit(&f->store, &sample, 5000, &id) == FAULTLINE_STORE_OK);
    CHECK(faultline_store_close(&f->store) == FAULTLINE_STORE_OK);
    return EXIT_SUCCESS;
}

static int check_reconciled(const struct fixture *f)
{
    const struct faultline_job *first = faultline_scheduler_find(&f->store.jobs, 1);
    const struct faultline_job *second = faultline_scheduler_find(&f->store.jobs, 2);
    CHECK(first->state == FAULTLINE_JOB_QUEUED && first->retry_count == 1 && first->attempt == 1);
    CHECK(second->state == FAULTLINE_JOB_FAILED && second->retry_count == 0 && second->attempt == 1);
    CHECK(first->failure == FAULTLINE_JOB_FAILURE_WORKER_LOST && second->failure == FAULTLINE_JOB_FAILURE_WORKER_LOST);
    CHECK(f->store.jobs.pending.count == 2 && f->store.workers.next_worker_id == 3);
    CHECK(f->store.jobs.next_job_id == 4 && f->store.time_base_ms == 4900);
    struct faultline_job_queue queue = f->store.jobs.pending;
    uint64_t id;
    CHECK(faultline_job_queue_pop(&queue, &id) == FAULTLINE_JOB_QUEUE_OK && id == 3);
    CHECK(faultline_job_queue_pop(&queue, &id) == FAULTLINE_JOB_QUEUE_OK && id == 1);
    for (size_t i = 0; i < FAULTLINE_MAX_WORKERS; ++i) { CHECK(f->store.workers.workers[i].state == FAULTLINE_WORKER_UNUSED); }
    return EXIT_SUCCESS;
}

static int test_restart_and_clock(void)
{
    struct fixture *f = setup();
    CHECK(f != NULL && seed_interrupted(f) == EXIT_SUCCESS);
    uint64_t sequence = f->store.wal.synced_sequence;
    faultline_store_init(&f->store);
    CHECK(faultline_store_open(&f->store, f->path, 0, 10) == FAULTLINE_STORE_OK);
    CHECK(f->store.interrupted_jobs == 2 && f->store.wal.synced_sequence == sequence + 2);
    CHECK(check_reconciled(f) == EXIT_SUCCESS);
    CHECK(faultline_store_close(&f->store) == FAULTLINE_STORE_OK);
    faultline_store_init(&f->store);
    CHECK(faultline_store_open(&f->store, f->path, 0, 0) == FAULTLINE_STORE_OK);
    CHECK(f->store.interrupted_jobs == 0 && f->store.wal.synced_sequence == sequence + 2);
    CHECK(check_reconciled(f) == EXIT_SUCCESS);
    CHECK(faultline_store_register(&f->store, 10, 1, &f->out.worker) == FAULTLINE_STORE_OK && f->out.worker == 3);
    CHECK(faultline_worker_find(&f->store.workers, 3)->last_heartbeat_ms == 1);
    CHECK(faultline_store_assign(&f->store, 3, 2, &f->out.assignment) == FAULTLINE_STORE_OK);
    CHECK(f->out.assignment.payload.job_assign.identity.job_id == 3);
    CHECK(faultline_scheduler_find(&f->store.jobs, 3)->assigned_at_ms == 4902);
    CHECK(cleanup(f) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static int test_interrupted_reconciliation(void)
{
    for (unsigned fail_at = 3; fail_at <= 4; ++fail_at) {
        struct fixture *f = setup();
        CHECK(f != NULL && seed_interrupted(f) == EXIT_SUCCESS);
        faultline_store_init(&f->store);
        f->syncs = 0; f->fail_sync = fail_at;
        CHECK(faultline_store_open_with_io(&f->store, f->path, 0, 0, &f->io, f) == FAULTLINE_STORE_FATAL);
        CHECK(f->store.interrupted_jobs == fail_at - 3);
        CHECK(faultline_store_close(&f->store) == FAULTLINE_STORE_FATAL);
        faultline_store_init(&f->store); f->fail_sync = 0;
        CHECK(faultline_store_open(&f->store, f->path, 0, 0) == FAULTLINE_STORE_OK);
        CHECK(f->store.interrupted_jobs == 4 - fail_at && check_reconciled(f) == EXIT_SUCCESS);
        CHECK(cleanup(f) == EXIT_SUCCESS);
    }
    return EXIT_SUCCESS;
}

static int test_clock_and_sequence_failures(void)
{
    for (int kind = 0; kind < 3; ++kind) {
        struct fixture *f = setup();
        CHECK(f != NULL && prepare(f, ASSIGN) == EXIT_SUCCESS);
        if (kind == 0) { f->store.time_base_ms = INT64_MAX; } /* Seed the arithmetic boundary. */
        if (kind == 2) { f->store.wal.next_sequence = 0; }
        arm(f);
        unsigned writes = f->writes, syncs = f->syncs;
        CHECK(faultline_store_assign(&f->store, 1, kind == 1 ? 101 : 110, &f->out.assignment) == FAULTLINE_STORE_FATAL);
        observe(f);
        CHECK(!f->bad_observation && f->writes == writes && f->syncs == syncs);
        CHECK(f->store.failure == (kind == 2 ? FAULTLINE_STORE_FAILURE_WAL : FAULTLINE_STORE_FAILURE_CLOCK));
        CHECK(cleanup(f) == EXIT_SUCCESS);
    }
    return EXIT_SUCCESS;
}

int main(void)
{
    const struct { const char *name; int (*run)(void); } tests[] = {
        {"all coordinator mutations publish only after record sync", test_publication},
        {"partial writes and failed syncs preserve live state and stop subsequent work", test_storage_failures},
        {"invalid requests and stale reports perform no storage I/O", test_rejections},
        {"startup reconciliation, FIFO, retry budgets, identities, and logical time", test_restart_and_clock},
        {"failed reconciliation sync does not charge an interrupted attempt twice", test_interrupted_reconciliation},
        {"clock overflow/regression and sequence exhaustion fail closed", test_clock_and_sequence_failures}
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (tests[i].run() != EXIT_SUCCESS) { return EXIT_FAILURE; }
        printf("PASS: %s\n", tests[i].name);
    }
    return EXIT_SUCCESS;
}
