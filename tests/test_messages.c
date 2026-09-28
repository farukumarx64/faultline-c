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

/* Literal whole frames check the header, length, and payload independently. */
static const struct {
    struct faultline_message message;
    size_t size;
    uint8_t wire[16];
} examples[] = {
    {{.message_type = FAULTLINE_MSG_PING, .payload.worker_id = 0}, 12,
     {0x46, 0x4c, 0x49, 0x4e, 0x00, 0x01, 0x00, 0x01, 0, 0, 0, 0}},
    {{.message_type = FAULTLINE_MSG_PONG, .payload.worker_id = 0}, 12,
     {0x46, 0x4c, 0x49, 0x4e, 0x00, 0x01, 0x00, 0x02, 0, 0, 0, 0}},
    {{.message_type = FAULTLINE_MSG_WORKER_REGISTER, .payload.worker_id = 0}, 12,
     {0x46, 0x4c, 0x49, 0x4e, 0x00, 0x01, 0x00, 0x03, 0, 0, 0, 0}},
    {{.message_type = FAULTLINE_MSG_WORKER_REGISTER_ACK, .payload.worker_id = UINT32_C(0x01020304)}, 16,
     {0x46, 0x4c, 0x49, 0x4e, 0x00, 0x01, 0x00, 0x04, 0, 0, 0, 4,
      0x01, 0x02, 0x03, 0x04}},
    {{.message_type = FAULTLINE_MSG_HEARTBEAT, .payload.worker_id = UINT32_C(0x01020304)}, 16,
     {0x46, 0x4c, 0x49, 0x4e, 0x00, 0x01, 0x00, 0x05, 0, 0, 0, 4,
      0x01, 0x02, 0x03, 0x04}}
};

static int messages_equal(const struct faultline_message *left,
                          const struct faultline_message *right)
{
    return left->message_type == right->message_type &&
           left->payload.worker_id == right->payload.worker_id;
}

