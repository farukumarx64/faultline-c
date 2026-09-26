#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return EXIT_FAILURE; } } while (0)
#define HEADER(type, length) 0x46, 0x4c, 0x49, 0x4e, 0, 1, 0, type, 0, 0, 0, length

static const uint8_t jobs_wire[] = {
    HEADER(16, 42), 0, 0, 0, 1,
    1, 2, 3, 4, 5, 6, 7, 8, 0, 4, 0, 4, 0x11, 0x22, 0x33, 0x44,
    0, 0, 0, 0, 1, 2, 3, 5, 1, 2, 3, 4, 0x11, 0x22, 0x33, 0x44,
    0, 0, 0, 0, 1, 0
};
static const uint8_t workers_wire[] = {
    HEADER(18, 38), 0, 0, 0, 1, 0, 0, 0x17, 0x70,
    0x11, 0x22, 0x33, 0x44, 0, 1, 1, 2, 3, 4, 5, 6, 7, 8,
    8, 7, 6, 5, 4, 3, 2, 1, 0, 0, 0, 1, 0, 0, 0, 0
};

static struct faultline_message sample(int jobs)
{
    struct faultline_message message = {0};
    if (jobs) {
        message.message_type = FAULTLINE_MSG_JOBS_RESPONSE;
        message.payload.jobs.count = 1;
        message.payload.jobs.entries[0] = (struct faultline_job_summary){
            .job_id = UINT64_C(0x0102030405060708), .task_type = 4, .state = 4,
            .worker_id = UINT32_C(0x11223344), .attempt = UINT64_C(0x01020305),
            .retry_count = UINT32_C(0x01020304), .max_retries = UINT32_C(0x11223344), .result_size = 256
        };
    } else {
        message.message_type = FAULTLINE_MSG_WORKERS_RESPONSE;
        message.payload.workers.count = 1;
        message.payload.workers.heartbeat_timeout_ms = 6000;
        message.payload.workers.entries[0] = (struct faultline_worker_summary){
            .worker_id = UINT32_C(0x11223344), .state = 1,
            .heartbeat_age_ms = UINT64_C(0x0102030405060708),
            .job_id = UINT64_C(0x0807060504030201), .attempt = UINT64_C(0x100000000)
        };
    }
    return message;
}

static int decode_error(const uint8_t *wire, size_t size, enum faultline_protocol_result expected)
{
    uint8_t *exact = malloc(size == 0 ? 1 : size);
    CHECK(exact != NULL);
    memcpy(exact, wire, size);
    struct faultline_message message;
    uint8_t before[sizeof(message)];
    memset(&message, 0xa5, sizeof(message)); memcpy(before, &message, sizeof(message));
    size_t consumed = 99;
    enum faultline_protocol_result result = faultline_message_decode(exact, size, &message, &consumed);
    free(exact);
    CHECK(result == expected && consumed == 99 && memcmp(before, &message, sizeof(message)) == 0);
    return EXIT_SUCCESS;
}

static int encode_error(const struct faultline_message *message, size_t capacity, enum faultline_protocol_result expected)
{
    uint8_t wire[FAULTLINE_MESSAGE_MAX_FRAME_SIZE], before[sizeof(wire)];
    memset(wire, 0xa5, sizeof(wire)); memcpy(before, wire, sizeof(wire));
    size_t written = 99;
    CHECK(faultline_message_encode(wire, capacity, message, &written) == expected);
    CHECK(written == 99 && memcmp(before, wire, sizeof(wire)) == 0);
    return EXIT_SUCCESS;
}

static void put_be(uint8_t *out, size_t width, uint64_t value)
{
    for (size_t i = 0; i < width; ++i) { out[width - i - 1] = (uint8_t)(value >> (8 * i)); }
}

