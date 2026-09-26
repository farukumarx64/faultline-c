#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
    return EXIT_FAILURE; } } while (0)
#define JOB_ID UINT64_C(0x0102030405060708)
#define JOB_BYTES 1, 2, 3, 4, 5, 6, 7, 8
#define WORKER_ID UINT32_C(0x11121314)
#define WORKER_BYTES 0x11, 0x12, 0x13, 0x14
#define HEADER(type, length) 0x46, 0x4c, 0x49, 0x4e, 0, 1, 0, type, 0, 0, 0, length

/* Literal oracles keep field offsets/byte order independent of the encoder. */
static const struct {
    struct faultline_message message;
    size_t size;
    uint8_t wire[51];
} examples[] = {
    {{.message_type = FAULTLINE_MSG_JOB_STATUS_REQUEST, .payload.job_status_request = JOB_ID},
     20, {HEADER(12, 8), JOB_BYTES}},
    {{.message_type = FAULTLINE_MSG_JOB_STATUS_NOT_FOUND, .payload.job_status_not_found = JOB_ID},
     20, {HEADER(14, 8), JOB_BYTES}},
    {{.message_type = FAULTLINE_MSG_JOB_STATUS_RESPONSE, .payload.job_status_response = {
        .job_id = JOB_ID, .state = FAULTLINE_JOB_QUEUED, .max_retries = 2}}, 48,
     {HEADER(13, 36), JOB_BYTES, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0}},
    {{.message_type = FAULTLINE_MSG_JOB_STATUS_RESPONSE, .payload.job_status_response = {
        .job_id = JOB_ID, .state = FAULTLINE_JOB_QUEUED, .attempt = 1, .retry_count = 1,
        .max_retries = 2, .failure = FAULTLINE_JOB_FAILURE_WORKER_LOST}}, 48,
     {HEADER(13, 36), JOB_BYTES, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1,
      0, 0, 0, 1, 0, 0, 0, 2, 0, 2, 0, 0, 0, 0}},
    {{.message_type = FAULTLINE_MSG_JOB_STATUS_RESPONSE, .payload.job_status_response = {
        .job_id = JOB_ID, .state = FAULTLINE_JOB_ASSIGNED, .worker_id = WORKER_ID, .attempt = 1}}, 48,
     {HEADER(13, 36), JOB_BYTES, 0, 2, WORKER_BYTES, 0, 0, 0, 0, 0, 0, 0, 1,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {{.message_type = FAULTLINE_MSG_JOB_STATUS_RESPONSE, .payload.job_status_response = {
        .job_id = JOB_ID, .state = FAULTLINE_JOB_RUNNING, .worker_id = WORKER_ID,
        .attempt = UINT64_C(0x01020305), .retry_count = UINT32_C(0x01020304),
        .max_retries = UINT32_C(0x11121314)}}, 48,
     {HEADER(13, 36), JOB_BYTES, 0, 3, WORKER_BYTES, 0, 0, 0, 0, 1, 2, 3, 5,
      1, 2, 3, 4, 0x11, 0x12, 0x13, 0x14, 0, 0, 0, 0, 0, 0}},
    {{.message_type = FAULTLINE_MSG_JOB_STATUS_RESPONSE, .payload.job_status_response = {
        .job_id = JOB_ID, .state = FAULTLINE_JOB_DONE, .worker_id = WORKER_ID,
        .attempt = 2, .retry_count = 1, .max_retries = 3,
        .result_size = 3, .result = {0xaa, 0, 0xbb}}}, 51,
     {HEADER(13, 39), JOB_BYTES, 0, 4, WORKER_BYTES, 0, 0, 0, 0, 0, 0, 0, 2,
      0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0, 0, 0, 3, 0xaa, 0, 0xbb}},
    {{.message_type = FAULTLINE_MSG_JOB_STATUS_RESPONSE, .payload.job_status_response = {
        .job_id = JOB_ID, .state = FAULTLINE_JOB_FAILED, .worker_id = WORKER_ID,
        .attempt = 3, .retry_count = 2, .max_retries = 2, .failure = FAULTLINE_JOB_FAILURE_TASK}}, 48,
     {HEADER(13, 36), JOB_BYTES, 0, 5, WORKER_BYTES, 0, 0, 0, 0, 0, 0, 0, 3,
      0, 0, 0, 2, 0, 0, 0, 2, 0, 1, 0, 0, 0, 0}}
};
#define EXAMPLE_COUNT (sizeof(examples) / sizeof(examples[0]))

static int equal(const struct faultline_message *a, const struct faultline_message *b)
{
    if (a->message_type != b->message_type) { return 0; }
    if (a->message_type == FAULTLINE_MSG_JOB_STATUS_REQUEST) {
        return a->payload.job_status_request == b->payload.job_status_request;
    }
    if (a->message_type == FAULTLINE_MSG_JOB_STATUS_NOT_FOUND) {
        return a->payload.job_status_not_found == b->payload.job_status_not_found;
    }
    const struct faultline_job_status_payload *x = &a->payload.job_status_response;
    const struct faultline_job_status_payload *y = &b->payload.job_status_response;
    return x->job_id == y->job_id && x->state == y->state && x->worker_id == y->worker_id &&
        x->attempt == y->attempt && x->retry_count == y->retry_count && x->max_retries == y->max_retries &&
        x->failure == y->failure && x->result_size == y->result_size &&
        memcmp(x->result, y->result, x->result_size) == 0;
}

static int encode_error(const struct faultline_message *message, size_t capacity,
                        enum faultline_protocol_result expected)
{
    uint8_t wire[FAULTLINE_MESSAGE_MAX_FRAME_SIZE], before[sizeof(wire)];
    memset(wire, 0xa5, sizeof(wire));
    memcpy(before, wire, sizeof(wire));
    size_t size = 99;
    CHECK(faultline_message_encode(wire, capacity, message, &size) == expected);
    CHECK(size == 99 && memcmp(wire, before, sizeof(wire)) == 0);
    return EXIT_SUCCESS;
}

static int decode_error(const uint8_t *wire, size_t size, enum faultline_protocol_result expected)
{
    /* Exact allocations expose overreads of incomplete prefixes to ASan. */
    uint8_t *input = malloc(size == 0 ? 1 : size);
    CHECK(input != NULL);
    memcpy(input, wire, size);
    struct faultline_message message;
    uint8_t before[sizeof(message)];
    memset(&message, 0xa5, sizeof(message));
    memcpy(before, &message, sizeof(message));
    size_t consumed = 99;
    enum faultline_protocol_result result = faultline_message_decode(input, size, &message, &consumed);
    free(input);
    CHECK(result == expected);
    CHECK(consumed == 99 && memcmp(&message, before, sizeof(message)) == 0);
    return EXIT_SUCCESS;
}

static int round_trip(const struct faultline_message *message)
{
    uint8_t wire[FAULTLINE_MESSAGE_MAX_FRAME_SIZE];
    struct faultline_message decoded;
    size_t size, consumed;
    CHECK(faultline_message_encode(wire, sizeof(wire), message, &size) == FAULTLINE_PROTOCOL_OK);
    CHECK(faultline_message_decode(wire, size, &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
    CHECK(consumed == size && equal(message, &decoded));
    return EXIT_SUCCESS;
}

static int test_literals(void)
{
    for (size_t i = 0; i < EXAMPLE_COUNT; ++i) {
        _Alignas(uint64_t) uint8_t wire[54];
        memset(wire, 0xa5, sizeof(wire));
        struct faultline_message decoded;
        size_t size, consumed;
        CHECK(faultline_message_encode(wire + 1, sizeof(wire) - 1, &examples[i].message, &size) == FAULTLINE_PROTOCOL_OK);
        CHECK(size == examples[i].size && memcmp(wire + 1, examples[i].wire, size) == 0);
        CHECK(wire[0] == 0xa5);
        for (size_t j = size + 1; j < sizeof(wire); ++j) { CHECK(wire[j] == 0xa5); }
        memcpy(wire + 1, examples[i].wire, size);
        CHECK(faultline_message_decode(wire + 1, sizeof(wire) - 1, &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
        CHECK(consumed == size && equal(&decoded, &examples[i].message));
    }
    return EXIT_SUCCESS;
}

static int check_model(const struct faultline_job *job)
{
    struct faultline_message message = {.message_type = FAULTLINE_MSG_JOB_STATUS_RESPONSE,
        .payload.job_status_response = {.job_id = job->id, .state = (uint16_t)job->state,
            .worker_id = job->worker_id, .attempt = job->attempt, .retry_count = job->retry_count,
            .max_retries = job->max_retries, .failure = (uint16_t)job->failure, .result_size = job->result_size}};
    memcpy(message.payload.job_status_response.result, job->result, job->result_size);
    return round_trip(&message);
}

static int test_model_states(void)
{
    for (uint32_t budget = 0; budget <= 2; ++budget) {
        for (int lost = 0; lost <= 1; ++lost) {
            struct faultline_job job;
            CHECK(faultline_job_init(&job, JOB_ID, FAULTLINE_TASK_HASH, NULL, 0, budget, 0) == FAULTLINE_JOB_OK);
            CHECK(check_model(&job) == EXIT_SUCCESS);
            for (uint32_t retry = 0; retry <= budget; ++retry) {
                CHECK(faultline_job_assign(&job, WORKER_ID, 0) == FAULTLINE_JOB_OK);
                CHECK(check_model(&job) == EXIT_SUCCESS);
                /* Both failure before execution and failure after STARTED are valid. */
                if (lost) {
                    CHECK(faultline_job_start(&job, WORKER_ID, job.attempt, 0) == FAULTLINE_JOB_OK);
                    CHECK(check_model(&job) == EXIT_SUCCESS);
                    struct faultline_job done = job;
                    const uint8_t result[] = {0, 255, 'a'};
                    CHECK(faultline_job_complete(&done, WORKER_ID, done.attempt, result, sizeof(result), 0) == FAULTLINE_JOB_OK);
                    CHECK(check_model(&done) == EXIT_SUCCESS);
                }
                CHECK(faultline_job_fail(&job, WORKER_ID, job.attempt,
                    lost ? FAULTLINE_JOB_FAILURE_WORKER_LOST : FAULTLINE_JOB_FAILURE_TASK, 0) == FAULTLINE_JOB_OK);
                CHECK(check_model(&job) == EXIT_SUCCESS);
            }
        }
    }
    return EXIT_SUCCESS;
}

static int test_boundaries(void)
{
    const uint64_t ids[] = {1, UINT64_C(0x100000000), UINT64_MAX};
    for (size_t i = 0; i < 3; ++i) {
        struct faultline_message request = examples[0].message, missing = examples[1].message;
        request.payload.job_status_request = missing.payload.job_status_not_found = ids[i];
        CHECK(round_trip(&request) == EXIT_SUCCESS && round_trip(&missing) == EXIT_SUCCESS);
        for (uint16_t state = FAULTLINE_JOB_QUEUED; state <= FAULTLINE_JOB_FAILED; ++state) {
            struct faultline_message message = {.message_type = FAULTLINE_MSG_JOB_STATUS_RESPONSE,
                .payload.job_status_response = {.job_id = ids[i], .state = state,
                    .worker_id = state == FAULTLINE_JOB_QUEUED ? 0 : UINT32_MAX,
                    .attempt = (uint64_t)UINT32_MAX + (state == FAULTLINE_JOB_QUEUED ? 0u : 1u),
                    .retry_count = UINT32_MAX, .max_retries = UINT32_MAX,
                    .failure = (state == FAULTLINE_JOB_QUEUED || state == FAULTLINE_JOB_FAILED) ?
                               FAULTLINE_JOB_FAILURE_WORKER_LOST : FAULTLINE_JOB_FAILURE_NONE}};
            CHECK(round_trip(&message) == EXIT_SUCCESS);
            uint8_t wire[48];
            size_t size;
            CHECK(faultline_message_encode(wire, sizeof(wire), &message, &size) == FAULTLINE_PROTOCOL_OK);
            if (state != FAULTLINE_JOB_QUEUED) {
                const uint8_t attempt[] = {0, 0, 0, 1, 0, 0, 0, 0};
                CHECK(memcmp(wire + 26, attempt, 8) == 0);
            }
        }
    }
    const size_t lengths[] = {0, 1, 255, 256, 1023, 1024};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        struct faultline_message message = examples[6].message, decoded;
        struct faultline_job_status_payload *status = &message.payload.job_status_response;
        status->result_size = lengths[i];
        for (size_t j = 0; j < status->result_size; ++j) { status->result[j] = (uint8_t)j; }
        uint8_t wire[FAULTLINE_MESSAGE_MAX_FRAME_SIZE];
        size_t size, consumed;
        CHECK(faultline_message_encode(wire, sizeof(wire), &message, &size) == FAULTLINE_PROTOCOL_OK);
        CHECK(size == 48 + lengths[i] && memcmp(wire + 48, status->result, lengths[i]) == 0);
        CHECK(faultline_message_decode(wire, size, &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
        CHECK(consumed == size);
        memset(wire, 0x55, sizeof(wire));
        CHECK(equal(&message, &decoded));
    }
    return EXIT_SUCCESS;
}

static int test_fragments(void)
{
    for (size_t i = 0; i < EXAMPLE_COUNT; ++i) {
        for (size_t n = 0; n < examples[i].size; ++n) {
            CHECK(decode_error(examples[i].wire, n, FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL) == EXIT_SUCCESS);
            CHECK(encode_error(&examples[i].message, n, FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL) == EXIT_SUCCESS);
        }
    }
    struct faultline_message message = examples[6].message;
    message.payload.job_status_response.result_size = 1024;
    uint8_t wire[FAULTLINE_MESSAGE_MAX_FRAME_SIZE];
    size_t size;
    CHECK(faultline_message_encode(wire, sizeof(wire), &message, &size) == FAULTLINE_PROTOCOL_OK);
    CHECK(size == FAULTLINE_HEADER_SIZE + FAULTLINE_JOB_STATUS_RESPONSE_PREFIX_SIZE + FAULTLINE_JOB_MAX_RESULT_SIZE);
    for (size_t n = 0; n < size; ++n) {
        CHECK(decode_error(wire, n, FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL) == EXIT_SUCCESS);
        CHECK(encode_error(&message, n, FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL) == EXIT_SUCCESS);
    }
    return EXIT_SUCCESS;
}

static void put_be(uint8_t *out, size_t width, uint64_t value)
{
    for (size_t i = 0; i < width; ++i) { out[width - i - 1] = (uint8_t)(value >> (8 * i)); }
}

static int test_invalid_fields(void)
{
    /* Mutate independent wire vectors and equivalent host fields. The prefix
     * alone must reject bad metadata, even before DONE's result bytes arrive. */
    const struct {size_t base, offset, width; uint64_t value; enum faultline_protocol_result error;} cases[] = {
        {2, 0, 8, 0, FAULTLINE_PROTOCOL_INVALID_JOB_ID},
        {2, 8, 2, 0, FAULTLINE_PROTOCOL_INVALID_JOB_STATE},
        {2, 8, 2, 6, FAULTLINE_PROTOCOL_INVALID_JOB_STATE},
        {2, 8, 2, UINT16_MAX, FAULTLINE_PROTOCOL_INVALID_JOB_STATE},
        {2, 10, 4, 1, FAULTLINE_PROTOCOL_INVALID_WORKER_ID},
        {4, 10, 4, 0, FAULTLINE_PROTOCOL_INVALID_WORKER_ID},
        {5, 10, 4, 0, FAULTLINE_PROTOCOL_INVALID_WORKER_ID},
        {6, 10, 4, 0, FAULTLINE_PROTOCOL_INVALID_WORKER_ID},
        {7, 10, 4, 0, FAULTLINE_PROTOCOL_INVALID_WORKER_ID},
        {2, 14, 8, 1, FAULTLINE_PROTOCOL_INVALID_ATTEMPT},
        {3, 14, 8, 0, FAULTLINE_PROTOCOL_INVALID_ATTEMPT},
        {4, 14, 8, 0, FAULTLINE_PROTOCOL_INVALID_ATTEMPT},
        {5, 14, 8, UINT64_MAX, FAULTLINE_PROTOCOL_INVALID_ATTEMPT},
        {6, 14, 8, 1, FAULTLINE_PROTOCOL_INVALID_ATTEMPT},
        {7, 14, 8, 2, FAULTLINE_PROTOCOL_INVALID_ATTEMPT},
        {2, 22, 4, 3, FAULTLINE_PROTOCOL_INVALID_RETRY_COUNT},
        {3, 26, 4, 0, FAULTLINE_PROTOCOL_INVALID_RETRY_COUNT},
        {7, 26, 4, 3, FAULTLINE_PROTOCOL_INVALID_RETRY_COUNT},
        {2, 30, 2, 1, FAULTLINE_PROTOCOL_INVALID_FAILURE},
        {3, 30, 2, 0, FAULTLINE_PROTOCOL_INVALID_FAILURE},
        {3, 30, 2, 3, FAULTLINE_PROTOCOL_INVALID_FAILURE},
        {4, 30, 2, 2, FAULTLINE_PROTOCOL_INVALID_FAILURE},
        {5, 30, 2, 1, FAULTLINE_PROTOCOL_INVALID_FAILURE},
        {6, 30, 2, 2, FAULTLINE_PROTOCOL_INVALID_FAILURE},
        {7, 30, 2, 0, FAULTLINE_PROTOCOL_INVALID_FAILURE},
        {7, 30, 2, UINT16_MAX, FAULTLINE_PROTOCOL_INVALID_FAILURE},
        {2, 32, 4, 1, FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH},
        {3, 32, 4, 1, FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH},
        {4, 32, 4, 1, FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH},
        {5, 32, 4, 1, FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH},
        {7, 32, 4, 1, FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH},
        {6, 32, 4, 1025, FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct faultline_message message = examples[cases[i].base].message;
        struct faultline_job_status_payload *s = &message.payload.job_status_response;
        uint64_t v = cases[i].value;
        switch (cases[i].offset) {
        case 0: s->job_id = v; break;
        case 8: s->state = (uint16_t)v; break;
        case 10: s->worker_id = (uint32_t)v; break;
        case 14: s->attempt = v; break;
        case 22: s->retry_count = (uint32_t)v; break;
        case 26: s->max_retries = (uint32_t)v; break;
        case 30: s->failure = (uint16_t)v; break;
        case 32: s->result_size = (size_t)v; break;
        default: return EXIT_FAILURE;
        }
        CHECK(encode_error(&message, FAULTLINE_MESSAGE_MAX_FRAME_SIZE, cases[i].error) == EXIT_SUCCESS);
        uint8_t wire[48];
        memcpy(wire, examples[cases[i].base].wire, sizeof(wire));
        put_be(wire + 12 + cases[i].offset, cases[i].width, v);
        CHECK(decode_error(wire, sizeof(wire), cases[i].error) == EXIT_SUCCESS);
    }
    for (size_t i = 0; i < 2; ++i) {
        struct faultline_message message = examples[i].message;
        if (i == 0) { message.payload.job_status_request = 0; }
        else { message.payload.job_status_not_found = 0; }
        CHECK(encode_error(&message, 20, FAULTLINE_PROTOCOL_INVALID_JOB_ID) == EXIT_SUCCESS);
        uint8_t wire[20];
        memcpy(wire, examples[i].wire, sizeof(wire)); memset(wire + 12, 0, 8);
        CHECK(decode_error(wire, sizeof(wire), FAULTLINE_PROTOCOL_INVALID_JOB_ID) == EXIT_SUCCESS);
    }
    return EXIT_SUCCESS;
}

static int test_lengths_and_pointers(void)
{
    for (size_t i = 0; i < EXAMPLE_COUNT; ++i) {
        uint8_t wire[51];
        memcpy(wire, examples[i].wire, sizeof(wire));
        uint32_t minimum = i < 2 ? 8 : 36;
        put_be(wire + 8, 4, minimum - 1);
        CHECK(decode_error(wire, 12, FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH) == EXIT_SUCCESS);
        put_be(wire + 8, 4, i < 2 ? 9 : 1061);
        CHECK(decode_error(wire, 12, i < 2 ? FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH :
                                          FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE) == EXIT_SUCCESS);
    }
    uint8_t wire[51];
    memcpy(wire, examples[6].wire, sizeof(wire));
    put_be(wire + 44, 4, 4);  /* Outer length still says three result bytes. */
    CHECK(decode_error(wire, 48, FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH) == EXIT_SUCCESS);
    put_be(wire + 44, 4, 0);
    CHECK(decode_error(wire, 48, FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH) == EXIT_SUCCESS);
    struct faultline_message message = examples[6].message;
    message.payload.job_status_response.result_size = SIZE_MAX;
    CHECK(encode_error(&message, FAULTLINE_MESSAGE_MAX_FRAME_SIZE, FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE) == EXIT_SUCCESS);
    message = examples[6].message;
    struct faultline_message before = message;
    uint8_t original[sizeof(wire)]; memcpy(original, wire, sizeof(wire));
    size_t size = 99;
    CHECK(faultline_message_encode(NULL, sizeof(wire), &message, &size) == FAULTLINE_PROTOCOL_INVALID_ARGUMENT);
    CHECK(faultline_message_encode(wire, sizeof(wire), NULL, &size) == FAULTLINE_PROTOCOL_INVALID_ARGUMENT);
    CHECK(faultline_message_encode(wire, sizeof(wire), &message, NULL) == FAULTLINE_PROTOCOL_INVALID_ARGUMENT);
    CHECK(faultline_message_decode(NULL, sizeof(wire), &message, &size) == FAULTLINE_PROTOCOL_INVALID_ARGUMENT);
    CHECK(faultline_message_decode(wire, sizeof(wire), NULL, &size) == FAULTLINE_PROTOCOL_INVALID_ARGUMENT);
    CHECK(faultline_message_decode(wire, sizeof(wire), &message, NULL) == FAULTLINE_PROTOCOL_INVALID_ARGUMENT);
    CHECK(size == 99 && equal(&message, &before) && memcmp(wire, original, sizeof(wire)) == 0);
    return EXIT_SUCCESS;
}

static int test_stream(void)
{
    uint8_t stream[12 + 51 + 20 + 20] = {HEADER(1, 0)};
    memcpy(stream + 12, examples[6].wire, 51);
    memcpy(stream + 63, examples[1].wire, 20);
    memcpy(stream + 83, examples[0].wire, 20);
    struct faultline_message decoded;
    size_t consumed, offset = 0;
    CHECK(faultline_message_decode(stream, sizeof(stream), &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
    CHECK(decoded.message_type == FAULTLINE_MSG_PING && consumed == 12);
    offset += consumed;
    const size_t indexes[] = {6, 1};
    for (size_t i = 0; i < 2; ++i) {
        CHECK(faultline_message_decode(stream + offset, sizeof(stream) - offset - 1, &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
        CHECK(consumed == examples[indexes[i]].size && equal(&decoded, &examples[indexes[i]].message));
        offset += consumed;
    }
    CHECK(offset == 83);
    CHECK(decode_error(stream + offset, 19, FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL) == EXIT_SUCCESS);
    CHECK(faultline_message_decode(stream + offset, 20, &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
    CHECK(consumed == 20 && equal(&decoded, &examples[0].message));
    return EXIT_SUCCESS;
}

int main(void)
{
    const struct {const char *name; int (*run)(void);} tests[] = {
        {"literal status request, states, binary result, and not-found frames", test_literals},
        {"status snapshots through real model transitions and retry exhaustion", test_model_states},
        {"status integer limits, 64-bit attempts, and owned binary results", test_boundaries},
        {"every status frame prefix and short capacity, including 1072-byte maximum", test_fragments},
        {"invalid status identities, states, counters, failures, and results", test_invalid_fields},
        {"status outer/inner lengths, early rejection, null pointers, and unchanged outputs", test_lengths_and_pointers},
        {"mixed status stream, exact consumption, and trailing partial request", test_stream}
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (tests[i].run() != EXIT_SUCCESS) { fprintf(stderr, "FAIL: %s\n", tests[i].name); return EXIT_FAILURE; }
        printf("PASS: %s\n", tests[i].name);
    }
    return EXIT_SUCCESS;
}
