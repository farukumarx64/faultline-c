#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Unlike assert(), these checks still run when compiled with NDEBUG. */
#define CHECK(condition)                                                     \
    do                                                                       \
    {                                                                        \
        if (!(condition))                                                    \
        {                                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                             \
            return EXIT_FAILURE;                                             \
        }                                                                    \
    } while (0)

static const struct faultline_header ping = {
    .magic = FAULTLINE_PROTOCOL_MAGIC,
    .version = FAULTLINE_PROTOCOL_VERSION,
    .message_type = FAULTLINE_MSG_PING,
    .payload_length = 0};

/* Literal bytes are an independent oracle for field order and byte order. */
static const uint8_t ping_wire[FAULTLINE_HEADER_SIZE] = {
    0x46, 0x4c, 0x49, 0x4e, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00};

static int headers_equal(const struct faultline_header *left,
                         const struct faultline_header *right)
{
    return left->magic == right->magic && left->version == right->version &&
           left->message_type == right->message_type &&
           left->payload_length == right->payload_length;
}

static int test_known_wire_bytes(void)
{
    const struct faultline_header pong = {
        .magic = FAULTLINE_PROTOCOL_MAGIC,
        .version = FAULTLINE_PROTOCOL_VERSION,
        .message_type = FAULTLINE_MSG_PONG,
        .payload_length = UINT32_C(0x00010203)};
    const uint8_t pong_wire[FAULTLINE_HEADER_SIZE] = {
        0x46, 0x4c, 0x49, 0x4e, 0x00, 0x01, 0x00, 0x02, 0x00, 0x01, 0x02, 0x03};
    uint8_t wire[FAULTLINE_HEADER_SIZE];
    struct faultline_header decoded = {0};

    CHECK(faultline_header_encode(wire, sizeof(wire), &ping) ==
          FAULTLINE_PROTOCOL_OK);
    CHECK(memcmp(wire, ping_wire, sizeof(wire)) == 0);
    CHECK(faultline_header_decode(ping_wire, sizeof(ping_wire), &decoded) ==
          FAULTLINE_PROTOCOL_OK);
    CHECK(headers_equal(&decoded, &ping));

    CHECK(faultline_header_encode(wire, sizeof(wire), &pong) ==
          FAULTLINE_PROTOCOL_OK);
    CHECK(memcmp(wire, pong_wire, sizeof(wire)) == 0);
    CHECK(faultline_header_decode(pong_wire, sizeof(pong_wire), &decoded) ==
          FAULTLINE_PROTOCOL_OK);
    CHECK(headers_equal(&decoded, &pong));
    return EXIT_SUCCESS;
}

static int test_payload_boundaries(void)
{
    const uint32_t lengths[] = {
        0, 1, 255, 256, 65535, 65536, FAULTLINE_MAX_PAYLOAD_SIZE};
    const uint8_t max_wire[FAULTLINE_HEADER_SIZE] = {
        0x46, 0x4c, 0x49, 0x4e, 0x00, 0x01, 0x00, 0x01, 0x00, 0x10, 0x00, 0x00};
    struct faultline_header header = ping;
    struct faultline_header decoded = {0};
    uint8_t wire[FAULTLINE_HEADER_SIZE];

    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i)
    {
        header.payload_length = lengths[i];
        CHECK(faultline_header_encode(wire, sizeof(wire), &header) ==
              FAULTLINE_PROTOCOL_OK);
        CHECK(faultline_header_decode(wire, sizeof(wire), &decoded) ==
              FAULTLINE_PROTOCOL_OK);
        CHECK(headers_equal(&decoded, &header));
    }
    CHECK(memcmp(wire, max_wire, sizeof(wire)) == 0);
    return EXIT_SUCCESS;
}

static int test_short_buffers(void)
{
    uint8_t output[FAULTLINE_HEADER_SIZE];
    uint8_t original[FAULTLINE_HEADER_SIZE];
    struct faultline_header decoded = ping;

    memset(original, 0xa5, sizeof(original));
    for (size_t size = 0; size < FAULTLINE_HEADER_SIZE; ++size)
    {
        /* Exact allocations let ASan catch reads beyond a truncated input. */
        uint8_t *input = malloc(size == 0 ? 1 : size);

        CHECK(input != NULL);
        memcpy(input, ping_wire, size);
        memcpy(output, original, sizeof(output));
        const enum faultline_protocol_result result =
            faultline_header_decode(input, size, &decoded);
        free(input);
        CHECK(result == FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL);
        CHECK(headers_equal(&decoded, &ping));
        CHECK(faultline_header_encode(output, size, &ping) ==
              FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL);
        CHECK(memcmp(output, original, sizeof(output)) == 0);
    }
    return EXIT_SUCCESS;
}