static int test_known_frames(void)
{
    for (size_t i = 0; i < sizeof(examples) / sizeof(examples[0]); ++i) {
        _Alignas(uint32_t) uint8_t storage[18];
        struct faultline_message decoded = {0};
        size_t written = 99;
        size_t consumed = 99;

        memset(storage, 0xa5, sizeof(storage));
        CHECK(faultline_message_encode(storage + 1, sizeof(storage) - 1,
                                       &examples[i].message, &written) ==
              FAULTLINE_PROTOCOL_OK);
        CHECK(written == examples[i].size);
        CHECK(memcmp(storage + 1, examples[i].wire, written) == 0);
        CHECK(storage[0] == 0xa5);
        for (size_t byte = written + 1; byte < sizeof(storage); ++byte) {
            CHECK(storage[byte] == 0xa5);
        }
        /* Decode independent literal bytes at an unaligned address. */
        memcpy(storage + 1, examples[i].wire, examples[i].size);
        CHECK(faultline_message_decode(storage + 1, sizeof(storage) - 1,
                                       &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
        CHECK(consumed == examples[i].size);
        CHECK(messages_equal(&decoded, &examples[i].message));
    }
    return EXIT_SUCCESS;
}

static int test_worker_id_boundaries(void)
{
    const struct {
        uint32_t id;
        uint8_t bytes[4];
    } ids[] = {
        {1, {0, 0, 0, 1}},
        {12, {0, 0, 0, 12}},
        {UINT32_C(0x01020304), {1, 2, 3, 4}},
        {UINT32_MAX, {0xff, 0xff, 0xff, 0xff}}
    };
    const uint16_t types[] = {
        FAULTLINE_MSG_WORKER_REGISTER_ACK, FAULTLINE_MSG_HEARTBEAT
    };

    for (size_t type = 0; type < sizeof(types) / sizeof(types[0]); ++type) {
        for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
            const struct faultline_message message = {.message_type = types[type], .payload.worker_id = ids[i].id};
            struct faultline_message decoded = {0};
            uint8_t wire[16];
            size_t size = 0;

            CHECK(faultline_message_encode(wire, sizeof(wire), &message, &size) ==
                  FAULTLINE_PROTOCOL_OK);
            CHECK(size == 16);
            CHECK(memcmp(wire + 12, ids[i].bytes, 4) == 0);
            CHECK(faultline_message_decode(wire, size, &decoded, &size) ==
                  FAULTLINE_PROTOCOL_OK);
            CHECK(messages_equal(&decoded, &message));
        }
    }
    return EXIT_SUCCESS;
}

static int test_incomplete_frames(void)
{
    const struct faultline_message sentinel = {.message_type = FAULTLINE_MSG_HEARTBEAT, .payload.worker_id = 99};
    uint8_t original[16];

    memset(original, 0xa5, sizeof(original));
    for (size_t i = 0; i < sizeof(examples) / sizeof(examples[0]); ++i) {
        for (size_t size = 0; size < examples[i].size; ++size) {
            /* ASan can catch a decoder reading beyond the available prefix. */
            uint8_t *input = malloc(size == 0 ? 1 : size);
            uint8_t output[16];
            struct faultline_message decoded = sentinel;
            size_t consumed = 99;
            size_t written = 99;

            CHECK(input != NULL);
            memcpy(input, examples[i].wire, size);
            const enum faultline_protocol_result result =
                faultline_message_decode(input, size, &decoded, &consumed);
            free(input);
            CHECK(result == FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL);
            CHECK(messages_equal(&decoded, &sentinel));
            CHECK(consumed == 99);

            memcpy(output, original, sizeof(output));
            CHECK(faultline_message_encode(output, size, &examples[i].message,
                                           &written) == FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL);
            CHECK(memcmp(output, original, sizeof(output)) == 0);
            CHECK(written == 99);
        }
    }
    return EXIT_SUCCESS;
}

static int test_invalid_frames(void)
{
    const struct faultline_message sentinel = {.message_type = FAULTLINE_MSG_PONG, .payload.worker_id = 0};
    const struct {
        uint32_t length;
        uint8_t bytes[4];
    } lengths[] = {
        {0, {0, 0, 0, 0}}, {1, {0, 0, 0, 1}}, {3, {0, 0, 0, 3}},
        {4, {0, 0, 0, 4}}, {5, {0, 0, 0, 5}},
        {1048576, {0, 0x10, 0, 0}}, {1048577, {0, 0x10, 0, 1}}
    };
    const struct {
        size_t offset;
        uint8_t byte;
        enum faultline_protocol_result result;
    } bad_headers[] = {
        {0, 0, FAULTLINE_PROTOCOL_BAD_MAGIC},
        {5, 2, FAULTLINE_PROTOCOL_UNSUPPORTED_VERSION},
        {7, 21, FAULTLINE_PROTOCOL_UNKNOWN_MESSAGE_TYPE}
    };
    struct faultline_message decoded = sentinel;
    size_t consumed = 99;
    uint8_t wire[16];

    for (size_t i = 0; i < sizeof(examples) / sizeof(examples[0]); ++i) {
        for (size_t j = 0; j < sizeof(lengths) / sizeof(lengths[0]); ++j) {
            if (lengths[j].length == examples[i].size - FAULTLINE_HEADER_SIZE) {
                continue;
            }
            memcpy(wire, examples[i].wire, sizeof(wire));
            memcpy(wire + 8, lengths[j].bytes, 4);
            const enum faultline_protocol_result expected =
                lengths[j].length > FAULTLINE_MAX_PAYLOAD_SIZE ?
                FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE : FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH;
            /* A bad declaration is rejected with only its header available. */
            CHECK(faultline_message_decode(wire, 12, &decoded, &consumed) == expected);
            CHECK(messages_equal(&decoded, &sentinel));
            CHECK(consumed == 99);
        }
        if (examples[i].message.payload.worker_id != 0) {
            memcpy(wire, examples[i].wire, sizeof(wire));
            memset(wire + 12, 0, 4);
            CHECK(faultline_message_decode(wire, sizeof(wire), &decoded, &consumed) ==
                  FAULTLINE_PROTOCOL_INVALID_WORKER_ID);
            CHECK(messages_equal(&decoded, &sentinel));
            CHECK(consumed == 99);
        }
    }
    for (size_t i = 0; i < sizeof(bad_headers) / sizeof(bad_headers[0]); ++i) {
        memcpy(wire, examples[3].wire, sizeof(wire));
        wire[bad_headers[i].offset] = bad_headers[i].byte;
        CHECK(faultline_message_decode(wire, sizeof(wire), &decoded, &consumed) ==
              bad_headers[i].result);
        CHECK(messages_equal(&decoded, &sentinel));
        CHECK(consumed == 99);
    }
    return EXIT_SUCCESS;
}

static int test_invalid_message_encode(void)
{
    const struct {
        struct faultline_message message;
        enum faultline_protocol_result result;
    } cases[] = {
        {{.message_type = 0, .payload.worker_id = 0}, FAULTLINE_PROTOCOL_UNKNOWN_MESSAGE_TYPE},
        {{.message_type = UINT16_MAX, .payload.worker_id = 0}, FAULTLINE_PROTOCOL_UNKNOWN_MESSAGE_TYPE},
        {{.message_type = FAULTLINE_MSG_PING, .payload.worker_id = 1}, FAULTLINE_PROTOCOL_INVALID_WORKER_ID},
        {{.message_type = FAULTLINE_MSG_PONG, .payload.worker_id = 1}, FAULTLINE_PROTOCOL_INVALID_WORKER_ID},
        {{.message_type = FAULTLINE_MSG_WORKER_REGISTER, .payload.worker_id = 1}, FAULTLINE_PROTOCOL_INVALID_WORKER_ID},
        {{.message_type = FAULTLINE_MSG_WORKER_REGISTER_ACK, .payload.worker_id = 0}, FAULTLINE_PROTOCOL_INVALID_WORKER_ID},
        {{.message_type = FAULTLINE_MSG_HEARTBEAT, .payload.worker_id = 0}, FAULTLINE_PROTOCOL_INVALID_WORKER_ID}
    };
    uint8_t original[16];

    memset(original, 0xa5, sizeof(original));
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint8_t wire[16];
        size_t written = 99;

        memcpy(wire, original, sizeof(wire));
        CHECK(faultline_message_encode(wire, sizeof(wire), &cases[i].message,
                                       &written) == cases[i].result);
        CHECK(memcmp(wire, original, sizeof(wire)) == 0);
        CHECK(written == 99);
    }
    return EXIT_SUCCESS;
}

