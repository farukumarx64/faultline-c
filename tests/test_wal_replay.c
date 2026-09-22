#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "wal_replay.h"
#include "../src/coordinator/wal_replay_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (errno=%d)\n", \
    __FILE__, __LINE__, #x, errno); return EXIT_FAILURE; } } while (0)

struct fixture {
    char directory[64], path[96];
    uint8_t bytes[262144];
    size_t size, last_offset, last_size;
    uint64_t sequence;
    struct faultline_wal_replay_state *state;
};

static struct fixture *setup(void)
{
    struct fixture *f = calloc(1, sizeof(*f));
    if (f == NULL) { return NULL; }
    f->state = malloc(sizeof(*f->state));
    if (f->state == NULL) { free(f); return NULL; }
    (void)snprintf(f->directory, sizeof(f->directory), "/tmp/faultline-replay-XXXXXX");
    if (mkdtemp(f->directory) == NULL) { free(f->state); free(f); return NULL; }
    (void)snprintf(f->path, sizeof(f->path), "%s/state.wal", f->directory);
    (void)faultline_wal_file_header_encode(f->bytes, sizeof(f->bytes));
    f->size = FAULTLINE_WAL_FILE_HEADER_SIZE;
    return f;
}

static int cleanup(struct fixture *f)
{
    CHECK(unlink(f->path) == 0 || errno == ENOENT);
    CHECK(rmdir(f->directory) == 0);
    free(f->state);
    free(f);
    return EXIT_SUCCESS;
}

static int save(const struct fixture *f, size_t size)
{
    int fd = open(f->path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = write(fd, f->bytes + offset, size - offset);
        if (count < 0 && errno == EINTR) { continue; }
        CHECK(count > 0);
        offset += (size_t)count;
    }
    CHECK(close(fd) == 0);
    return EXIT_SUCCESS;
}

static int file_matches(const struct fixture *f, size_t size)
{
    uint8_t buffer[4096];
    int fd = open(f->path, O_RDONLY);
    CHECK(fd >= 0);
    size_t offset = 0;
    while (offset < size) {
        size_t request = size - offset < sizeof(buffer) ? size - offset : sizeof(buffer);
        ssize_t count = read(fd, buffer, request);
        if (count < 0 && errno == EINTR) { continue; }
        CHECK(count > 0 && memcmp(buffer, f->bytes + offset, (size_t)count) == 0);
        offset += (size_t)count;
    }
    CHECK(read(fd, buffer, 1) == 0 && close(fd) == 0);
    return EXIT_SUCCESS;
}

static int add_record(struct fixture *f, struct faultline_wal_record *record)
{
    record->sequence = ++f->sequence;
    f->last_offset = f->size;
    CHECK(faultline_wal_record_encode(f->bytes + f->size, sizeof(f->bytes) - f->size,
                                     record, &f->last_size) == FAULTLINE_WAL_OK);
    f->size += f->last_size;
    return EXIT_SUCCESS;
}

static int add_worker(struct fixture *f, uint32_t id)
{
    struct faultline_wal_record record = {.type = FAULTLINE_WAL_WORKER_ID_ALLOCATED, .payload.worker_id = id};
    return add_record(f, &record);
}

static int add_job(struct fixture *f, const struct faultline_job *job, enum faultline_wal_record_type type)
{
    struct faultline_wal_record record = {.type = type, .payload.job = *job};
    return add_record(f, &record);
}

static int new_job(struct fixture *f, struct faultline_job *job, uint64_t id, uint32_t budget,
                   enum faultline_task_type task, int64_t time)
{
    const uint8_t argument[] = {'1', 0, '2'};
    CHECK(faultline_job_init(job, id, task, argument, sizeof(argument), budget, time) == FAULTLINE_JOB_OK);
    return add_job(f, job, FAULTLINE_WAL_JOB_CREATED);
}

static int assign(struct fixture *f, struct faultline_job *job, uint32_t worker, int64_t time)
{
    CHECK(faultline_job_assign(job, worker, time) == FAULTLINE_JOB_OK);
    return add_job(f, job, FAULTLINE_WAL_JOB_ASSIGNED);
}

static int start(struct fixture *f, struct faultline_job *job, int64_t time)
{
    CHECK(faultline_job_start(job, job->worker_id, job->attempt, time) == FAULTLINE_JOB_OK);
    return add_job(f, job, FAULTLINE_WAL_JOB_STARTED);
}

static int finish(struct fixture *f, struct faultline_job *job, int64_t time)
{
    uint8_t result[1024];
    for (size_t i = 0; i < sizeof(result); ++i) { result[i] = (uint8_t)i; }
    CHECK(faultline_job_complete(job, job->worker_id, job->attempt, result, sizeof(result), time) == FAULTLINE_JOB_OK);
    return add_job(f, job, FAULTLINE_WAL_JOB_COMPLETED);
}

static int fail_job(struct fixture *f, struct faultline_job *job, enum faultline_job_failure reason, int64_t time)
{
    CHECK(faultline_job_fail(job, job->worker_id, job->attempt, reason, time) == FAULTLINE_JOB_OK);
    return add_job(f, job, job->state == FAULTLINE_JOB_QUEUED ? FAULTLINE_WAL_JOB_REQUEUED : FAULTLINE_WAL_JOB_FAILED);
}

static int untouched(const struct faultline_wal_replay_state *state)
{
    const unsigned char *bytes = (const unsigned char *)state;
    for (size_t i = 0; i < sizeof(*state); ++i) { if (bytes[i] != 0xa5) { return 0; } }
    return 1;
}