static int test_invalid_headers(void)
{
    const struct
    {
        size_t offset;
        uint8_t replacement;
        enum faultline_protocol_result result;
    } cases[] = {
        {0, 0x00, FAULTLINE_PROTOCOL_BAD_MAGIC},
        {4, 0x01, FAULTLINE_PROTOCOL_UNSUPPORTED_VERSION},
        {5, 0x00, FAULTLINE_PROTOCOL_UNSUPPORTED_VERSION},
        {5, 0x02, FAULTLINE_PROTOCOL_UNSUPPORTED_VERSION},
        {6, 0x01, FAULTLINE_PROTOCOL_UNKNOWN_MESSAGE_TYPE},
        {7, 0x00, FAULTLINE_PROTOCOL_UNKNOWN_MESSAGE_TYPE},
        {7, 0x0f, FAULTLINE_PROTOCOL_UNKNOWN_MESSAGE_TYPE},
        {8, 0xff, FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE}};
    const uint8_t oversized_wire[FAULTLINE_HEADER_SIZE] = {
        0x46, 0x4c, 0x49, 0x4e, 0x00, 0x01, 0x00, 0x01, 0x00, 0x10, 0x00, 0x01};
    uint8_t wire[FAULTLINE_HEADER_SIZE];
    struct faultline_header decoded = ping;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        memcpy(wire, ping_wire, sizeof(wire));
        wire[cases[i].offset] = cases[i].replacement;
        CHECK(faultline_header_decode(wire, sizeof(wire), &decoded) ==
              cases[i].result);
        CHECK(headers_equal(&decoded, &ping));
    }
    CHECK(faultline_header_decode(oversized_wire, sizeof(oversized_wire), &decoded) ==
          FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE);
    CHECK(headers_equal(&decoded, &ping));

    /* A sender using little-endian field order must not be accepted. */
    const uint8_t little_endian_wire[FAULTLINE_HEADER_SIZE] = {
        0x4e, 0x49, 0x4c, 0x46, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK(faultline_header_decode(little_endian_wire, sizeof(little_endian_wire),
                                  &decoded) == FAULTLINE_PROTOCOL_BAD_MAGIC);
    CHECK(headers_equal(&decoded, &ping));
    return EXIT_SUCCESS;
}

static int test_invalid_encode(void)
{
    struct faultline_header bad_headers[] = {ping, ping, ping, ping, ping, ping};
    const enum faultline_protocol_result expected[] = {
        FAULTLINE_PROTOCOL_BAD_MAGIC,
        FAULTLINE_PROTOCOL_UNSUPPORTED_VERSION,
        FAULTLINE_PROTOCOL_UNKNOWN_MESSAGE_TYPE,
        FAULTLINE_PROTOCOL_UNKNOWN_MESSAGE_TYPE,
        FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE,
        FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE};
    uint8_t wire[FAULTLINE_HEADER_SIZE];
    uint8_t original[FAULTLINE_HEADER_SIZE];

    bad_headers[0].magic = 0;
    bad_headers[1].version = 2;
    bad_headers[2].message_type = 0;
    bad_headers[3].message_type = UINT16_MAX;
    bad_headers[4].payload_length = FAULTLINE_MAX_PAYLOAD_SIZE + UINT32_C(1);
    bad_headers[5].payload_length = UINT32_MAX;
    memset(original, 0xa5, sizeof(original));

    for (size_t i = 0; i < sizeof(bad_headers) / sizeof(bad_headers[0]); ++i)
    {
        memcpy(wire, original, sizeof(wire));
        CHECK(faultline_header_encode(wire, sizeof(wire), &bad_headers[i]) ==
              expected[i]);
        CHECK(memcmp(wire, original, sizeof(wire)) == 0);
    }
    return EXIT_SUCCESS;
}

static int test_null_arguments(void)
{
    uint8_t wire[FAULTLINE_HEADER_SIZE];
    struct faultline_header decoded = ping;

    memcpy(wire, ping_wire, sizeof(wire));
    CHECK(faultline_header_encode(NULL, sizeof(wire), &ping) ==
          FAULTLINE_PROTOCOL_INVALID_ARGUMENT);
    CHECK(faultline_header_encode(wire, sizeof(wire), NULL) ==
          FAULTLINE_PROTOCOL_INVALID_ARGUMENT);
    CHECK(memcmp(wire, ping_wire, sizeof(wire)) == 0);
    CHECK(faultline_header_decode(NULL, sizeof(wire), &decoded) ==
          FAULTLINE_PROTOCOL_INVALID_ARGUMENT);
    CHECK(faultline_header_decode(wire, sizeof(wire), NULL) ==
          FAULTLINE_PROTOCOL_INVALID_ARGUMENT);
    CHECK(headers_equal(&decoded, &ping));
    return EXIT_SUCCESS;
}

static int test_unaligned_and_trailing_bytes(void)
{
    _Alignas(uint32_t) uint8_t storage[FAULTLINE_HEADER_SIZE + 3u];
    struct faultline_header decoded = {0};

    memset(storage, 0xa5, sizeof(storage));
    CHECK(faultline_header_encode(storage + 1, sizeof(storage) - 1, &ping) ==
          FAULTLINE_PROTOCOL_OK);
    CHECK(storage[0] == 0xa5);
    CHECK(memcmp(storage + 1, ping_wire, sizeof(ping_wire)) == 0);
    CHECK(storage[FAULTLINE_HEADER_SIZE + 1u] == 0xa5);
    CHECK(storage[FAULTLINE_HEADER_SIZE + 2u] == 0xa5);
    CHECK(faultline_header_decode(storage + 1, sizeof(storage) - 1, &decoded) ==
          FAULTLINE_PROTOCOL_OK);
    CHECK(headers_equal(&decoded, &ping));
    return EXIT_SUCCESS;
}

int main(void)
{
    const struct
    {
        const char *name;
        int (*run)(void);
    } tests[] = {
        {"known wire bytes", test_known_wire_bytes},
        {"payload boundaries", test_payload_boundaries},
        {"short buffers", test_short_buffers},
        {"invalid headers", test_invalid_headers},
        {"invalid encode", test_invalid_encode},
        {"null arguments", test_null_arguments},
        {"unaligned and trailing bytes", test_unaligned_and_trailing_bytes}};

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)
    {
        if (tests[i].run() != EXIT_SUCCESS)
        {
            fprintf(stderr, "FAIL: %s\n", tests[i].name);
            return EXIT_FAILURE;
        }
        printf("PASS: %s\n", tests[i].name);
    }
    return EXIT_SUCCESS;
}
