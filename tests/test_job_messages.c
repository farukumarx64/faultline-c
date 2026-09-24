#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,   \
                    #condition);                                             \
            return EXIT_FAILURE;                                             \
        }                                                                    \
    } while (0)

#define SAMPLE_IDENTITY {UINT64_C(0x0102030405060708), UINT32_C(0x11121314), \
                         UINT64_C(0x2122232425262728)}
#define IDENTITY_BYTES 1, 2, 3, 4, 5, 6, 7, 8, 0x11, 0x12, 0x13, 0x14, \
                       0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28

/* Independent whole-frame vectors: offsets, lengths, IDs, and byte order. */
static const struct {
    struct faultline_message message;
    size_t size;
    uint8_t wire[41];
} examples[] = {
    {{.message_type = FAULTLINE_MSG_JOB_SUBMIT, .payload.job_submit = {
        .task_type = FAULTLINE_TASK_HASH, .max_retries = UINT32_C(0x31323334),
        .argument_size = 3, .arguments = {0xaa, 0, 0xbb}}}, 25,
     {0x46, 0x4c, 0x49, 0x4e, 0, 1, 0, 6, 0, 0, 0, 13,
      0, 4, 0x31, 0x32, 0x33, 0x34, 0, 0, 0, 3, 0xaa, 0, 0xbb}},
    {{.message_type = FAULTLINE_MSG_JOB_SUBMIT_ACK,
      .payload.job_submit_ack = UINT64_C(0x0102030405060708)}, 20,
     {0x46, 0x4c, 0x49, 0x4e, 0, 1, 0, 7, 0, 0, 0, 8,
      1, 2, 3, 4, 5, 6, 7, 8}},
    {{.message_type = FAULTLINE_MSG_JOB_ASSIGN, .payload.job_assign = {
        .identity = SAMPLE_IDENTITY, .task_type = FAULTLINE_TASK_HASH,
        .argument_size = 3, .arguments = {0xaa, 0, 0xbb}}}, 41,
     {0x46, 0x4c, 0x49, 0x4e, 0, 1, 0, 8, 0, 0, 0, 29,
      IDENTITY_BYTES, 0, 4, 0, 0, 0, 3, 0xaa, 0, 0xbb}},
    {{.message_type = FAULTLINE_MSG_JOB_STARTED,
      .payload.job_started = SAMPLE_IDENTITY}, 32,
     {0x46, 0x4c, 0x49, 0x4e, 0, 1, 0, 9, 0, 0, 0, 20, IDENTITY_BYTES}},
    {{.message_type = FAULTLINE_MSG_JOB_COMPLETED, .payload.job_completed = {
        .identity = SAMPLE_IDENTITY, .result_size = 3, .result = {0xaa, 0, 0xbb}}}, 39,
     {0x46, 0x4c, 0x49, 0x4e, 0, 1, 0, 10, 0, 0, 0, 27,
      IDENTITY_BYTES, 0, 0, 0, 3, 0xaa, 0, 0xbb}},
    {{.message_type = FAULTLINE_MSG_JOB_FAILED, .payload.job_failed = {
        .identity = SAMPLE_IDENTITY, .failure = FAULTLINE_JOB_FAILURE_TASK}}, 34,
     {0x46, 0x4c, 0x49, 0x4e, 0, 1, 0, 11, 0, 0, 0, 22,
      IDENTITY_BYTES, 0, 1}}
};
#define EXAMPLE_COUNT (sizeof(examples) / sizeof(examples[0]))

static struct faultline_job_identity *identity_of(struct faultline_message *message)
{
    switch (message->message_type) {
    case FAULTLINE_MSG_JOB_ASSIGN: return &message->payload.job_assign.identity;
    case FAULTLINE_MSG_JOB_STARTED: return &message->payload.job_started;
    case FAULTLINE_MSG_JOB_COMPLETED: return &message->payload.job_completed.identity;
    case FAULTLINE_MSG_JOB_FAILED: return &message->payload.job_failed.identity;
    default: return NULL;
    }
}

