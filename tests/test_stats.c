#include "coordinator_stats.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return EXIT_FAILURE; } } while (0)

static const uint8_t literal[] = {
    0x46, 0x4c, 0x49, 0x4e, 0, 1, 0, 20, 0, 0, 0, 192,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x17, 0x70,
};

static struct faultline_message sample(void)
{
    return (struct faultline_message){.message_type = FAULTLINE_MSG_STATS_RESPONSE, .payload.stats = {
        .jobs_submitted_total = 5,
        .jobs_queued = 1,
        .jobs_assigned = 1,
        .jobs_running = 1,
        .jobs_completed_total = 1,
        .jobs_failed_total = 1,
        .job_attempts_total = 4,
        .job_retries_total = 0,
        .completed_latency_avg_ms = UINT64_C(0x01020304),
        .workers_retained = 4,
        .workers_alive = 2,
        .workers_expired = 1,
        .workers_dead = 1,
        .workers_busy = 1,
        .workers_idle = 1,
        .session_uptime_ms = UINT64_C(0x0102030405060708),
        .session_jobs_submitted = 3,
        .session_jobs_completed = 1,
        .session_jobs_failed = 0,
        .session_job_retries = 0,
        .startup_jobs_recovered = 2,
        .startup_interrupted_jobs = 1,
        .startup_duration_ms = 9,
        .heartbeat_timeout_ms = 6000,
    }};
}

static int decode_error(const uint8_t *bytes, size_t length, enum faultline_protocol_result expected)
{
    uint8_t *exact = malloc(length == 0 ? 1 : length);
    CHECK(exact != NULL);
    memcpy(exact, bytes, length);
    struct faultline_message out, before;
    memset(&out, 0xa5, sizeof(out)); memcpy(&before, &out, sizeof(out));
    size_t consumed = 999;
    enum faultline_protocol_result result = faultline_message_decode(exact, length, &out, &consumed);
    free(exact);
    CHECK(result == expected && consumed == 999 && memcmp(&out, &before, sizeof(out)) == 0);
    return 0;
}