static int rejected(struct fixture *f, enum faultline_wal_replay_result expected)
{
    struct faultline_wal_writer writer = FAULTLINE_WAL_WRITER_INIT;
    struct faultline_wal_replay_report report;
    memset(f->state, 0xa5, sizeof(*f->state));
    CHECK(save(f, f->size) == EXIT_SUCCESS);
    CHECK(faultline_wal_replay_open(&writer, f->path, f->state, &report) == expected);
    CHECK(untouched(f->state) && writer.state == FAULTLINE_WAL_WRITER_FAILED);
    CHECK(writer.synced_sequence == 0 && writer.next_sequence == 0);
    if (expected == FAULTLINE_WAL_REPLAY_BAD_FORMAT) { CHECK(report.format_error != FAULTLINE_WAL_OK); }
    if (expected == FAULTLINE_WAL_REPLAY_BAD_HISTORY) { CHECK(report.history_error != FAULTLINE_WAL_HISTORY_OK); }
    CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_IO_ERROR);
    CHECK(file_matches(f, f->size) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static int empty_registry(const struct faultline_worker_registry *registry)
{
    for (size_t i = 0; i < FAULTLINE_MAX_WORKERS; ++i) {
        CHECK(registry->workers[i].id == 0 && registry->workers[i].fd == -1);
        CHECK(registry->workers[i].state == FAULTLINE_WORKER_UNUSED && registry->workers[i].last_heartbeat_ms == 0);
    }
    return EXIT_SUCCESS;
}

static int test_empty_and_resume(void)
{
    struct fixture *f = setup();
    CHECK(f != NULL && save(f, f->size) == EXIT_SUCCESS);
    struct faultline_wal_writer writer = FAULTLINE_WAL_WRITER_INIT;
    struct faultline_wal_replay_report report;
    CHECK(faultline_wal_replay_open(&writer, f->path, f->state, &report) == FAULTLINE_WAL_REPLAY_OK);
    CHECK(f->state->scheduler.count == 0 && f->state->scheduler.next_job_id == 1);
    CHECK(f->state->workers.next_worker_id == 1 && empty_registry(&f->state->workers) == EXIT_SUCCESS);
    CHECK(f->state->job_time_base_ms == 0 && f->state->last_sequence == 0);
    CHECK(report.valid_bytes == 24 && report.tail_bytes == 0 && writer.next_sequence == 1);
    CHECK((fcntl(writer.fd, F_GETFL) & O_APPEND) != 0 && (fcntl(writer.fd, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK(add_worker(f, 7) == EXIT_SUCCESS);
    struct faultline_wal_record record = {.type = FAULTLINE_WAL_WORKER_ID_ALLOCATED, .sequence = 1, .payload.worker_id = 7};
    CHECK(faultline_wal_writer_append(&writer, &record) == FAULTLINE_WAL_WRITE_OK);
    CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_OK);
    CHECK(file_matches(f, f->size) == EXIT_SUCCESS);
    CHECK(faultline_wal_replay_open(&writer, f->path, f->state, &report) == FAULTLINE_WAL_REPLAY_OK);
    CHECK(f->state->highest_worker_id == 7 && f->state->workers.next_worker_id == 8);
    CHECK(report.last_sequence == 1 && writer.next_sequence == 2 && writer.synced_sequence == 1);
    CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_OK);
    CHECK(cleanup(f) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static int test_mixed_history(void)
{
    struct fixture *f = setup();
    struct faultline_job a, b, c, d, e, g, h;
    CHECK(f != NULL);
    CHECK(add_worker(f, 10) == EXIT_SUCCESS && add_worker(f, 11) == EXIT_SUCCESS && add_worker(f, 30) == EXIT_SUCCESS);
    CHECK(new_job(f, &a, 5, 2, FAULTLINE_TASK_HASH, 1) == EXIT_SUCCESS);
    CHECK(new_job(f, &b, 9, 0, FAULTLINE_TASK_SLEEP, 2) == EXIT_SUCCESS);
    CHECK(new_job(f, &c, 12, 1, FAULTLINE_TASK_HASH, 3) == EXIT_SUCCESS);
    CHECK(assign(f, &a, 10, 4) == EXIT_SUCCESS && start(f, &a, 5) == EXIT_SUCCESS);
    CHECK(assign(f, &b, 11, 6) == EXIT_SUCCESS && fail_job(f, &b, FAULTLINE_JOB_FAILURE_WORKER_LOST, 7) == EXIT_SUCCESS);
    CHECK(fail_job(f, &a, FAULTLINE_JOB_FAILURE_WORKER_LOST, 8) == EXIT_SUCCESS);
    CHECK(assign(f, &c, 10, 9) == EXIT_SUCCESS && start(f, &c, 10) == EXIT_SUCCESS && finish(f, &c, 11) == EXIT_SUCCESS);
    CHECK(assign(f, &a, 11, 12) == EXIT_SUCCESS && start(f, &a, 13) == EXIT_SUCCESS && finish(f, &a, 14) == EXIT_SUCCESS);
    CHECK(new_job(f, &d, 20, 0, FAULTLINE_TASK_SLEEP, 15) == EXIT_SUCCESS);
    CHECK(new_job(f, &e, 25, 1, FAULTLINE_TASK_PRIME_COUNT, 16) == EXIT_SUCCESS);
    CHECK(new_job(f, &g, 27, 2, FAULTLINE_TASK_FIBONACCI, 17) == EXIT_SUCCESS);
    CHECK(assign(f, &d, 10, 18) == EXIT_SUCCESS && assign(f, &e, 11, 19) == EXIT_SUCCESS && start(f, &e, 20) == EXIT_SUCCESS);
    CHECK(new_job(f, &h, 40, 0, FAULTLINE_TASK_HASH, 21) == EXIT_SUCCESS);
    CHECK(assign(f, &g, 30, 22) == EXIT_SUCCESS && fail_job(f, &g, FAULTLINE_JOB_FAILURE_TASK, 23) == EXIT_SUCCESS);
    CHECK(add_worker(f, 50) == EXIT_SUCCESS && save(f, f->size) == EXIT_SUCCESS);
    for (int pass = 0; pass < 3; ++pass) {
        struct faultline_wal_writer writer = FAULTLINE_WAL_WRITER_INIT;
        struct faultline_wal_replay_report report;
        CHECK(faultline_wal_replay_open(&writer, f->path, f->state, &report) == FAULTLINE_WAL_REPLAY_OK);
        CHECK(f->state->scheduler.count == 7 && f->state->scheduler.next_job_id == 41);
        CHECK(f->state->highest_worker_id == 50 && f->state->workers.next_worker_id == 51);
        CHECK(f->state->highest_job_id == 40 && f->state->job_time_base_ms == 23);
        CHECK(empty_registry(&f->state->workers) == EXIT_SUCCESS);
        const struct faultline_job *expected[] = {&a, &b, &c, &d, &e, &g, &h};
        for (size_t i = 0; i < 7; ++i) {
            const struct faultline_job *job = faultline_scheduler_find(&f->state->scheduler, expected[i]->id);
            CHECK(job != NULL);
            enum faultline_wal_record_type type = job->state == FAULTLINE_JOB_DONE ? FAULTLINE_WAL_JOB_COMPLETED :
                job->state == FAULTLINE_JOB_FAILED ? FAULTLINE_WAL_JOB_FAILED :
                job->state == FAULTLINE_JOB_ASSIGNED ? FAULTLINE_WAL_JOB_ASSIGNED :
                job->state == FAULTLINE_JOB_RUNNING ? FAULTLINE_WAL_JOB_STARTED :
                job->retry_count ? FAULTLINE_WAL_JOB_REQUEUED : FAULTLINE_WAL_JOB_CREATED;
            struct faultline_wal_record left = {.type = type, .sequence = 1, .payload.job = *job};
            struct faultline_wal_record right = {.type = type, .sequence = 1, .payload.job = *expected[i]};
            uint8_t x[FAULTLINE_WAL_MAX_RECORD_SIZE], y[sizeof(x)];
            size_t nx, ny;
            CHECK(faultline_wal_record_encode(x, sizeof(x), &left, &nx) == FAULTLINE_WAL_OK);
            CHECK(faultline_wal_record_encode(y, sizeof(y), &right, &ny) == FAULTLINE_WAL_OK);
            CHECK(nx == ny && memcmp(x, y, nx) == 0);
        }
        uint64_t id;
        CHECK(f->state->scheduler.pending.count == 2);
        CHECK(faultline_job_queue_pop(&f->state->scheduler.pending, &id) == FAULTLINE_JOB_QUEUE_OK && id == 40);
        CHECK(faultline_job_queue_pop(&f->state->scheduler.pending, &id) == FAULTLINE_JOB_QUEUE_OK && id == 27);
        uint32_t worker;
        CHECK(faultline_worker_register(&f->state->workers, 99, 0, &worker) == FAULTLINE_REGISTRY_OK && worker == 51);
        CHECK(report.tail_bytes == 0 && report.last_sequence == f->sequence && report.valid_bytes == f->size);
        CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_OK);
        CHECK(file_matches(f, f->size) == EXIT_SUCCESS);
    }
    CHECK(cleanup(f) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static int test_incomplete_tails(void)
{
    struct fixture *f = setup();
    struct faultline_job job;
    uint8_t arguments[1024];
    for (size_t i = 0; i < sizeof(arguments); ++i) { arguments[i] = (uint8_t)i; }
    CHECK(f != NULL && add_worker(f, 1) == EXIT_SUCCESS);
    CHECK(faultline_job_init(&job, 1, FAULTLINE_TASK_HASH, arguments, sizeof(arguments), 0, 0) == FAULTLINE_JOB_OK);
    CHECK(add_job(f, &job, FAULTLINE_WAL_JOB_CREATED) == EXIT_SUCCESS);
    CHECK(assign(f, &job, 1, 0) == EXIT_SUCCESS && start(f, &job, 0) == EXIT_SUCCESS && finish(f, &job, 0) == EXIT_SUCCESS);
    CHECK(f->last_size == FAULTLINE_WAL_MAX_RECORD_SIZE);
    for (size_t tail = 0; tail < f->last_size; ++tail) {
        CHECK(save(f, f->last_offset + tail) == EXIT_SUCCESS);
        struct faultline_wal_writer writer = FAULTLINE_WAL_WRITER_INIT;
        struct faultline_wal_replay_report report;
        CHECK(faultline_wal_replay_open(&writer, f->path, f->state, &report) == FAULTLINE_WAL_REPLAY_OK);
        CHECK(report.valid_bytes == f->last_offset && report.tail_bytes == tail && report.last_sequence == f->sequence - 1);
        CHECK(writer.next_sequence == f->sequence && writer.synced_sequence == f->sequence - 1);
        const struct faultline_job *saved = faultline_scheduler_find(&f->state->scheduler, 1);
        CHECK(saved != NULL && saved->state == FAULTLINE_JOB_RUNNING && saved->attempt == 1 && saved->retry_count == 0);
        CHECK(saved->argument_size == sizeof(arguments) && memcmp(saved->arguments, arguments, sizeof(arguments)) == 0);
        CHECK(file_matches(f, f->last_offset) == EXIT_SUCCESS);
        if (tail == 1 || tail == 31 || tail == 32 || tail + 1 == f->last_size) {
            struct faultline_wal_record record = {.type = FAULTLINE_WAL_JOB_COMPLETED, .sequence = f->sequence, .payload.job = job};
            CHECK(faultline_wal_writer_append(&writer, &record) == FAULTLINE_WAL_WRITE_OK);
            CHECK(file_matches(f, f->size) == EXIT_SUCCESS);
        }
        CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_OK);
    }
    CHECK(save(f, f->size) == EXIT_SUCCESS);
    struct faultline_wal_writer writer = FAULTLINE_WAL_WRITER_INIT;
    struct faultline_wal_replay_report report;
    CHECK(faultline_wal_replay_open(&writer, f->path, f->state, &report) == FAULTLINE_WAL_REPLAY_OK);
    CHECK(report.tail_bytes == 0 && f->state->scheduler.jobs[0].state == FAULTLINE_JOB_DONE);
    CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_OK && cleanup(f) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static void put32(uint8_t *out, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) { out[i] = (uint8_t)(value >> (24 - i * 8)); }
}

static uint32_t crc(const uint8_t *bytes, size_t size)
{
    uint32_t value = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        value ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) { value = (value >> 1) ^ ((value & 1u) ? UINT32_C(0xedb88320) : 0); }
    }
    return value ^ UINT32_MAX;
}

static int test_file_headers(void)
{
    struct fixture *f = setup();
    CHECK(f != NULL);
    for (size_t size = 0; size < 24; ++size) {
        f->size = size;
        CHECK(rejected(f, FAULTLINE_WAL_REPLAY_BAD_FORMAT) == EXIT_SUCCESS);
    }
    f->size = 24;
    const size_t offsets[] = {0, 9, 11, 15, 19, 23};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        CHECK(faultline_wal_file_header_encode(f->bytes, sizeof(f->bytes)) == FAULTLINE_WAL_OK);
        f->bytes[offsets[i]] ^= 1;
        if (offsets[i] < 20) { put32(f->bytes + 20, crc(f->bytes, 20)); }
        CHECK(rejected(f, FAULTLINE_WAL_REPLAY_BAD_FORMAT) == EXIT_SUCCESS);
    }
    CHECK(cleanup(f) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static int test_corrupt_records(void)
{
    struct fixture *f = setup();
    CHECK(f != NULL && add_worker(f, 1) == EXIT_SUCCESS && add_worker(f, 2) == EXIT_SUCCESS);
    uint8_t original[36];
    memcpy(original, f->bytes + f->last_offset, sizeof(original));
    for (int kind = 0; kind < 12; ++kind) {
        uint8_t *record = f->bytes + f->last_offset;
        memcpy(record, original, sizeof(original));
        f->size = f->last_offset + 36;
        switch (kind) {
        case 0: record[35] ^= 1; break; /* complete corrupt payload at EOF */
        case 1: record[11] ^= 1; break; /* damaged length must not become a partial tail */
        case 2: record[0] ^= 1; break;
        case 3: record[5] = 2; break;
        case 4: record[7] = 99; break;
        case 5: record[15] = 1; break;
        case 6: record[23] = 1; break; /* duplicate sequence */
        case 7: record[23] = 3; break; /* sequence gap */
        case 8: put32(record + 8, UINT32_MAX); break;
        case 9: record[23] = 0; break;
        case 10: record[23] = 3; f->size = f->last_offset + 32; break; /* no payload: bad sequence still fatal */
        default: record[35] = 0; put32(record + 24, crc(record + 32, 4)); break; /* zero worker */
        }
        if (kind >= 2) { put32(record + 28, crc(record, 28)); }
        CHECK(rejected(f, FAULTLINE_WAL_REPLAY_BAD_FORMAT) == EXIT_SUCCESS);
    }
    /* Never skip corruption earlier in a file to repair an incomplete tail. */
    memcpy(f->bytes + f->last_offset, original, 36);
    f->size = f->last_offset + 36;
    f->bytes[59] ^= 1;
    f->bytes[f->size++] = 0x46;
    CHECK(rejected(f, FAULTLINE_WAL_REPLAY_BAD_FORMAT) == EXIT_SUCCESS);
    CHECK(cleanup(f) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static int test_bad_histories(void)
{
    for (int kind = 0; kind < 25; ++kind) {
        struct fixture *f = setup();
        struct faultline_job job, other, saved;
        CHECK(f != NULL && add_worker(f, 1) == EXIT_SUCCESS && add_worker(f, 3) == EXIT_SUCCESS);
        CHECK(new_job(f, &job, 10, kind == 18 ? 0u : 2u, FAULTLINE_TASK_HASH, 10) == EXIT_SUCCESS);
        if (kind != 22 && kind != 24) {
            CHECK(assign(f, &job, 1, 20) == EXIT_SUCCESS);
            if (kind != 16) { CHECK(start(f, &job, 30) == EXIT_SUCCESS); }
        }
        if (kind == 17) { CHECK(finish(f, &job, 40) == EXIT_SUCCESS); }
        if (kind == 18) { CHECK(fail_job(f, &job, FAULTLINE_JOB_FAILURE_TASK, 40) == EXIT_SUCCESS); }
        if (kind == 19) {
            CHECK(fail_job(f, &job, FAULTLINE_JOB_FAILURE_TASK, 40) == EXIT_SUCCESS);
            CHECK(assign(f, &job, 1, 50) == EXIT_SUCCESS && start(f, &job, 60) == EXIT_SUCCESS);
        }
        saved = job;
        /* A locally valid completion, independently of the preceding history. */
        saved.state = FAULTLINE_JOB_DONE;
        saved.assigned_at_ms = 20;
        saved.started_at_ms = 30;
        saved.finished_at_ms = saved.updated_at_ms = 70;
        saved.failure = FAULTLINE_JOB_FAILURE_NONE;
        saved.worker_id = 1;
        enum faultline_wal_record_type type = FAULTLINE_WAL_JOB_COMPLETED;
        switch (kind) {
        case 0:
            CHECK(faultline_job_init(&saved, 10, FAULTLINE_TASK_HASH, NULL, 0, 2, 70) == FAULTLINE_JOB_OK);
            type = FAULTLINE_WAL_JOB_CREATED; break;
        case 1: saved.id = 99; break;
        case 2: saved.worker_id = 3; break;
        case 3: saved.attempt = 2; saved.retry_count = 1; break;
        case 4: saved.arguments[0] ^= 1; break;
        case 5: saved.task_type = FAULTLINE_TASK_FIBONACCI; break;
        case 6: saved.max_retries = 3; break;
        case 7: saved.created_at_ms = 11; break;
        case 8: saved.assigned_at_ms = 21; break;
        case 9: saved.started_at_ms = 31; break;
        case 10: saved.started_at_ms = 21; saved.finished_at_ms = saved.updated_at_ms = 25; break;
        case 11: saved = job; saved.started_at_ms = saved.updated_at_ms = 70; type = FAULTLINE_WAL_JOB_STARTED; break;
        case 12:
            saved = job; saved.state = FAULTLINE_JOB_ASSIGNED; saved.worker_id = 3;
            saved.assigned_at_ms = saved.updated_at_ms = 70; saved.started_at_ms = -1;
            type = FAULTLINE_WAL_JOB_ASSIGNED; break;
        case 13: case 14:
            saved = job;
            CHECK(faultline_job_fail(&saved, 1, 1, FAULTLINE_JOB_FAILURE_TASK, 70) == FAULTLINE_JOB_OK);
            if (kind == 13) { saved.attempt = 2; saved.retry_count = 2; }
            else { saved.max_retries = 3; }
            type = FAULTLINE_WAL_JOB_REQUEUED; break;
        case 15:
            saved = job; saved.max_retries = 0;
            CHECK(faultline_job_fail(&saved, 1, 1, FAULTLINE_JOB_FAILURE_TASK, 70) == FAULTLINE_JOB_OK);
            type = FAULTLINE_WAL_JOB_FAILED; break;
        case 19: saved.retry_count = 0; saved.attempt = 1; break;
        case 20: case 21:
            CHECK(add_worker(f, kind == 20 ? 3u : 2u) == EXIT_SUCCESS); break;
        case 22:
            saved = job;
            CHECK(faultline_job_assign(&saved, 2, 70) == FAULTLINE_JOB_OK);
            type = FAULTLINE_WAL_JOB_ASSIGNED; break;
        case 23: case 24:
            CHECK(new_job(f, &other, 20, 0, FAULTLINE_TASK_SLEEP, 40) == EXIT_SUCCESS);
            saved = other;
            CHECK(faultline_job_assign(&saved, 1, 70) == FAULTLINE_JOB_OK);
            type = FAULTLINE_WAL_JOB_ASSIGNED; break;
        default: break;
        }
        if (kind != 20 && kind != 21) { CHECK(add_job(f, &saved, type) == EXIT_SUCCESS); }
        CHECK(rejected(f, FAULTLINE_WAL_REPLAY_BAD_HISTORY) == EXIT_SUCCESS);
        CHECK(cleanup(f) == EXIT_SUCCESS);
    }
    return EXIT_SUCCESS;
}

static int test_allocations_and_limits(void)
{
    struct fixture *f = setup();
    CHECK(f != NULL);
    for (uint32_t id = 1; id <= 200; ++id) { CHECK(add_worker(f, id) == EXIT_SUCCESS); }
    for (uint32_t id = 1000; id <= 1400; id += 2) { CHECK(add_worker(f, id) == EXIT_SUCCESS); }
    struct faultline_job job;
    CHECK(new_job(f, &job, UINT64_MAX, 0, FAULTLINE_TASK_HASH, INT64_MAX) == EXIT_SUCCESS);
    CHECK(assign(f, &job, 1, INT64_MAX) == EXIT_SUCCESS); /* Very old allocation still belongs to the history. */
    CHECK(add_worker(f, UINT32_MAX) == EXIT_SUCCESS && save(f, f->size) == EXIT_SUCCESS);
    struct faultline_wal_writer writer = FAULTLINE_WAL_WRITER_INIT;
    struct faultline_wal_replay_report report;
    CHECK(faultline_wal_replay_open(&writer, f->path, f->state, &report) == FAULTLINE_WAL_REPLAY_OK);
    CHECK(f->state->highest_worker_id == UINT32_MAX && f->state->highest_job_id == UINT64_MAX);
    CHECK(f->state->scheduler.next_job_id == 0 && f->state->workers.next_worker_id == 0);
    CHECK(f->state->job_time_base_ms == INT64_MAX && empty_registry(&f->state->workers) == EXIT_SUCCESS);
    uint32_t worker = 99;
    uint64_t id = 99;
    struct faultline_job_submit_payload submit = {.task_type = FAULTLINE_TASK_SLEEP};
    CHECK(faultline_worker_register(&f->state->workers, 4, 0, &worker) == FAULTLINE_REGISTRY_ID_EXHAUSTED && worker == 99);
    CHECK(faultline_scheduler_submit(&f->state->scheduler, &submit, INT64_MAX, &id) == FAULTLINE_SCHEDULER_ID_EXHAUSTED && id == 99);
    CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_OK);
    CHECK(cleanup(f) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static int test_capacity(void)
{
    struct fixture *f = setup();
    CHECK(f != NULL && add_worker(f, 1) == EXIT_SUCCESS);
    for (uint64_t id = 1; id <= FAULTLINE_JOB_STORE_CAPACITY; ++id) {
        struct faultline_job job;
        CHECK(new_job(f, &job, id, 0, FAULTLINE_TASK_SLEEP, 0) == EXIT_SUCCESS);
        CHECK(assign(f, &job, 1, 0) == EXIT_SUCCESS && fail_job(f, &job, FAULTLINE_JOB_FAILURE_TASK, 0) == EXIT_SUCCESS);
    }
    CHECK(save(f, f->size) == EXIT_SUCCESS);
    struct faultline_wal_writer writer = FAULTLINE_WAL_WRITER_INIT;
    struct faultline_wal_replay_report report;
    CHECK(faultline_wal_replay_open(&writer, f->path, f->state, &report) == FAULTLINE_WAL_REPLAY_OK);
    CHECK(f->state->scheduler.count == FAULTLINE_JOB_STORE_CAPACITY && f->state->scheduler.pending.count == 0);
    CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_OK);
    struct faultline_job job;
    CHECK(new_job(f, &job, FAULTLINE_JOB_STORE_CAPACITY + 1, 0, FAULTLINE_TASK_SLEEP, 0) == EXIT_SUCCESS);
    f->bytes[f->size++] = 0x46; /* Capacity failure must precede any trailing repair. */
    CHECK(rejected(f, FAULTLINE_WAL_REPLAY_CAPACITY) == EXIT_SUCCESS);
    CHECK(cleanup(f) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

struct faults {
    struct faultline_wal_writer *writer;
    struct faultline_wal_replay_state *output;
    size_t read_bytes, max_read, fail_read_at;
    int fail_read, read_interrupts, truncate_interrupts, truncate_error, fail_sync;
    unsigned reads, writes, truncates, syncs;
    int bad_publication;
};

static ssize_t fault_read(void *context, int fd, void *bytes, size_t size)
{
    struct faults *f = context;
    ++f->reads;
    if (f->read_interrupts > 0) { --f->read_interrupts; errno = EINTR; return -1; }
    if (f->fail_read && f->read_bytes >= f->fail_read_at) { errno = EIO; return -1; }
    if (f->fail_read && size > f->fail_read_at - f->read_bytes) { size = f->fail_read_at - f->read_bytes; }
    if (f->max_read != 0 && size > f->max_read) { size = f->max_read; }
    ssize_t count = read(fd, bytes, size);
    if (count > 0) { f->read_bytes += (size_t)count; }
    return count;
}

static ssize_t fault_write(void *context, int fd, const void *bytes, size_t size)
{
    struct faults *f = context;
    ++f->writes;
    return write(fd, bytes, size);
}

static void observe_unpublished(struct faults *f)
{
    struct faultline_wal_record record = {.type = FAULTLINE_WAL_WORKER_ID_ALLOCATED, .sequence = 1, .payload.worker_id = 1};
    if (!untouched(f->output) || f->writer->state != FAULTLINE_WAL_WRITER_RECOVERING ||
        f->writer->next_sequence != 0 || f->writer->synced_sequence != 0 ||
        faultline_wal_writer_append(f->writer, &record) != FAULTLINE_WAL_WRITE_INVALID_STATE) {
        f->bad_publication = 1;
    }
}

static int fault_sync(void *context, int fd)
{
    struct faults *f = context;
    ++f->syncs;
    observe_unpublished(f);
    if (f->fail_sync > 0 && f->syncs == (unsigned)f->fail_sync) { errno = EIO; return -1; }
    return fsync(fd);
}

static int fault_truncate(void *context, int fd, off_t length)
{
    struct faults *f = context;
    ++f->truncates;
    observe_unpublished(f);
    if (f->truncate_interrupts > 0) { --f->truncate_interrupts; errno = EINTR; return -1; }
    if (f->truncate_error != 0) { errno = f->truncate_error; return -1; }
    return ftruncate(fd, length);
}

static struct faultline_wal_writer_io fault_io(void)
{
    struct faultline_wal_writer_io io = *faultline_wal_writer_system_io();
    io.read = fault_read; io.write = fault_write; io.sync = fault_sync; io.truncate = fault_truncate;
    return io;
}

static int test_short_reads(void)
{
    struct fixture *f = setup();
    CHECK(f != NULL && add_worker(f, 1) == EXIT_SUCCESS && add_worker(f, 5) == EXIT_SUCCESS);
    size_t prefix = f->size;
    CHECK(add_worker(f, 6) == EXIT_SUCCESS && save(f, prefix + 34) == EXIT_SUCCESS);
    struct faultline_wal_writer writer = FAULTLINE_WAL_WRITER_INIT;
    struct faultline_wal_replay_report report;
    struct faults faults = {.writer = &writer, .output = f->state, .max_read = 1, .read_interrupts = 3, .truncate_interrupts = 2};
    struct faultline_wal_writer_io io = fault_io();
    memset(f->state, 0xa5, sizeof(*f->state));
    CHECK(faultline_wal_replay_open_with_io(&writer, f->path, f->state, &report, &io, &faults) == FAULTLINE_WAL_REPLAY_OK);
    CHECK(report.valid_bytes == prefix && report.tail_bytes == 34 && report.last_sequence == 2);
    CHECK(f->state->highest_worker_id == 5 && writer.next_sequence == 3);
    CHECK(!faults.bad_publication && faults.truncates == 3 && faults.syncs == 2 && faults.writes == 0);
    CHECK(faults.read_bytes == prefix + 34 && faults.reads > faults.read_bytes);
    CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_OK && file_matches(f, prefix) == EXIT_SUCCESS);
    CHECK(cleanup(f) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static int test_io_failures(void)
{
    const size_t positions[] = {0, 1, 23, 24, 25, 55, 56, 59, 60};
    for (size_t scenario = 0; scenario < 14; ++scenario) {
        struct fixture *f = setup();
        CHECK(f != NULL && add_worker(f, 1) == EXIT_SUCCESS);
        size_t prefix = f->size;
        struct faultline_wal_writer writer = FAULTLINE_WAL_WRITER_INIT;
        struct faultline_wal_replay_report report;
        struct faults faults = {.writer = &writer, .output = f->state};
        if (scenario < 9) { faults.fail_read = 1; faults.fail_read_at = positions[scenario]; }
        else {
            f->bytes[f->size++] = 0x46;
            if (scenario == 9) { faults.truncate_error = EIO; }
            else { faults.fail_sync = scenario == 10 || scenario == 12 ? 1 : 2; }
            if (scenario >= 12) { f->size = prefix; } /* Sync is required even without a tail. */
        }
        CHECK(save(f, f->size) == EXIT_SUCCESS);
        struct faultline_wal_writer_io io = fault_io();
        memset(f->state, 0xa5, sizeof(*f->state));
        CHECK(faultline_wal_replay_open_with_io(&writer, f->path, f->state, &report, &io, &faults) == FAULTLINE_WAL_REPLAY_IO_ERROR);
        CHECK(untouched(f->state) && !faults.bad_publication && writer.state == FAULTLINE_WAL_WRITER_FAILED);
        CHECK(writer.system_error == EIO && writer.next_sequence == 0 && writer.synced_sequence == 0 && faults.writes == 0);
        enum faultline_wal_writer_operation expected = scenario < 9 ? FAULTLINE_WAL_IO_READ_FILE :
            scenario == 9 ? FAULTLINE_WAL_IO_TRUNCATE_TAIL :
            scenario == 10 || scenario == 12 ? FAULTLINE_WAL_IO_SYNC_RECOVERY : FAULTLINE_WAL_IO_SYNC_DIRECTORY;
        CHECK(writer.failed_operation == expected);
        if (scenario < 9) { CHECK(faults.truncates == 0 && faults.syncs == 0); }
        unsigned syncs = faults.syncs, truncates = faults.truncates;
        struct faultline_wal_record record = {.type = FAULTLINE_WAL_WORKER_ID_ALLOCATED, .sequence = 2, .payload.worker_id = 2};
        CHECK(faultline_wal_writer_append(&writer, &record) == FAULTLINE_WAL_WRITE_IO_ERROR);
        CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_IO_ERROR);
        CHECK(writer.failed_operation == expected && writer.system_error == EIO && faults.writes == 0);
        CHECK(faults.syncs == syncs && faults.truncates == truncates);
        CHECK(file_matches(f, scenario == 10 || scenario == 11 ? prefix : f->size) == EXIT_SUCCESS);
        CHECK(cleanup(f) == EXIT_SUCCESS);
    }
    return EXIT_SUCCESS;
}

static int test_open_guards(void)
{
    struct fixture *f = setup();
    CHECK(f != NULL);
    struct faultline_wal_writer writer = FAULTLINE_WAL_WRITER_INIT;
    struct faultline_wal_replay_report report;
    CHECK(faultline_wal_replay_open(NULL, f->path, f->state, &report) == FAULTLINE_WAL_REPLAY_INVALID_ARGUMENT);
    CHECK(faultline_wal_replay_open(&writer, NULL, f->state, &report) == FAULTLINE_WAL_REPLAY_INVALID_ARGUMENT);
    CHECK(faultline_wal_replay_open(&writer, "", f->state, &report) == FAULTLINE_WAL_REPLAY_INVALID_ARGUMENT);
    CHECK(faultline_wal_replay_open(&writer, "/tmp/", f->state, &report) == FAULTLINE_WAL_REPLAY_INVALID_ARGUMENT);
    CHECK(faultline_wal_replay_open(&writer, f->path, NULL, &report) == FAULTLINE_WAL_REPLAY_INVALID_ARGUMENT);
    CHECK(faultline_wal_replay_open(&writer, f->path, f->state, NULL) == FAULTLINE_WAL_REPLAY_INVALID_ARGUMENT);
    CHECK(faultline_wal_replay_open(&writer, f->path, f->state, &report) == FAULTLINE_WAL_REPLAY_IO_ERROR);
    CHECK(writer.system_error == ENOENT && writer.failed_operation == FAULTLINE_WAL_IO_OPEN_EXISTING);
    CHECK(access(f->path, F_OK) < 0 && errno == ENOENT);
    CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_IO_ERROR);
    for (int kind = 0; kind < 3; ++kind) {
        if (kind == 0) { CHECK(symlink("missing-target", f->path) == 0); }
        if (kind == 1) { CHECK(mkfifo(f->path, 0600) == 0); }
        if (kind == 2) { CHECK(mkdir(f->path, 0700) == 0); }
        struct faultline_wal_writer other = FAULTLINE_WAL_WRITER_INIT;
        CHECK(faultline_wal_replay_open(&other, f->path, f->state, &report) == FAULTLINE_WAL_REPLAY_IO_ERROR);
        CHECK(faultline_wal_writer_close(&other) == FAULTLINE_WAL_WRITE_IO_ERROR);
        if (kind == 2) { CHECK(rmdir(f->path) == 0); } else { CHECK(unlink(f->path) == 0); }
    }
    CHECK(save(f, f->size) == EXIT_SUCCESS);
    struct faultline_wal_writer first = FAULTLINE_WAL_WRITER_INIT, second = FAULTLINE_WAL_WRITER_INIT;
    CHECK(faultline_wal_replay_open(&first, f->path, f->state, &report) == FAULTLINE_WAL_REPLAY_OK);
    CHECK(faultline_wal_replay_open(&first, f->path, f->state, &report) == FAULTLINE_WAL_REPLAY_INVALID_STATE);
    CHECK(faultline_wal_replay_open(&second, f->path, f->state, &report) == FAULTLINE_WAL_REPLAY_IO_ERROR);
    CHECK(second.failed_operation == FAULTLINE_WAL_IO_LOCK && (second.system_error == EWOULDBLOCK || second.system_error == EAGAIN));
    CHECK(faultline_wal_writer_close(&second) == FAULTLINE_WAL_WRITE_IO_ERROR);
    CHECK(faultline_wal_writer_close(&first) == FAULTLINE_WAL_WRITE_OK);
    CHECK(file_matches(f, f->size) == EXIT_SUCCESS && cleanup(f) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

struct crash_context { int ready; int crash; };

static ssize_t crash_write(void *context, int fd, const void *bytes, size_t size)
{
    struct crash_context *crash = context;
    if (!crash->crash) { return write(fd, bytes, size); }
    if (size < 7 || write(fd, bytes, 7) != 7 || write(crash->ready, "Y", 1) != 1) { _exit(EXIT_FAILURE); }
    for (;;) { pause(); }
}

static int test_crash_tail(void)
{
    struct fixture *f = setup();
    int pipe_fds[2];
    CHECK(f != NULL && pipe(pipe_fds) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        (void)close(pipe_fds[0]);
        struct crash_context context = {.ready = pipe_fds[1]};
        struct faultline_wal_writer_io io = *faultline_wal_writer_system_io();
        io.write = crash_write;
        struct faultline_wal_writer writer = FAULTLINE_WAL_WRITER_INIT;
        struct faultline_wal_record record = {.type = FAULTLINE_WAL_WORKER_ID_ALLOCATED, .sequence = 1, .payload.worker_id = 1};
        if (faultline_wal_writer_create_with_io(&writer, f->path, &io, &context) != FAULTLINE_WAL_WRITE_OK ||
            faultline_wal_writer_append(&writer, &record) != FAULTLINE_WAL_WRITE_OK) { _exit(EXIT_FAILURE); }
        record.sequence = 2; record.payload.worker_id = 2; context.crash = 1;
        (void)faultline_wal_writer_append(&writer, &record);
        _exit(EXIT_FAILURE);
    }
    CHECK(close(pipe_fds[1]) == 0);
    struct pollfd poll_fd = {.fd = pipe_fds[0], .events = POLLIN};
    int polled;
    do { polled = poll(&poll_fd, 1, 5000); } while (polled < 0 && errno == EINTR);
    char ready = 0;
    ssize_t received = polled > 0 ? read(pipe_fds[0], &ready, 1) : -1;
    int killed = kill(child, SIGKILL), status;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    CHECK(close(pipe_fds[0]) == 0 && waited == child && killed == 0);
    CHECK(received == 1 && ready == 'Y' && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    struct faultline_wal_writer writer = FAULTLINE_WAL_WRITER_INIT;
    struct faultline_wal_replay_report report;
    CHECK(faultline_wal_replay_open(&writer, f->path, f->state, &report) == FAULTLINE_WAL_REPLAY_OK);
    CHECK(report.valid_bytes == 60 && report.tail_bytes == 7 && report.last_sequence == 1);
    CHECK(f->state->highest_worker_id == 1 && writer.next_sequence == 2);
    struct faultline_wal_record record = {.type = FAULTLINE_WAL_WORKER_ID_ALLOCATED, .sequence = 2, .payload.worker_id = 2};
    CHECK(faultline_wal_writer_append(&writer, &record) == FAULTLINE_WAL_WRITE_OK);
    CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_OK);
    CHECK(faultline_wal_replay_open(&writer, f->path, f->state, &report) == FAULTLINE_WAL_REPLAY_OK);
    CHECK(report.tail_bytes == 0 && report.last_sequence == 2 && f->state->highest_worker_id == 2);
    CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_OK && cleanup(f) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

int main(void)
{
    const struct {const char *name; int (*run)(void);} tests[] = {
        {"empty log, allocation replay, and append resumption", test_empty_and_resume},
        {"mixed jobs, results, retries, FIFO, clocks, and empty live registry", test_mixed_history},
        {"every incomplete prefix of a maximum record and safe resumed appends", test_incomplete_tails},
        {"missing, incomplete, and invalid file headers are never repaired", test_file_headers},
        {"complete corruption and invalid headers refuse recovery without truncation", test_corrupt_records},
        {"checksummed but impossible histories preserve caller state and file", test_bad_histories},
        {"sparse historical allocations, old worker IDs, and exhausted counters", test_allocations_and_limits},
        {"retained terminal jobs enforce store capacity", test_capacity},
        {"short reads, EINTR, and publication after repair and synchronization", test_short_reads},
        {"read, truncate, and sync errors never publish recovery state", test_io_failures},
        {"invalid arguments, existing-file policy, special files, and exclusive locking", test_open_guards},
        {"SIGKILL during append, tail repair, and subsequent replay", test_crash_tail}
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (tests[i].run() != EXIT_SUCCESS) { return EXIT_FAILURE; }
        printf("PASS: %s\n", tests[i].name);
    }
    return EXIT_SUCCESS;
}