static int identities_equal(const struct faultline_job_identity *a,
                            const struct faultline_job_identity *b)
{
    return a->job_id == b->job_id && a->worker_id == b->worker_id && a->attempt == b->attempt;
}

static int messages_equal(const struct faultline_message *a, const struct faultline_message *b)
{
    if (a->message_type != b->message_type) {
        return 0;
    }
    switch (a->message_type) {
    case FAULTLINE_MSG_JOB_SUBMIT:
        return a->payload.job_submit.task_type == b->payload.job_submit.task_type &&
               a->payload.job_submit.max_retries == b->payload.job_submit.max_retries &&
               a->payload.job_submit.argument_size == b->payload.job_submit.argument_size &&
               memcmp(a->payload.job_submit.arguments, b->payload.job_submit.arguments,
                      a->payload.job_submit.argument_size) == 0;
    case FAULTLINE_MSG_JOB_SUBMIT_ACK:
        return a->payload.job_submit_ack == b->payload.job_submit_ack;
    case FAULTLINE_MSG_JOB_ASSIGN:
        return identities_equal(&a->payload.job_assign.identity, &b->payload.job_assign.identity) &&
               a->payload.job_assign.task_type == b->payload.job_assign.task_type &&
               a->payload.job_assign.argument_size == b->payload.job_assign.argument_size &&
               memcmp(a->payload.job_assign.arguments, b->payload.job_assign.arguments,
                      a->payload.job_assign.argument_size) == 0;
    case FAULTLINE_MSG_JOB_STARTED:
        return identities_equal(&a->payload.job_started, &b->payload.job_started);
    case FAULTLINE_MSG_JOB_COMPLETED:
        return identities_equal(&a->payload.job_completed.identity, &b->payload.job_completed.identity) &&
               a->payload.job_completed.result_size == b->payload.job_completed.result_size &&
               memcmp(a->payload.job_completed.result, b->payload.job_completed.result,
                      a->payload.job_completed.result_size) == 0;
    case FAULTLINE_MSG_JOB_FAILED:
        return identities_equal(&a->payload.job_failed.identity, &b->payload.job_failed.identity) &&
               a->payload.job_failed.failure == b->payload.job_failed.failure;
    default: return 0;
    }
}

static int decode_error(const uint8_t *wire, size_t size, enum faultline_protocol_result expected)
{
    struct faultline_message decoded;
    unsigned char original[sizeof(decoded)];
    size_t consumed = 99;
    /* Exact allocations expose prefix overreads to ASan. */
    uint8_t *input = malloc(size == 0 ? 1 : size);
    CHECK(input != NULL);
    memcpy(input, wire, size);
    memset(&decoded, 0xa5, sizeof(decoded));
    memcpy(original, &decoded, sizeof(decoded));
    enum faultline_protocol_result result =
        faultline_message_decode(input, size, &decoded, &consumed);
    free(input);
    CHECK(result == expected);
    CHECK(memcmp(&decoded, original, sizeof(decoded)) == 0);
    CHECK(consumed == 99);
    return EXIT_SUCCESS;
}

static int encode_error(const struct faultline_message *message, size_t capacity,
                        enum faultline_protocol_result expected)
{
    uint8_t wire[FAULTLINE_MESSAGE_MAX_FRAME_SIZE + 1];
    uint8_t original[sizeof(wire)];
    size_t written = 99;
    memset(wire, 0xa5, sizeof(wire));
    memcpy(original, wire, sizeof(wire));
    CHECK(faultline_message_encode(wire, capacity, message, &written) == expected);
    CHECK(memcmp(wire, original, sizeof(wire)) == 0);
    CHECK(written == 99);
    return EXIT_SUCCESS;
}