static int test_literals(void)
{
    for (int jobs = 0; jobs <= 1; ++jobs) {
        struct faultline_message message = sample(jobs), decoded;
        const uint8_t *literal = jobs ? jobs_wire : workers_wire;
        size_t length = jobs ? sizeof(jobs_wire) : sizeof(workers_wire), written, consumed;
        _Alignas(uint64_t) uint8_t wire[57];
        memset(wire, 0xa5, sizeof(wire));
        CHECK(faultline_message_encode(wire + 1, sizeof(wire) - 1, &message, &written) == FAULTLINE_PROTOCOL_OK);
        CHECK(written == length && memcmp(wire + 1, literal, length) == 0);
        CHECK(wire[0] == 0xa5 && wire[length + 1] == 0xa5);
        memcpy(wire + 1, literal, length);
        CHECK(faultline_message_decode(wire + 1, length, &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
        CHECK(consumed == length);
        memset(wire, 0, sizeof(wire)); /* Decoded arrays own their data. */
        CHECK(faultline_message_encode(wire, sizeof(wire), &decoded, &written) == FAULTLINE_PROTOCOL_OK);
        CHECK(written == length && memcmp(wire, literal, length) == 0);
    }
    return EXIT_SUCCESS;
}

static int test_empty(void)
{
    const uint8_t requests[][12] = {{HEADER(15, 0)}, {HEADER(17, 0)}};
    const uint8_t empty_jobs[] = {HEADER(16, 4), 0, 0, 0, 0};
    const uint8_t empty_workers[] = {HEADER(18, 8), 0, 0, 0, 0, 0, 0, 0x17, 0x70};
    for (size_t i = 0; i < 4; ++i) {
        struct faultline_message message = {0}, decoded;
        const uint8_t *literal = i < 2 ? requests[i] : i == 2 ? empty_jobs : empty_workers;
        size_t size = i < 2 ? 12 : i == 2 ? 16 : 20, written, consumed;
        message.message_type = i == 0 ? 15 : i == 1 ? 17 : i == 2 ? 16 : 18;
        if (i == 3) { message.payload.workers.heartbeat_timeout_ms = 6000; }
        uint8_t wire[20];
        CHECK(faultline_message_encode(wire, sizeof(wire), &message, &written) == FAULTLINE_PROTOCOL_OK);
        CHECK(written == size && memcmp(wire, literal, size) == 0);
        CHECK(faultline_message_decode(literal, size, &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
        CHECK(consumed == size && decoded.message_type == message.message_type);
        if (i < 2) {
            message.payload.worker_id = 1;
            CHECK(encode_error(&message, 20, FAULTLINE_PROTOCOL_INVALID_WORKER_ID) == EXIT_SUCCESS);
        }
    }
    return EXIT_SUCCESS;
}

static struct faultline_message maximum(int jobs)
{
    struct faultline_message message = sample(jobs);
    if (jobs) {
        message.payload.jobs.count = FAULTLINE_JOBS_MAX_ENTRIES;
        for (size_t i = 0; i < message.payload.jobs.count; ++i) {
            message.payload.jobs.entries[i] = (struct faultline_job_summary){
                .job_id = UINT64_MAX - FAULTLINE_JOBS_MAX_ENTRIES + i + 1,
                .task_type = (uint16_t)(i % 4 + 1), .state = 4, .worker_id = UINT32_MAX,
                .attempt = UINT64_C(0x100000000), .retry_count = UINT32_MAX,
                .max_retries = UINT32_MAX, .result_size = 1024
            };
        }
    } else {
        message.payload.workers.count = FAULTLINE_WORKERS_MAX_ENTRIES;
        message.payload.workers.heartbeat_timeout_ms = UINT32_MAX;
        for (size_t i = 0; i < message.payload.workers.count; ++i) {
            message.payload.workers.entries[i] = (struct faultline_worker_summary){
                .worker_id = UINT32_MAX - FAULTLINE_WORKERS_MAX_ENTRIES + (uint32_t)i + 1,
                .state = i % 2 == 0 ? 1 : 2, .heartbeat_age_ms = UINT64_MAX
            };
        }
        message.payload.workers.entries[0].job_id = UINT64_MAX;
        message.payload.workers.entries[0].attempt = UINT64_MAX;
    }
    return message;
}

static int test_maximum_and_fragments(void)
{
    for (int jobs = 0; jobs <= 1; ++jobs) {
        struct faultline_message message = maximum(jobs), decoded;
        uint8_t wire[FAULTLINE_MESSAGE_MAX_FRAME_SIZE], again[sizeof(wire)];
        size_t size, consumed, written;
        CHECK(faultline_message_encode(wire, sizeof(wire), &message, &size) == FAULTLINE_PROTOCOL_OK);
        CHECK(size == (jobs ? 9744u : 1940u));
        for (size_t n = 0; n < size; ++n) {
            CHECK(decode_error(wire, n, FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL) == EXIT_SUCCESS);
        }
        const size_t capacities[] = {0, 11, 12, 15, 19, size - 1};
        for (size_t i = 0; i < sizeof(capacities) / sizeof(capacities[0]); ++i) {
            CHECK(encode_error(&message, capacities[i], FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL) == EXIT_SUCCESS);
        }
        CHECK(faultline_message_decode(wire, size, &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
        CHECK(consumed == size);
        CHECK(faultline_message_encode(again, sizeof(again), &decoded, &written) == FAULTLINE_PROTOCOL_OK);
        CHECK(written == size && memcmp(wire, again, size) == 0);
    }
    return EXIT_SUCCESS;
}

static int test_states(void)
{
    for (uint16_t state = 1; state <= 5; ++state) {
        struct faultline_message message = sample(1), decoded;
        message.payload.jobs.entries[0] = (struct faultline_job_summary){
            .job_id = 1, .task_type = 1, .state = state, .worker_id = state == 1 ? 0 : 1,
            .attempt = state == 1 ? 2 : 3, .retry_count = 2, .max_retries = 2,
            .failure = state == 1 || state == 5 ? 2 : 0
        };
        uint8_t wire[54]; size_t size, consumed;
        CHECK(faultline_message_encode(wire, sizeof(wire), &message, &size) == FAULTLINE_PROTOCOL_OK);
        CHECK(faultline_message_decode(wire, size, &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
        CHECK(decoded.payload.jobs.entries[0].state == state);
    }
    return EXIT_SUCCESS;
}

static int test_counts_and_lengths(void)
{
    for (int jobs = 0; jobs <= 1; ++jobs) {
        struct faultline_message message = sample(jobs);
        uint8_t wire[54]; size_t size;
        CHECK(faultline_message_encode(wire, sizeof(wire), &message, &size) == FAULTLINE_PROTOCOL_OK);
        size_t prefix = jobs ? 16 : 20;
        put_be(wire + 12, 4, UINT32_MAX);
        CHECK(decode_error(wire, prefix, FAULTLINE_PROTOCOL_INVALID_LIST_COUNT) == EXIT_SUCCESS);
        put_be(wire + 12, 4, 0);
        CHECK(decode_error(wire, prefix, FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH) == EXIT_SUCCESS);
        put_be(wire + 8, 4, jobs ? 3 : 7);
        CHECK(decode_error(wire, 12, FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH) == EXIT_SUCCESS);
        put_be(wire + 8, 4, jobs ? 9733 : 1929);
        CHECK(decode_error(wire, 12, FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE) == EXIT_SUCCESS);
        if (jobs) { message.payload.jobs.count = SIZE_MAX; }
        else { message.payload.workers.count = SIZE_MAX; }
        CHECK(encode_error(&message, sizeof(wire), FAULTLINE_PROTOCOL_INVALID_LIST_COUNT) == EXIT_SUCCESS);
    }
    uint8_t wire[20] = {HEADER(18, 8)};
    CHECK(decode_error(wire, sizeof(wire), FAULTLINE_PROTOCOL_INVALID_HEARTBEAT_TIMEOUT) == EXIT_SUCCESS);
    struct faultline_message message = sample(0);
    message.payload.workers.heartbeat_timeout_ms = 0;
    CHECK(encode_error(&message, sizeof(wire), FAULTLINE_PROTOCOL_INVALID_HEARTBEAT_TIMEOUT) == EXIT_SUCCESS);
    size_t size = 99;
    CHECK(faultline_message_encode(NULL, 0, &message, &size) == FAULTLINE_PROTOCOL_INVALID_ARGUMENT && size == 99);
    CHECK(faultline_message_decode(wire, sizeof(wire), NULL, &size) == FAULTLINE_PROTOCOL_INVALID_ARGUMENT && size == 99);
    return EXIT_SUCCESS;
}

static int test_invalid_rows(void)
{
    const struct {int jobs; size_t offset, width; uint64_t value; enum faultline_protocol_result error;} cases[] = {
        {1, 0, 8, 0, FAULTLINE_PROTOCOL_INVALID_JOB_ID}, {1, 8, 2, 0, FAULTLINE_PROTOCOL_INVALID_TASK_TYPE},
        {1, 10, 2, 6, FAULTLINE_PROTOCOL_INVALID_JOB_STATE}, {1, 12, 4, 0, FAULTLINE_PROTOCOL_INVALID_WORKER_ID},
        {1, 16, 8, 0, FAULTLINE_PROTOCOL_INVALID_ATTEMPT}, {1, 24, 4, UINT32_MAX, FAULTLINE_PROTOCOL_INVALID_RETRY_COUNT},
        {1, 32, 2, 1, FAULTLINE_PROTOCOL_INVALID_FAILURE}, {1, 34, 4, 1025, FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE},
        {0, 0, 4, 0, FAULTLINE_PROTOCOL_INVALID_WORKER_ID}, {0, 4, 2, 0, FAULTLINE_PROTOCOL_INVALID_WORKER_STATE},
        {0, 4, 2, 3, FAULTLINE_PROTOCOL_INVALID_WORKER_STATE}, {0, 4, 2, 2, FAULTLINE_PROTOCOL_INVALID_JOB_ID},
        {0, 14, 8, 0, FAULTLINE_PROTOCOL_INVALID_ATTEMPT}, {0, 22, 8, 0, FAULTLINE_PROTOCOL_INVALID_ATTEMPT}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint8_t wire[54];
        size_t size = cases[i].jobs ? sizeof(jobs_wire) : sizeof(workers_wire);
        memcpy(wire, cases[i].jobs ? jobs_wire : workers_wire, size);
        put_be(wire + (cases[i].jobs ? 16 : 20) + cases[i].offset, cases[i].width, cases[i].value);
        CHECK(decode_error(wire, size, cases[i].error) == EXIT_SUCCESS);
    }
    for (int jobs = 0; jobs <= 1; ++jobs) {
        struct faultline_message message = sample(jobs);
        if (jobs) { message.payload.jobs.count = 2; message.payload.jobs.entries[1] = message.payload.jobs.entries[0]; }
        else { message.payload.workers.count = 2; message.payload.workers.entries[1] = message.payload.workers.entries[0]; }
        CHECK(encode_error(&message, FAULTLINE_MESSAGE_MAX_FRAME_SIZE, FAULTLINE_PROTOCOL_INVALID_LIST_ORDER) == EXIT_SUCCESS);
        uint8_t wire[100]; size_t size = jobs ? 92 : 80, prefix = jobs ? 16 : 20, stride = jobs ? 38 : 30;
        memcpy(wire, jobs ? jobs_wire : workers_wire, prefix + stride);
        memcpy(wire + prefix + stride, wire + prefix, stride);
        put_be(wire + 8, 4, size - 12); put_be(wire + 12, 4, 2);
        CHECK(decode_error(wire, size, FAULTLINE_PROTOCOL_INVALID_LIST_ORDER) == EXIT_SUCCESS);
        put_be(wire + prefix + stride, jobs ? 8 : 4, 1);
        CHECK(decode_error(wire, size, FAULTLINE_PROTOCOL_INVALID_LIST_ORDER) == EXIT_SUCCESS);
    }
    return EXIT_SUCCESS;
}

static int test_stream(void)
{
    uint8_t wire[116] = {HEADER(15, 0)};
    memcpy(wire + 12, jobs_wire, 54); memcpy(wire + 66, workers_wire, 50);
    struct faultline_message message; size_t consumed;
    CHECK(faultline_message_decode(wire, sizeof(wire), &message, &consumed) == FAULTLINE_PROTOCOL_OK && consumed == 12);
    CHECK(faultline_message_decode(wire + 12, sizeof(wire) - 13, &message, &consumed) == FAULTLINE_PROTOCOL_OK && consumed == 54);
    CHECK(decode_error(wire + 66, 49, FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL) == EXIT_SUCCESS);
    CHECK(faultline_message_decode(wire + 66, 50, &message, &consumed) == FAULTLINE_PROTOCOL_OK && consumed == 50);
    return EXIT_SUCCESS;
}

int main(void)
{
    const struct {const char *name; int (*run)(void);} tests[] = {
        {"literal job and worker listing bytes, unaligned buffers, and ownership", test_literals},
        {"empty listing requests and responses", test_empty},
        {"maximum lists, integer boundaries, every truncated prefix, and short capacities", test_maximum_and_fragments},
        {"job listing state and retry combinations", test_states},
        {"early count and length rejection, overflow, timeout, and null arguments", test_counts_and_lengths},
        {"invalid listing fields and duplicate or unsorted identities", test_invalid_rows},
        {"mixed listing stream and partial trailing frame", test_stream}
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (tests[i].run() != EXIT_SUCCESS) { return EXIT_FAILURE; }
        printf("PASS: %s\n", tests[i].name);
    }
    return EXIT_SUCCESS;
}