static int test_null_arguments(void)
{
    uint8_t wire[16];
    struct faultline_message message = examples[3].message;
    size_t size = 99;

    memcpy(wire, examples[3].wire, sizeof(wire));
    CHECK(faultline_message_encode(NULL, sizeof(wire), &message, &size) ==
          FAULTLINE_PROTOCOL_INVALID_ARGUMENT);
    CHECK(faultline_message_encode(wire, sizeof(wire), NULL, &size) ==
          FAULTLINE_PROTOCOL_INVALID_ARGUMENT);
    CHECK(faultline_message_encode(wire, sizeof(wire), &message, NULL) ==
          FAULTLINE_PROTOCOL_INVALID_ARGUMENT);
    CHECK(memcmp(wire, examples[3].wire, sizeof(wire)) == 0);
    CHECK(size == 99);
    CHECK(faultline_message_decode(NULL, sizeof(wire), &message, &size) ==
          FAULTLINE_PROTOCOL_INVALID_ARGUMENT);
    CHECK(faultline_message_decode(wire, sizeof(wire), NULL, &size) ==
          FAULTLINE_PROTOCOL_INVALID_ARGUMENT);
    CHECK(faultline_message_decode(wire, sizeof(wire), &message, NULL) ==
          FAULTLINE_PROTOCOL_INVALID_ARGUMENT);
    CHECK(messages_equal(&message, &examples[3].message));
    CHECK(size == 99);
    return EXIT_SUCCESS;
}

static int test_consecutive_and_fragmented_frames(void)
{
    uint8_t stream[44];
    struct faultline_message decoded = {0};
    size_t consumed = 0;
    size_t offset = 0;

    /* A registration, its ACK, and only part of a heartbeat are buffered. */
    memcpy(stream, examples[2].wire, 12);
    memcpy(stream + 12, examples[3].wire, 16);
    memcpy(stream + 28, examples[4].wire, 14);
    for (size_t i = 2; i <= 3; ++i) {
        CHECK(faultline_message_decode(stream + offset, 42 - offset,
                                       &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
        CHECK(messages_equal(&decoded, &examples[i].message));
        CHECK(consumed == examples[i].size);
        offset += consumed;
    }
    CHECK(offset == 28);
    for (size_t available = 14; available < 16; ++available) {
        consumed = 99;
        CHECK(faultline_message_decode(stream + offset, available,
                                       &decoded, &consumed) == FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL);
        CHECK(messages_equal(&decoded, &examples[3].message));
        CHECK(consumed == 99);
        stream[offset + available] = examples[4].wire[available];
    }
    CHECK(faultline_message_decode(stream + offset, 16, &decoded, &consumed) ==
          FAULTLINE_PROTOCOL_OK);
    CHECK(consumed == 16);
    CHECK(messages_equal(&decoded, &examples[4].message));
    return EXIT_SUCCESS;
}

int main(void)
{
    const struct {
        const char *name;
        int (*run)(void);
    } tests[] = {
        {"known message frames and unaligned buffers", test_known_frames},
        {"worker ID boundaries", test_worker_id_boundaries},
        {"incomplete frames and short output buffers", test_incomplete_frames},
        {"invalid message frames", test_invalid_frames},
        {"invalid message encode", test_invalid_message_encode},
        {"message null arguments", test_null_arguments},
        {"consecutive and fragmented frames", test_consecutive_and_fragmented_frames}
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (tests[i].run() != EXIT_SUCCESS) {
            fprintf(stderr, "FAIL: %s\n", tests[i].name);
            return EXIT_FAILURE;
        }
        printf("PASS: %s\n", tests[i].name);
    }
    return EXIT_SUCCESS;
}
