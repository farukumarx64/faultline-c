#include "protocol.h"
#include "wal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return EXIT_FAILURE; } } while (0)

#include "fixtures/wal_keyed_creation.h"

static const uint8_t keyed_submit[] = {
    0x46,0x4c,0x49,0x4e, 0,1, 0,21, 0,0,0,29,
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
    0,4, 0,0,0,2, 0,0,0,3, 'a',0,255
};

static int test_messages(void)
{
    struct faultline_message input = {.message_type = FAULTLINE_MSG_JOB_SUBMIT_KEYED,
        .payload.job_submit = {.task_type = FAULTLINE_TASK_HASH, .max_retries = 2,
                              .argument_size = 3, .arguments = {'a',0,255}}};
    for (size_t i = 0; i < FAULTLINE_REQUEST_KEY_SIZE; ++i) { input.payload.job_submit.request_key[i] = (uint8_t)i; }
    uint8_t wire[FAULTLINE_MESSAGE_MAX_FRAME_SIZE];
    size_t written = 99, consumed = 99;
    CHECK(faultline_message_encode(wire, sizeof(wire), &input, &written) == FAULTLINE_PROTOCOL_OK);
    CHECK(written == sizeof(keyed_submit) && memcmp(wire, keyed_submit, written) == 0);
    struct faultline_message decoded, before;
    memset(&before, 0xa5, sizeof(before));
    for (size_t cut = 0; cut < written; ++cut) {
        uint8_t *partial = malloc(cut == 0 ? 1 : cut);
        CHECK(partial != NULL);
        memcpy(partial, wire, cut);
        decoded = before;
        CHECK(faultline_message_decode(partial, cut, &decoded, &consumed) == FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL);
        CHECK(consumed == 99 && memcmp(&decoded, &before, sizeof(decoded)) == 0);
        free(partial);
    }
    CHECK(faultline_message_decode(wire, written, &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
    CHECK(memcmp(decoded.payload.job_submit.request_key, input.payload.job_submit.request_key, 16) == 0);
    CHECK(memcmp(decoded.payload.job_submit.arguments, "a\0\xff", 3) == 0);
    input.payload.job_submit.argument_size = 1024;
    CHECK(faultline_message_encode(wire, sizeof(wire), &input, &written) == FAULTLINE_PROTOCOL_OK);
    CHECK(written == 12 + 26 + 1024);
    CHECK(faultline_message_decode(wire, written, &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
    input.payload.job_submit.argument_size = SIZE_MAX;
    CHECK(faultline_message_encode(wire, sizeof(wire), &input, &written) == FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE);
    input.payload.job_submit.argument_size = 3;
    input.message_type = FAULTLINE_MSG_JOB_SUBMIT;
    CHECK(faultline_message_encode(wire, sizeof(wire), &input, &written) == FAULTLINE_PROTOCOL_INVALID_REQUEST_KEY);
    input.message_type = FAULTLINE_MSG_JOB_SUBMIT_KEYED;
    memset(input.payload.job_submit.request_key, 0, 16);
    CHECK(faultline_message_encode(wire, sizeof(wire), &input, &written) == FAULTLINE_PROTOCOL_INVALID_REQUEST_KEY);
    memcpy(wire, keyed_submit, sizeof(keyed_submit)); memset(wire + 12, 0, 16);
    CHECK(faultline_message_decode(wire, sizeof(keyed_submit), &decoded, &consumed) == FAULTLINE_PROTOCOL_INVALID_REQUEST_KEY);
    input = (struct faultline_message){.message_type = FAULTLINE_MSG_JOB_SUBMIT_CONFLICT};
    for (size_t i = 0; i < 16; ++i) { input.payload.job_submit_conflict[i] = (uint8_t)i; }
    CHECK(faultline_message_encode(wire, sizeof(wire), &input, &written) == FAULTLINE_PROTOCOL_OK);
    CHECK(written == 28 && wire[7] == 22 && wire[11] == 16 && memcmp(wire + 12, keyed_submit + 12, 16) == 0);
    CHECK(faultline_message_decode(wire, written, &decoded, &consumed) == FAULTLINE_PROTOCOL_OK);
    CHECK(memcmp(decoded.payload.job_submit_conflict, input.payload.job_submit_conflict, 16) == 0);
    return EXIT_SUCCESS;
}

static int test_wal_key(void)
{
    struct faultline_wal_record record = {.type = FAULTLINE_WAL_JOB_CREATED_KEYED, .sequence = 1};
    CHECK(faultline_job_init(&record.payload.job, 1, FAULTLINE_TASK_HASH, (const uint8_t *)"a\0\xff", 3, 2, 10) == FAULTLINE_JOB_OK);
    for (size_t i = 0; i < 16; ++i) { record.payload.job.request_key[i] = (uint8_t)i; }
    uint8_t wire[FAULTLINE_WAL_MAX_RECORD_SIZE];
    size_t written, consumed = 99;
    CHECK(faultline_wal_record_encode(wire, sizeof(wire), &record, &written) == FAULTLINE_WAL_OK);
    CHECK(written == sizeof(keyed_creation) && memcmp(wire, keyed_creation, written) == 0);
    struct faultline_wal_record decoded, before;
    memset(&before, 0xa5, sizeof(before));
    for (size_t cut = 0; cut < written; ++cut) {
        decoded = before;
        CHECK(faultline_wal_record_decode(wire, cut, 1, &decoded, &consumed) == FAULTLINE_WAL_INCOMPLETE);
        CHECK(consumed == 99 && memcmp(&decoded, &before, sizeof(decoded)) == 0);
    }
    CHECK(faultline_wal_record_decode(wire, written, 1, &decoded, &consumed) == FAULTLINE_WAL_OK);
    CHECK(memcmp(decoded.payload.job.request_key, record.payload.job.request_key, 16) == 0);
    wire[32] ^= 1;
    CHECK(faultline_wal_record_decode(wire, written, 1, &decoded, &consumed) == FAULTLINE_WAL_BAD_CHECKSUM);
    record.type = FAULTLINE_WAL_JOB_CREATED;
    CHECK(faultline_wal_record_encode(wire, sizeof(wire), &record, &written) == FAULTLINE_WAL_INVALID_PAYLOAD);
    record.type = FAULTLINE_WAL_JOB_CREATED_KEYED;
    memset(record.payload.job.request_key, 0, 16);
    CHECK(faultline_wal_record_encode(wire, sizeof(wire), &record, &written) == FAULTLINE_WAL_INVALID_PAYLOAD);
    return EXIT_SUCCESS;
}

int main(void)
{
    if (test_messages() != EXIT_SUCCESS || test_wal_key() != EXIT_SUCCESS) { return EXIT_FAILURE; }
    puts("PASS: keyed submission/conflict bytes, fragmentation, bounds, and key validation");
    puts("PASS: atomic keyed WAL creation vector, incomplete tails, checksum, and key validation");
    return EXIT_SUCCESS;
}