static int test_literal_and_empty(void)
{
    struct faultline_message message = sample(), decoded;
    _Alignas(uint64_t) uint8_t wire[206];
    size_t written = 0, consumed = 0;
    memset(wire, 0xa5, sizeof(wire));
    CHECK(faultline_message_encode(wire + 1, 204, &message, &written) == FAULTLINE_PROTOCOL_OK);
    CHECK(written == 204 && memcmp(wire + 1, literal, 204) == 0);
    CHECK(wire[0] == 0xa5 && wire[205] == 0xa5);
    CHECK(faultline_message_decode(wire + 1, 204, &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
    memset(wire, 0, sizeof(wire));
    CHECK(consumed == 204 && decoded.payload.stats.session_uptime_ms == UINT64_C(0x0102030405060708));
    CHECK(faultline_message_encode(wire, sizeof(wire), &decoded, &written) == FAULTLINE_PROTOCOL_OK);
    CHECK(memcmp(wire, literal, 204) == 0);
    message = (struct faultline_message){.message_type = FAULTLINE_MSG_STATS_REQUEST};
    const uint8_t request[] = {0x46, 0x4c, 0x49, 0x4e, 0, 1, 0, 19, 0, 0, 0, 0};
    CHECK(faultline_message_encode(wire, sizeof(wire), &message, &written) == FAULTLINE_PROTOCOL_OK);
    CHECK(written == 12 && memcmp(wire, request, 12) == 0);
    CHECK(faultline_message_decode(request, sizeof(request), &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
    CHECK(consumed == 12 && decoded.message_type == 19 && decoded.payload.worker_id == 0);
    message = (struct faultline_message){.message_type = 20, .payload.stats.heartbeat_timeout_ms = 1};
    CHECK(faultline_message_encode(wire, sizeof(wire), &message, &written) == FAULTLINE_PROTOCOL_OK);
    return 0;
}

static int test_partial_lengths_and_streams(void)
{
    struct faultline_message message = sample(), out;
    uint8_t wire[216], before[216];
    size_t n;
    for (size_t cut = 0; cut < sizeof(literal); ++cut) {
        CHECK(decode_error(literal, cut, FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL) == 0);
        memset(wire, 0xa5, sizeof(wire)); memcpy(before, wire, sizeof(wire)); n = 999;
        CHECK(faultline_message_encode(wire, cut, &message, &n) == FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL);
        CHECK(n == 999 && memcmp(before, wire, sizeof(wire)) == 0);
    }
    memcpy(wire, literal, 204);
    for (unsigned int size = 0; size <= 255; ++size) {
        if (size == 192) { continue; }
        wire[11] = (uint8_t)size;
        CHECK(decode_error(wire, 12, FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH) == 0);
    }
    memcpy(wire, literal, 204);
    const uint8_t request[] = {0x46, 0x4c, 0x49, 0x4e, 0, 1, 0, 19, 0, 0, 0, 0};
    memcpy(wire + 204, request, 12);
    CHECK(faultline_message_decode(wire, sizeof(wire), &out, &n) == FAULTLINE_PROTOCOL_OK && n == 204);
    CHECK(faultline_message_decode(wire + n, 12, &out, &n) == FAULTLINE_PROTOCOL_OK && n == 12);
    wire[7] = 19; wire[11] = 1;
    CHECK(decode_error(wire, 12, FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH) == 0);
    CHECK(faultline_message_decode(NULL, 204, &out, &n) == FAULTLINE_PROTOCOL_INVALID_ARGUMENT);
    CHECK(faultline_message_decode(literal, 204, NULL, &n) == FAULTLINE_PROTOCOL_INVALID_ARGUMENT);
    CHECK(faultline_message_decode(literal, 204, &out, NULL) == FAULTLINE_PROTOCOL_INVALID_ARGUMENT);
    CHECK(faultline_message_encode(NULL, 204, &message, &n) == FAULTLINE_PROTOCOL_INVALID_ARGUMENT);
    CHECK(faultline_message_encode(wire, 204, NULL, &n) == FAULTLINE_PROTOCOL_INVALID_ARGUMENT);
    CHECK(faultline_message_encode(wire, 204, &message, NULL) == FAULTLINE_PROTOCOL_INVALID_ARGUMENT);
    return 0;
}

static int test_invalid_counters(void)
{
    struct { size_t offset, index; uint64_t value; } cases[] = {
#define BAD(field, index, value) {offsetof(struct faultline_stats_payload, field), index, value}
        BAD(jobs_submitted_total, 0, UINT64_MAX), BAD(jobs_queued, 1, 0),
        BAD(jobs_assigned, 2, UINT64_MAX), BAD(jobs_running, 3, UINT64_MAX),
        BAD(jobs_completed_total, 4, UINT64_MAX), BAD(jobs_failed_total, 5, UINT64_MAX),
        BAD(job_attempts_total, 6, 5), BAD(job_retries_total, 7, UINT64_MAX),
        BAD(completed_latency_avg_ms, 8, UINT64_MAX), BAD(workers_retained, 9, 65),
        BAD(workers_alive, 10, 3), BAD(workers_expired, 11, UINT64_MAX),
        BAD(workers_dead, 12, 2), BAD(workers_busy, 13, 0), BAD(workers_idle, 14, UINT64_MAX),
        BAD(session_uptime_ms, 15, UINT64_MAX), BAD(session_jobs_submitted, 16, 4),
        BAD(session_jobs_completed, 17, 2), BAD(session_jobs_failed, 18, 2),
        BAD(session_job_retries, 19, 1), BAD(startup_jobs_recovered, 20, 6),
        BAD(startup_interrupted_jobs, 21, 3), BAD(startup_duration_ms, 22, UINT64_MAX),
        BAD(heartbeat_timeout_ms, 23, 0), BAD(heartbeat_timeout_ms, 23, (uint64_t)INT_MAX + 1)
#undef BAD
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct faultline_message message = sample();
        memcpy((uint8_t *)&message.payload.stats + cases[i].offset, &cases[i].value, 8);
        uint8_t wire[204], before[204]; size_t n = 999;
        memset(wire, 0xa5, sizeof(wire)); memcpy(before, wire, sizeof(wire));
        CHECK(faultline_message_encode(wire, sizeof(wire), &message, &n) == FAULTLINE_PROTOCOL_INVALID_STATS);
        CHECK(n == 999 && memcmp(before, wire, sizeof(wire)) == 0);
        memcpy(wire, literal, 204);
        for (size_t byte = 0; byte < 8; ++byte) {
            wire[12 + cases[i].index * 8 + byte] = (uint8_t)(cases[i].value >> (56 - byte * 8));
        }
        CHECK(decode_error(wire, 204, FAULTLINE_PROTOCOL_INVALID_STATS) == 0);
    }
    return 0;
}

static struct faultline_coordinator_store *new_store(void)
{
    struct faultline_coordinator_store *store = calloc(1, sizeof(*store));
    if (store == NULL) { abort(); }
    store->opened = 1; store->session_start_ms = 100;
    return store;
}

static struct faultline_job job(uint64_t id, enum faultline_job_state state, uint32_t retries, int64_t finished)
{
    return (struct faultline_job){.id = id, .task_type = FAULTLINE_TASK_HASH, .state = state,
        .worker_id = state == FAULTLINE_JOB_QUEUED ? 0 : (uint32_t)id,
        .attempt = (uint64_t)retries + (state == FAULTLINE_JOB_QUEUED ? 0u : 1u),
        .retry_count = retries, .max_retries = retries, .created_at_ms = 10, .finished_at_ms = finished};
}

static int test_durable_and_session_scopes(void)
{
    struct faultline_coordinator_store *store = new_store(), *before = malloc(sizeof(*before));
    CHECK(before != NULL);
    store->jobs.count = 3; store->interrupted_jobs = 1;
    store->jobs.jobs[0] = job(1, FAULTLINE_JOB_DONE, 0, 30);
    store->jobs.jobs[1] = job(2, FAULTLINE_JOB_FAILED, 0, 40);
    store->jobs.jobs[2] = job(3, FAULTLINE_JOB_QUEUED, 2, -1);
    struct faultline_stats_session session;
    struct faultline_stats_payload s;
    CHECK(faultline_stats_begin(&session, store, 200) == 0);
    CHECK(faultline_stats_snapshot(&session, store, 200, 6000, &s) == 0);
    CHECK(s.job_retries_total == 2 && s.session_job_retries == 0 && s.session_jobs_completed == 0);
    store->jobs.count = 5;
    store->jobs.jobs[2] = job(3, FAULTLINE_JOB_DONE, 2, 81);
    store->jobs.jobs[3] = job(4, FAULTLINE_JOB_FAILED, 1, 100);
    store->jobs.jobs[4] = job(5, FAULTLINE_JOB_QUEUED, 0, -1);
    memcpy(before, store, sizeof(*store));
    CHECK(faultline_stats_snapshot(&session, store, 1200, 6000, &s) == 0);
    CHECK(memcmp(before, store, sizeof(*store)) == 0);
    CHECK(s.jobs_submitted_total == 5 && s.jobs_completed_total == 2 && s.jobs_failed_total == 2 && s.jobs_queued == 1);
    CHECK(s.job_attempts_total == 7 && s.job_retries_total == 3 && s.completed_latency_avg_ms == 45);
    CHECK(s.session_uptime_ms == 1000 && s.session_jobs_submitted == 2 &&
          s.session_jobs_completed == 1 && s.session_jobs_failed == 1 && s.session_job_retries == 1);
    CHECK(s.startup_jobs_recovered == 3 && s.startup_interrupted_jobs == 1 && s.startup_duration_ms == 100);
    free(before); free(store); return 0;
}

static int test_maximum_and_latency_overflow(void)
{
    struct faultline_coordinator_store *store = new_store();
    store->jobs.count = FAULTLINE_JOB_STORE_CAPACITY;
    for (size_t i = 0; i < store->jobs.count; ++i) {
        store->jobs.jobs[i] = job(i + 1, FAULTLINE_JOB_DONE, UINT32_MAX, INT64_MAX - (int64_t)(i % 2));
        store->jobs.jobs[i].created_at_ms = 0;
    }
    struct faultline_stats_session session;
    struct faultline_message reply = {.message_type = 20};
    CHECK(faultline_stats_begin(&session, store, 200) == 0);
    CHECK(faultline_stats_snapshot(&session, store, INT64_MAX, INT_MAX, &reply.payload.stats) == 0);
    CHECK(reply.payload.stats.completed_latency_avg_ms == (uint64_t)INT64_MAX - 1);
    CHECK(reply.payload.stats.job_attempts_total == UINT64_C(256) * (UINT64_C(1) << 32));
    CHECK(reply.payload.stats.job_retries_total == UINT64_C(256) * UINT32_MAX);
    uint8_t wire[204]; size_t n;
    CHECK(faultline_message_encode(wire, sizeof(wire), &reply, &n) == FAULTLINE_PROTOCOL_OK);
    free(store); return 0;
}

static int test_liveness_and_activity(void)
{
    struct faultline_coordinator_store *store = new_store();
    store->jobs.count = 2;
    store->jobs.jobs[0] = job(1, FAULTLINE_JOB_ASSIGNED, 0, -1);
    store->jobs.jobs[1] = job(2, FAULTLINE_JOB_RUNNING, 0, -1);
    store->jobs.jobs[1].worker_id = 3;
    store->workers.workers[0] = (struct faultline_worker){1, 11, FAULTLINE_WORKER_ALIVE, 101};
    store->workers.workers[1] = (struct faultline_worker){2, 12, FAULTLINE_WORKER_ALIVE, 101};
    store->workers.workers[2] = (struct faultline_worker){3, 13, FAULTLINE_WORKER_ALIVE, 100};
    store->workers.workers[3] = (struct faultline_worker){4, -1, FAULTLINE_WORKER_DEAD, 100};
    struct faultline_stats_session session; struct faultline_stats_payload s;
    CHECK(faultline_stats_begin(&session, store, 200) == 0);
    CHECK(faultline_stats_snapshot(&session, store, 700, 600, &s) == 0);
    CHECK(s.workers_retained == 4 && s.workers_alive == 2 && s.workers_expired == 1 && s.workers_dead == 1);
    CHECK(s.workers_busy == 1 && s.workers_idle == 1);
    CHECK(store->workers.workers[2].state == FAULTLINE_WORKER_ALIVE);
    CHECK(faultline_stats_snapshot(&session, store, 701, 600, &s) == 0);
    CHECK(s.workers_expired == 3 && s.workers_busy == 0 && s.workers_idle == 0);
    free(store); return 0;
}

static int test_invalid_snapshot_inputs(void)
{
    struct faultline_coordinator_store *store = new_store();
    struct faultline_stats_session session = {0}, before = session;
    struct faultline_stats_payload out, saved;
    memset(&out, 0xa5, sizeof(out)); memcpy(&saved, &out, sizeof(out));
    CHECK(faultline_stats_begin(&session, store, 99) == -1 && memcmp(&session, &before, sizeof(session)) == 0);
    CHECK(faultline_stats_begin(&session, store, 200) == 0);
    CHECK(faultline_stats_snapshot(&session, store, 199, 6000, &out) == -1);
    CHECK(faultline_stats_snapshot(&session, store, 200, 0, &out) == -1);
    CHECK(faultline_stats_snapshot(NULL, store, 200, 6000, &out) == -1);
    CHECK(faultline_stats_snapshot(&session, NULL, 200, 6000, &out) == -1);
    CHECK(faultline_stats_snapshot(&session, store, 200, 6000, NULL) == -1);
    store->workers.workers[0] = (struct faultline_worker){1, 11, FAULTLINE_WORKER_ALIVE, 201};
    CHECK(faultline_stats_snapshot(&session, store, 200, 6000, &out) == -1);
    store->failure = FAULTLINE_STORE_FAILURE_WAL;
    CHECK(faultline_stats_snapshot(&session, store, 200, 6000, &out) == -1);
    CHECK(memcmp(&out, &saved, sizeof(out)) == 0);
    CHECK(faultline_stats_begin(NULL, store, 200) == -1);
    CHECK(faultline_stats_begin(&session, NULL, 200) == -1);
    free(store); return 0;
}

int main(void)
{
    int (*tests[])(void) = {test_literal_and_empty, test_partial_lengths_and_streams, test_invalid_counters,
        test_durable_and_session_scopes, test_maximum_and_latency_overflow, test_liveness_and_activity,
        test_invalid_snapshot_inputs};
    const char *names[] = {"stats exact bytes and empty counts", "stats partial frames and streams",
        "stats invalid counters", "stats durable and session scopes", "stats maximum counts and mean overflow",
        "stats liveness and activity", "stats invalid snapshot inputs"};
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (tests[i]() != 0) { return EXIT_FAILURE; }
        printf("PASS: %s\n", names[i]);
    }
    return EXIT_SUCCESS;
}