static int test_literal_frames(void)
{
    for (size_t i = 0; i < EXAMPLE_COUNT; ++i) {
        _Alignas(uint64_t) uint8_t storage[43];
        struct faultline_message decoded;
        size_t written = 99, consumed = 99;
        memset(storage, 0xa5, sizeof(storage));
        CHECK(faultline_message_encode(storage + 1, sizeof(storage) - 1,
                                       &examples[i].message, &written) == FAULTLINE_PROTOCOL_OK);
        CHECK(written == examples[i].size);
        CHECK(memcmp(storage + 1, examples[i].wire, written) == 0);
        CHECK(storage[0] == 0xa5);
        for (size_t j = written + 1; j < sizeof(storage); ++j) {
            CHECK(storage[j] == 0xa5);
        }
        memcpy(storage + 1, examples[i].wire, examples[i].size);
        CHECK(faultline_message_decode(storage + 1, sizeof(storage) - 1,
                                       &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
        CHECK(consumed == examples[i].size);
        CHECK(messages_equal(&decoded, &examples[i].message));
    }
    return EXIT_SUCCESS;
}

static int test_partial_frames(void)
{
    for (size_t i = 0; i < EXAMPLE_COUNT; ++i) {
        for (size_t size = 0; size < examples[i].size; ++size) {
            CHECK(decode_error(examples[i].wire, size, FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL) == EXIT_SUCCESS);
            CHECK(encode_error(&examples[i].message, size, FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL) == EXIT_SUCCESS);
        }
    }
    /* Every prefix/capacity of the largest assignment, including long data. */
    struct faultline_message message = examples[2].message;
    uint8_t wire[FAULTLINE_MESSAGE_MAX_FRAME_SIZE];
    size_t written = 0;
    message.payload.job_assign.argument_size = FAULTLINE_JOB_MAX_ARGUMENT_SIZE;
    CHECK(faultline_message_encode(wire, sizeof(wire), &message, &written) == FAULTLINE_PROTOCOL_OK);
    CHECK(written == FAULTLINE_HEADER_SIZE + FAULTLINE_JOB_ASSIGN_PREFIX_SIZE + FAULTLINE_JOB_MAX_ARGUMENT_SIZE);
    for (size_t size = 0; size < written; ++size) {
        CHECK(decode_error(wire, size, FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL) == EXIT_SUCCESS);
        CHECK(encode_error(&message, size, FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL) == EXIT_SUCCESS);
    }
    return EXIT_SUCCESS;
}

static int test_data_boundaries_and_ownership(void)
{
    const size_t example_ids[] = {0, 2, 4};
    const size_t sizes[] = {0, 1, 255, 256, 1024};
    const size_t prefixes[] = {10, 26, 24};
    for (size_t type = 0; type < 3; ++type) {
        for (size_t n = 0; n < sizeof(sizes) / sizeof(sizes[0]); ++n) {
            struct faultline_message message = examples[example_ids[type]].message;
            struct faultline_message decoded;
            uint8_t wire[FAULTLINE_MESSAGE_MAX_FRAME_SIZE];
            uint8_t *data;
            size_t written = 0, consumed = 0;
            if (type == 0) {
                data = message.payload.job_submit.arguments;
                message.payload.job_submit.argument_size = sizes[n];
            } else if (type == 1) {
                data = message.payload.job_assign.arguments;
                message.payload.job_assign.argument_size = sizes[n];
            } else {
                data = message.payload.job_completed.result;
                message.payload.job_completed.result_size = sizes[n];
            }
            for (size_t i = 0; i < sizes[n]; ++i) { data[i] = (uint8_t)i; }
            CHECK(faultline_message_encode(wire, sizeof(wire), &message, &written) == FAULTLINE_PROTOCOL_OK);
            CHECK(written == 12 + prefixes[type] + sizes[n]);
            CHECK(memcmp(wire + 12 + prefixes[type], data, sizes[n]) == 0);
            if (sizes[n] == 1024) {
                const uint8_t length[] = {0, 0, 4, 0};
                CHECK(memcmp(wire + 12 + prefixes[type] - 4, length, 4) == 0);
            }
            CHECK(faultline_message_decode(wire, written, &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
            CHECK(consumed == written);
            /* The decoded arguments/results survive reuse of the receive buffer. */
            memset(wire, 0x55, sizeof(wire));
            CHECK(messages_equal(&decoded, &message));
        }
        struct faultline_message message = examples[example_ids[type]].message;
        const size_t bad_sizes[] = {1025, SIZE_MAX};
        for (size_t n = 0; n < 2; ++n) {
            if (type == 0) { message.payload.job_submit.argument_size = bad_sizes[n]; }
            else if (type == 1) { message.payload.job_assign.argument_size = bad_sizes[n]; }
            else { message.payload.job_completed.result_size = bad_sizes[n]; }
            CHECK(encode_error(&message, FAULTLINE_MESSAGE_MAX_FRAME_SIZE,
                               FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE) == EXIT_SUCCESS);
        }
    }
    return EXIT_SUCCESS;
}

static int test_integer_boundaries(void)
{
    const uint64_t ids[] = {1, UINT64_C(0x100000000), UINT64_MAX};
    for (size_t i = 0; i < EXAMPLE_COUNT; ++i) {
        for (size_t j = 0; j < 3; ++j) {
            struct faultline_message message = examples[i].message;
            struct faultline_message decoded;
            struct faultline_job_identity *identity = identity_of(&message);
            uint8_t wire[FAULTLINE_MESSAGE_MAX_FRAME_SIZE];
            size_t size = 0, consumed = 0;
            if (identity != NULL) {
                identity->job_id = ids[j];
                identity->attempt = ids[j];
                identity->worker_id = j == 0 ? 1 : UINT32_MAX;
            } else if (i == 1) {
                message.payload.job_submit_ack = ids[j];
            } else {
                message.payload.job_submit.max_retries = j == 0 ? 0 : UINT32_MAX;
            }
            CHECK(faultline_message_encode(wire, sizeof(wire), &message, &size) == FAULTLINE_PROTOCOL_OK);
            CHECK(faultline_message_decode(wire, size, &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
            CHECK(messages_equal(&message, &decoded));
            if (i != 0 && j == 2) {
                const uint8_t all_bits[] = {255, 255, 255, 255, 255, 255, 255, 255};
                CHECK(memcmp(wire + 12, all_bits, 8) == 0);
                if (identity != NULL) { CHECK(memcmp(wire + 24, all_bits, 8) == 0); }
            }
        }
    }
    for (uint16_t task = 1; task <= 4; ++task) {
        for (size_t i = 0; i <= 2; i += 2) {
            struct faultline_message message = examples[i].message, decoded;
            uint8_t wire[41];
            size_t size = 0, consumed = 0;
            if (i == 0) { message.payload.job_submit.task_type = task; }
            else { message.payload.job_assign.task_type = task; }
            CHECK(faultline_message_encode(wire, sizeof(wire), &message, &size) == FAULTLINE_PROTOCOL_OK);
            CHECK(faultline_message_decode(wire, size, &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
            CHECK(messages_equal(&message, &decoded));
        }
    }
    return EXIT_SUCCESS;
}

static int test_invalid_fields(void)
{
    for (size_t i = 1; i < EXAMPLE_COUNT; ++i) {
        const enum faultline_protocol_result errors[] = {
            FAULTLINE_PROTOCOL_INVALID_JOB_ID, FAULTLINE_PROTOCOL_INVALID_WORKER_ID,
            FAULTLINE_PROTOCOL_INVALID_ATTEMPT
        };
        const size_t offsets[] = {12, 20, 24}, widths[] = {8, 4, 8};
        for (size_t field = 0; field < (i == 1 ? 1u : 3u); ++field) {
            struct faultline_message message = examples[i].message;
            struct faultline_job_identity *identity = identity_of(&message);
            uint8_t wire[41];
            if (i == 1) { message.payload.job_submit_ack = 0; }
            else if (field == 0) { identity->job_id = 0; }
            else if (field == 1) { identity->worker_id = 0; }
            else { identity->attempt = 0; }
            CHECK(encode_error(&message, sizeof(wire), errors[field]) == EXIT_SUCCESS);
            memcpy(wire, examples[i].wire, sizeof(wire));
            memset(wire + offsets[field], 0, widths[field]);
            CHECK(decode_error(wire, examples[i].size, errors[field]) == EXIT_SUCCESS);
        }
    }
    const uint16_t invalid[] = {0, 5, UINT16_MAX};
    for (size_t i = 0; i <= 2; i += 2) {
        for (size_t j = 0; j < 3; ++j) {
            struct faultline_message message = examples[i].message;
            uint8_t wire[41];
            size_t offset = i == 0 ? 12 : 32;
            if (i == 0) { message.payload.job_submit.task_type = invalid[j]; }
            else { message.payload.job_assign.task_type = invalid[j]; }
            CHECK(encode_error(&message, sizeof(wire), FAULTLINE_PROTOCOL_INVALID_TASK_TYPE) == EXIT_SUCCESS);
            memcpy(wire, examples[i].wire, sizeof(wire));
            wire[offset] = (uint8_t)(invalid[j] >> 8);
            wire[offset + 1] = (uint8_t)invalid[j];
            CHECK(decode_error(wire, examples[i].size, FAULTLINE_PROTOCOL_INVALID_TASK_TYPE) == EXIT_SUCCESS);
        }
    }
    const uint16_t failures[] = {FAULTLINE_JOB_FAILURE_NONE, FAULTLINE_JOB_FAILURE_WORKER_LOST, UINT16_MAX};
    for (size_t i = 0; i < 3; ++i) {
        struct faultline_message message = examples[5].message;
        uint8_t wire[41];
        message.payload.job_failed.failure = failures[i];
        CHECK(encode_error(&message, sizeof(wire), FAULTLINE_PROTOCOL_INVALID_FAILURE) == EXIT_SUCCESS);
        memcpy(wire, examples[5].wire, sizeof(wire));
        wire[32] = (uint8_t)(failures[i] >> 8);
        wire[33] = (uint8_t)failures[i];
        CHECK(decode_error(wire, examples[5].size, FAULTLINE_PROTOCOL_INVALID_FAILURE) == EXIT_SUCCESS);
    }
    return EXIT_SUCCESS;
}

static int test_invalid_lengths(void)
{
    const uint8_t minimums[] = {10, 8, 26, 20, 24, 22};
    for (size_t i = 0; i < EXAMPLE_COUNT; ++i) {
        uint8_t wire[41];
        memcpy(wire, examples[i].wire, sizeof(wire));
        wire[11] = (uint8_t)(minimums[i] - 1);
        CHECK(decode_error(wire, 12, FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH) == EXIT_SUCCESS);
        if (i == 1 || i == 3 || i == 5) {
            wire[11] = (uint8_t)(minimums[i] + 1);
            CHECK(decode_error(wire, 12, FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH) == EXIT_SUCCESS);
            continue;
        }
        /* Outer size exceeds this message's bound, even though below 1 MiB. */
        wire[10] = 4;
        wire[11] = (uint8_t)(minimums[i] + 1);
        CHECK(decode_error(wire, 12, FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE) == EXIT_SUCCESS);
        const uint8_t inner_lengths[][4] = {{0, 0, 0, 2}, {0, 0, 0, 4},
                                           {0, 0, 4, 1}, {255, 255, 255, 255}};
        for (size_t j = 0; j < 4; ++j) {
            memcpy(wire, examples[i].wire, sizeof(wire));
            memcpy(wire + 12 + minimums[i] - 4, inner_lengths[j], 4);
            /* Detect inconsistent/huge inner lengths with only the prefix present. */
            CHECK(decode_error(wire, 12 + minimums[i], j < 2 ?
                  FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH : FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE) == EXIT_SUCCESS);
        }
        memcpy(wire, examples[i].wire, sizeof(wire));
        wire[11] = (uint8_t)(minimums[i] + 4);
        CHECK(decode_error(wire, examples[i].size, FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH) == EXIT_SUCCESS);
    }
    return EXIT_SUCCESS;
}

static int test_mixed_stream(void)
{
    const uint8_t heartbeat[] = {0x46, 0x4c, 0x49, 0x4e, 0, 1, 0, 5, 0, 0, 0, 4, 0, 0, 0, 7};
    uint8_t stream[256];
    size_t size = 0, offset = 0, consumed = 0;
    struct faultline_message decoded;
    for (size_t i = 0; i < EXAMPLE_COUNT; ++i) {
        memcpy(stream + size, examples[i].wire, examples[i].size);
        size += examples[i].size;
        if (i == 2) { memcpy(stream + size, heartbeat, sizeof(heartbeat)); size += sizeof(heartbeat); }
    }
    /* All earlier frames followed by an incomplete failed report. */
    for (size_t i = 0; i < EXAMPLE_COUNT - 1; ++i) {
        CHECK(faultline_message_decode(stream + offset, size - 1 - offset,
                                       &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
        CHECK(consumed == examples[i].size);
        CHECK(messages_equal(&decoded, &examples[i].message));
        offset += consumed;
        if (i == 2) {
            CHECK(faultline_message_decode(stream + offset, size - 1 - offset,
                                           &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
            CHECK(consumed == sizeof(heartbeat));
            CHECK(decoded.message_type == FAULTLINE_MSG_HEARTBEAT && decoded.payload.worker_id == 7);
            offset += consumed;
        }
    }
    CHECK(decode_error(stream + offset, size - 1 - offset, FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL) == EXIT_SUCCESS);
    CHECK(faultline_message_decode(stream + offset, size - offset, &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
    CHECK(messages_equal(&decoded, &examples[5].message));
    CHECK(offset + consumed == size);
    return EXIT_SUCCESS;
}

static int test_model_report_identity(void)
{
    struct faultline_job job;
    struct faultline_message report = {.message_type = FAULTLINE_MSG_JOB_STARTED,
                                       .payload.job_started = {42, 7, 1}};
    struct faultline_message decoded;
    uint8_t wire[FAULTLINE_MESSAGE_MAX_FRAME_SIZE];
    size_t written = 0, consumed = 0;
    CHECK(faultline_job_init(&job, 42, FAULTLINE_TASK_SLEEP, NULL, 0, 1, 100) == FAULTLINE_JOB_OK);
    CHECK(faultline_job_assign(&job, 7, 101) == FAULTLINE_JOB_OK);
    CHECK(faultline_message_encode(wire, sizeof(wire), &report, &written) == FAULTLINE_PROTOCOL_OK);
    CHECK(faultline_message_decode(wire, written, &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
    const struct faultline_job_identity *identity = &decoded.payload.job_started;
    CHECK(identity->job_id == job.id);
    CHECK(faultline_job_start(&job, identity->worker_id, identity->attempt, 102) == FAULTLINE_JOB_OK);
    CHECK(faultline_job_fail(&job, 7, 1, FAULTLINE_JOB_FAILURE_TASK, 103) == FAULTLINE_JOB_OK);
    CHECK(faultline_job_assign(&job, 7, 104) == FAULTLINE_JOB_OK);
    /* Codec accepts well-formed bytes; model rejects the now-stale attempt. */
    CHECK(faultline_message_decode(wire, written, &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
    CHECK(faultline_job_start(&job, identity->worker_id, identity->attempt, 105) == FAULTLINE_JOB_STALE_ATTEMPT);
    CHECK(job.state == FAULTLINE_JOB_ASSIGNED && job.attempt == 2);
    return EXIT_SUCCESS;
}

int main(void)
{
    const struct { const char *name; int (*run)(void); } tests[] = {
        {"literal job frames and unaligned buffers", test_literal_frames},
        {"job frame truncation and short output buffers", test_partial_frames},
        {"job data bounds and decoded ownership", test_data_boundaries_and_ownership},
        {"job message integer boundaries and task IDs", test_integer_boundaries},
        {"invalid job message fields", test_invalid_fields},
        {"job outer and inner length validation", test_invalid_lengths},
        {"mixed lifecycle/job stream and trailing partial frame", test_mixed_stream},
        {"decoded report identity and stale model attempt", test_model_report_identity}
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (tests[i].run() != EXIT_SUCCESS) { fprintf(stderr, "FAIL: %s\n", tests[i].name); return EXIT_FAILURE; }
        printf("PASS: %s\n", tests[i].name);
    }
    return EXIT_SUCCESS;
}
