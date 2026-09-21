#include "wal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            return EXIT_FAILURE;                                                \
        }                                                                       \
    } while (0)

#define SAMPLE_SEQUENCE UINT64_C(0x0102030405060708)
#define SAMPLE_JOB UINT64_C(0x0102030405060708)
#define SAMPLE_WORKER UINT32_C(0x11223344)

#include "fixtures/wal_v1_vectors.h"

static void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static void put_u32(uint8_t *out, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) { out[i] = (uint8_t)(value >> (24 - 8 * i)); }
}

static void put_u64(uint8_t *out, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) { out[i] = (uint8_t)(value >> (56 - 8 * i)); }
}

/* MSB-first reference, unlike the production reflected implementation. Used to
 * repair checksums after intentionally corrupting semantic fields. Literal
 * valid vectors above come from Python/zlib and do not use either C algorithm. */
static uint32_t reference_crc(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_MAX, reflected = 0;
    for (size_t i = 0; i < size; ++i) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            uint32_t feedback = (crc >> 31) ^ (((uint32_t)data[i] >> bit) & 1u);
            crc <<= 1;
            if (feedback != 0) { crc ^= UINT32_C(0x04c11db7); }
        }
    }
    for (unsigned bit = 0; bit < 32; ++bit) {
        reflected = (reflected << 1) | (crc & 1u);
        crc >>= 1;
    }
    return reflected ^ UINT32_MAX;
}

static void repair_record(uint8_t *wire, size_t size)
{
    put_u32(wire + 24, reference_crc(wire + 32, size - 32));
    put_u32(wire + 28, reference_crc(wire, 28));
}

static int make_record(struct faultline_wal_record *record, enum faultline_wal_record_type type)
{
    const uint8_t arguments[] = {0xaa, 0, 0xbb}, result[] = {0xcc, 0, 0xdd};
    *record = (struct faultline_wal_record){.type = type, .sequence = SAMPLE_SEQUENCE};
    if (type == FAULTLINE_WAL_WORKER_ID_ALLOCATED) {
        record->payload.worker_id = SAMPLE_WORKER;
        return EXIT_SUCCESS;
    }
    struct faultline_job *job = &record->payload.job;
    CHECK(faultline_job_init(job, SAMPLE_JOB, FAULTLINE_TASK_HASH,
                            arguments, sizeof(arguments), 1, 10) == FAULTLINE_JOB_OK);
    if (type == FAULTLINE_WAL_JOB_CREATED) { return EXIT_SUCCESS; }
    CHECK(faultline_job_assign(job, SAMPLE_WORKER, 20) == FAULTLINE_JOB_OK);
    if (type == FAULTLINE_WAL_JOB_ASSIGNED) { return EXIT_SUCCESS; }
    CHECK(faultline_job_start(job, SAMPLE_WORKER, 1, 30) == FAULTLINE_JOB_OK);
    if (type == FAULTLINE_WAL_JOB_STARTED) { return EXIT_SUCCESS; }
    if (type == FAULTLINE_WAL_JOB_COMPLETED) {
        CHECK(faultline_job_complete(job, SAMPLE_WORKER, 1, result, sizeof(result), 40) == FAULTLINE_JOB_OK);
        return EXIT_SUCCESS;
    }
    CHECK(faultline_job_fail(job, SAMPLE_WORKER, 1, FAULTLINE_JOB_FAILURE_TASK, 40) == FAULTLINE_JOB_OK);
    if (type == FAULTLINE_WAL_JOB_REQUEUED) { return EXIT_SUCCESS; }
    CHECK(faultline_job_assign(job, SAMPLE_WORKER, 50) == FAULTLINE_JOB_OK);
    CHECK(faultline_job_start(job, SAMPLE_WORKER, 2, 60) == FAULTLINE_JOB_OK);
    CHECK(faultline_job_fail(job, SAMPLE_WORKER, 2, FAULTLINE_JOB_FAILURE_WORKER_LOST, 70) == FAULTLINE_JOB_OK);
    return EXIT_SUCCESS;
}

static int jobs_equal(const struct faultline_job *a, const struct faultline_job *b)
{
    return a->id == b->id && a->task_type == b->task_type && a->state == b->state &&
           a->worker_id == b->worker_id && a->attempt == b->attempt &&
           a->retry_count == b->retry_count && a->max_retries == b->max_retries &&
           a->created_at_ms == b->created_at_ms && a->updated_at_ms == b->updated_at_ms &&
           a->assigned_at_ms == b->assigned_at_ms && a->started_at_ms == b->started_at_ms &&
           a->finished_at_ms == b->finished_at_ms && a->failure == b->failure &&
           a->argument_size == b->argument_size && a->result_size == b->result_size &&
           memcmp(a->arguments, b->arguments, a->argument_size) == 0 &&
           memcmp(a->result, b->result, a->result_size) == 0;
}

static int roundtrip(const struct faultline_wal_record *record)
{
    uint8_t wire[FAULTLINE_WAL_MAX_RECORD_SIZE];
    size_t written = 0, consumed = 0;
    struct faultline_wal_record decoded;
    CHECK(faultline_wal_record_encode(wire, sizeof(wire), record, &written) == FAULTLINE_WAL_OK);
    CHECK(faultline_wal_record_decode(wire, written, record->sequence, &decoded, &consumed) == FAULTLINE_WAL_OK);
    CHECK(consumed == written && decoded.type == record->type && decoded.sequence == record->sequence);
    if (record->type == FAULTLINE_WAL_WORKER_ID_ALLOCATED) {
        CHECK(decoded.payload.worker_id == record->payload.worker_id);
    } else {
        CHECK(jobs_equal(&decoded.payload.job, &record->payload.job));
    }
    return EXIT_SUCCESS;
}

static int decode_error(const uint8_t *wire, size_t size, uint64_t sequence,
                        enum faultline_wal_result expected)
{
    struct faultline_wal_record decoded;
    uint8_t before[sizeof(decoded)];
    size_t consumed = 99;
    /* Exact allocations let ASan catch even a one-byte incomplete-input overread. */
    uint8_t *input = malloc(size == 0 ? 1 : size);
    CHECK(input != NULL);
    memcpy(input, wire, size);
    memset(&decoded, 0xa5, sizeof(decoded));
    memcpy(before, &decoded, sizeof(before));
    enum faultline_wal_result actual = faultline_wal_record_decode(input, size, sequence, &decoded, &consumed);
    free(input);
    CHECK(actual == expected);
    CHECK(consumed == 99 && memcmp(before, &decoded, sizeof(before)) == 0);
    return EXIT_SUCCESS;
}

static int encode_error(const struct faultline_wal_record *record, size_t capacity,
                        enum faultline_wal_result expected)
{
    uint8_t wire[FAULTLINE_WAL_MAX_RECORD_SIZE], before[sizeof(wire)];
    size_t written = 99;
    memset(wire, 0xa5, sizeof(wire));
    memcpy(before, wire, sizeof(wire));
    CHECK(faultline_wal_record_encode(wire, capacity, record, &written) == expected);
    CHECK(written == 99 && memcmp(wire, before, sizeof(wire)) == 0);
    return EXIT_SUCCESS;
}

static int header_error(const uint8_t *wire, uint64_t sequence, enum faultline_wal_result expected)
{
    struct faultline_wal_record_header header;
    uint8_t before[sizeof(header)];
    memset(&header, 0xa5, sizeof(header));
    memcpy(before, &header, sizeof(header));
    CHECK(faultline_wal_record_header_decode(wire, 32, sequence, &header) == expected);
    CHECK(memcmp(before, &header, sizeof(header)) == 0);
    CHECK(decode_error(wire, 32, sequence, expected) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static int test_literal_bytes(void)
{
    _Alignas(uint64_t) uint8_t wire[FAULTLINE_WAL_MAX_RECORD_SIZE + 2];
    memset(wire, 0xa5, sizeof(wire));
    CHECK(faultline_wal_file_header_encode(wire + 1, sizeof(wire) - 1) == FAULTLINE_WAL_OK);
    CHECK(memcmp(wire + 1, file_header_bytes, 24) == 0 && wire[0] == 0xa5 && wire[25] == 0xa5);
    CHECK(faultline_wal_file_header_decode(wire + 1, sizeof(wire) - 1) == FAULTLINE_WAL_OK);
    CHECK(reference_crc((const uint8_t *)"123456789", 9) == UINT32_C(0xcbf43926));
    CHECK(reference_crc(NULL, 0) == 0);
    for (size_t i = 0; i < 7; ++i) {
        struct faultline_wal_record record, decoded;
        struct faultline_wal_record_header header;
        size_t written = 99, consumed = 99;
        CHECK(make_record(&record, (enum faultline_wal_record_type)(i + 1)) == EXIT_SUCCESS);
        memset(wire, 0xa5, sizeof(wire));
        CHECK(faultline_wal_record_encode(wire + 1, sizeof(wire) - 1, &record, &written) == FAULTLINE_WAL_OK);
        CHECK(written == record_vectors[i].size);
        CHECK(memcmp(wire + 1, record_vectors[i].bytes, written) == 0);
        CHECK(wire[0] == 0xa5);
        for (size_t j = written + 1; j < sizeof(wire); ++j) { CHECK(wire[j] == 0xa5); }
        CHECK(faultline_wal_record_header_decode(wire + 1, 32, SAMPLE_SEQUENCE, &header) == FAULTLINE_WAL_OK);
        CHECK(header.type == record.type && header.sequence == SAMPLE_SEQUENCE);
        CHECK(header.payload_length == written - 32);
        CHECK(faultline_wal_record_decode(wire + 1, sizeof(wire) - 1, SAMPLE_SEQUENCE,
                                          &decoded, &consumed) == FAULTLINE_WAL_OK);
        CHECK(consumed == written && decoded.type == record.type && decoded.sequence == record.sequence);
        if (i == 0) { CHECK(decoded.payload.worker_id == SAMPLE_WORKER); }
        else { CHECK(jobs_equal(&decoded.payload.job, &record.payload.job)); }
    }
    return EXIT_SUCCESS;
}

static int test_incomplete_and_capacity(void)
{
    uint8_t wire[FAULTLINE_WAL_MAX_RECORD_SIZE], before[sizeof(wire)];
    for (size_t n = 0; n < 24; ++n) {
        uint8_t *input = malloc(n == 0 ? 1 : n);
        CHECK(input != NULL);
        memcpy(input, file_header_bytes, n);
        CHECK(faultline_wal_file_header_decode(input, n) == FAULTLINE_WAL_INCOMPLETE);
        free(input);
        memset(wire, 0xa5, sizeof(wire));
        memcpy(before, wire, sizeof(wire));
        CHECK(faultline_wal_file_header_encode(wire, n) == FAULTLINE_WAL_BUFFER_TOO_SMALL);
        CHECK(memcmp(wire, before, sizeof(wire)) == 0);
    }
    for (size_t i = 0; i < 7; ++i) {
        struct faultline_wal_record record;
        CHECK(make_record(&record, (enum faultline_wal_record_type)(i + 1)) == EXIT_SUCCESS);
        for (size_t n = 0; n < record_vectors[i].size; ++n) {
            CHECK(decode_error(record_vectors[i].bytes, n, SAMPLE_SEQUENCE, FAULTLINE_WAL_INCOMPLETE) == EXIT_SUCCESS);
            CHECK(encode_error(&record, n, FAULTLINE_WAL_BUFFER_TOO_SMALL) == EXIT_SUCCESS);
            if (n < FAULTLINE_WAL_RECORD_HEADER_SIZE) {
                struct faultline_wal_record_header header;
                uint8_t saved[sizeof(header)];
                uint8_t *input = malloc(n == 0 ? 1 : n);
                CHECK(input != NULL);
                memcpy(input, record_vectors[i].bytes, n);
                memset(&header, 0xa5, sizeof(header));
                memcpy(saved, &header, sizeof(saved));
                CHECK(faultline_wal_record_header_decode(input, n, SAMPLE_SEQUENCE,
                                                         &header) == FAULTLINE_WAL_INCOMPLETE);
                free(input);
                CHECK(memcmp(saved, &header, sizeof(saved)) == 0);
            }
        }
    }
    struct faultline_wal_record maximum;
    size_t size = 0;
    CHECK(make_record(&maximum, FAULTLINE_WAL_JOB_COMPLETED) == EXIT_SUCCESS);
    maximum.payload.job.argument_size = FAULTLINE_JOB_MAX_ARGUMENT_SIZE;
    maximum.payload.job.result_size = FAULTLINE_JOB_MAX_RESULT_SIZE;
    CHECK(faultline_wal_record_encode(wire, sizeof(wire), &maximum, &size) == FAULTLINE_WAL_OK);
    CHECK(size == sizeof(wire) && size == 2164);
    for (size_t n = 0; n < size; ++n) {
        CHECK(decode_error(wire, n, SAMPLE_SEQUENCE, FAULTLINE_WAL_INCOMPLETE) == EXIT_SUCCESS);
        CHECK(encode_error(&maximum, n, FAULTLINE_WAL_BUFFER_TOO_SMALL) == EXIT_SUCCESS);
    }
    return EXIT_SUCCESS;
}

static int test_corruption(void)
{
    uint8_t wire[122];
    for (size_t byte = 0; byte < 24; ++byte) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            memcpy(wire, file_header_bytes, 24);
            wire[byte] ^= (uint8_t)(1u << bit);
            CHECK(faultline_wal_file_header_decode(wire, 24) == FAULTLINE_WAL_BAD_CHECKSUM);
        }
    }
    for (size_t i = 0; i < 7; ++i) {
        for (size_t byte = 0; byte < record_vectors[i].size; ++byte) {
            for (unsigned bit = 0; bit < 8; ++bit) {
                memcpy(wire, record_vectors[i].bytes, record_vectors[i].size);
                wire[byte] ^= (uint8_t)(1u << bit);
                CHECK(decode_error(wire, record_vectors[i].size, SAMPLE_SEQUENCE,
                                   FAULTLINE_WAL_BAD_CHECKSUM) == EXIT_SUCCESS);
            }
        }
    }
    /* Trust neither an inflated length nor a corrupted payload CRC while waiting for data. */
    memcpy(wire, record_vectors[1].bytes, 32);
    wire[11] ^= 1;
    CHECK(header_error(wire, SAMPLE_SEQUENCE, FAULTLINE_WAL_BAD_CHECKSUM) == EXIT_SUCCESS);
    memcpy(wire, record_vectors[1].bytes, 32);
    wire[24] ^= 1;
    CHECK(header_error(wire, SAMPLE_SEQUENCE, FAULTLINE_WAL_BAD_CHECKSUM) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static int test_invalid_headers(void)
{
    uint8_t wire[122];
    const struct {size_t offset; uint8_t value; enum faultline_wal_result error;} files[] = {
        {0, 0, FAULTLINE_WAL_BAD_MAGIC}, {9, 2, FAULTLINE_WAL_UNSUPPORTED_VERSION},
        {11, 23, FAULTLINE_WAL_INVALID_HEADER}, {15, 1, FAULTLINE_WAL_INVALID_HEADER},
        {19, 1, FAULTLINE_WAL_INVALID_HEADER}
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        memcpy(wire, file_header_bytes, 24);
        wire[files[i].offset] = files[i].value;
        put_u32(wire + 20, reference_crc(wire, 20));
        CHECK(faultline_wal_file_header_decode(wire, 24) == files[i].error);
    }
    const struct {size_t offset; uint8_t value; enum faultline_wal_result error;} headers[] = {
        {0, 0, FAULTLINE_WAL_BAD_MAGIC}, {5, 2, FAULTLINE_WAL_UNSUPPORTED_VERSION},
        {7, 0, FAULTLINE_WAL_UNKNOWN_RECORD_TYPE}, {7, 8, FAULTLINE_WAL_UNKNOWN_RECORD_TYPE},
        {15, 1, FAULTLINE_WAL_INVALID_HEADER}, {23, 0, FAULTLINE_WAL_INVALID_SEQUENCE},
        {11, 3, FAULTLINE_WAL_INVALID_PAYLOAD_LENGTH}, {11, 5, FAULTLINE_WAL_INVALID_PAYLOAD_LENGTH}
    };
    for (size_t i = 0; i < sizeof(headers) / sizeof(headers[0]); ++i) {
        memcpy(wire, record_vectors[0].bytes, 32);
        wire[headers[i].offset] = headers[i].value;
        put_u32(wire + 28, reference_crc(wire, 28));
        CHECK(header_error(wire, SAMPLE_SEQUENCE, headers[i].error) == EXIT_SUCCESS);
    }
    for (size_t i = 1; i < 7; ++i) {
        uint32_t lengths[] = {83, UINT32_MAX, i == 4 ? 2133u : 1109u};
        for (size_t j = 0; j < 3; ++j) {
            memcpy(wire, record_vectors[i].bytes, 32);
            put_u32(wire + 8, lengths[j]);
            put_u32(wire + 28, reference_crc(wire, 28));
            CHECK(header_error(wire, SAMPLE_SEQUENCE, FAULTLINE_WAL_INVALID_PAYLOAD_LENGTH) == EXIT_SUCCESS);
        }
    }
    return EXIT_SUCCESS;
}

static int test_invalid_payloads(void)
{
    uint8_t wire[122];
    /* Known-good CRCs after mutations ensure semantic checks, not CRCs, reject these. */
    const struct {size_t vector, offset, width; uint64_t value; enum faultline_wal_result error;} cases[] = {
        {0, 0, 4, 0, FAULTLINE_WAL_INVALID_PAYLOAD},
        {1, 0, 8, 0, FAULTLINE_WAL_INVALID_PAYLOAD},
        {1, 8, 2, 0, FAULTLINE_WAL_INVALID_PAYLOAD},
        {1, 8, 2, 5, FAULTLINE_WAL_INVALID_PAYLOAD},
        {1, 10, 2, 2, FAULTLINE_WAL_INVALID_PAYLOAD},
        {1, 12, 4, 1, FAULTLINE_WAL_INVALID_PAYLOAD},
        {1, 16, 8, 1, FAULTLINE_WAL_INVALID_PAYLOAD},
        {1, 24, 4, 1, FAULTLINE_WAL_INVALID_PAYLOAD},
        {1, 32, 8, UINT64_MAX, FAULTLINE_WAL_INVALID_PAYLOAD},
        {1, 40, 8, 9, FAULTLINE_WAL_INVALID_PAYLOAD},
        {1, 40, 8, 11, FAULTLINE_WAL_INVALID_PAYLOAD},
        {1, 48, 8, 0, FAULTLINE_WAL_INVALID_PAYLOAD},
        {1, 72, 2, 1, FAULTLINE_WAL_INVALID_PAYLOAD},
        {1, 74, 2, 1, FAULTLINE_WAL_INVALID_PAYLOAD},
        {1, 76, 4, 1025, FAULTLINE_WAL_INVALID_PAYLOAD_LENGTH},
        {1, 76, 4, UINT32_MAX, FAULTLINE_WAL_INVALID_PAYLOAD_LENGTH},
        {1, 80, 4, UINT32_MAX, FAULTLINE_WAL_INVALID_PAYLOAD_LENGTH},
        {1, 76, 4, 2, FAULTLINE_WAL_INVALID_PAYLOAD_LENGTH},
        {2, 12, 4, 0, FAULTLINE_WAL_INVALID_PAYLOAD},
        {2, 16, 8, 0, FAULTLINE_WAL_INVALID_PAYLOAD},
        {2, 48, 8, UINT64_C(0x8000000000000000), FAULTLINE_WAL_INVALID_PAYLOAD},
        {2, 56, 8, 20, FAULTLINE_WAL_INVALID_PAYLOAD},
        {3, 56, 8, 19, FAULTLINE_WAL_INVALID_PAYLOAD},
        {3, 64, 8, 30, FAULTLINE_WAL_INVALID_PAYLOAD},
        {4, 56, 8, UINT64_MAX, FAULTLINE_WAL_INVALID_PAYLOAD},
        {4, 64, 8, 29, FAULTLINE_WAL_INVALID_PAYLOAD},
        {4, 72, 2, 1, FAULTLINE_WAL_INVALID_PAYLOAD},
        {5, 24, 4, 0, FAULTLINE_WAL_INVALID_PAYLOAD},
        {5, 24, 4, 2, FAULTLINE_WAL_INVALID_PAYLOAD},
        {5, 72, 2, 0, FAULTLINE_WAL_INVALID_PAYLOAD},
        {6, 28, 4, 2, FAULTLINE_WAL_INVALID_PAYLOAD},
        {6, 72, 2, 3, FAULTLINE_WAL_INVALID_PAYLOAD}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        size_t n = cases[i].vector, size = record_vectors[n].size;
        memcpy(wire, record_vectors[n].bytes, size);
        uint8_t *field = wire + 32 + cases[i].offset;
        if (cases[i].width == 2) { put_u16(field, (uint16_t)cases[i].value); }
        else if (cases[i].width == 4) { put_u32(field, (uint32_t)cases[i].value); }
        else { put_u64(field, cases[i].value); }
        repair_record(wire, size);
        CHECK(decode_error(wire, size, SAMPLE_SEQUENCE, cases[i].error) == EXIT_SUCCESS);
    }
    /* A result in a non-DONE snapshot, with lengths and CRCs otherwise consistent. */
    memcpy(wire, record_vectors[1].bytes, 119);
    put_u32(wire + 32 + 76, 2);
    put_u32(wire + 32 + 80, 1);
    repair_record(wire, 119);
    CHECK(decode_error(wire, 119, SAMPLE_SEQUENCE, FAULTLINE_WAL_INVALID_PAYLOAD) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static int test_invalid_encode(void)
{
    struct faultline_wal_record record, bad;
    CHECK(make_record(&record, FAULTLINE_WAL_JOB_COMPLETED) == EXIT_SUCCESS);
#define REJECT_FIELD(field, value) do { bad = record; bad.payload.job.field = (value); \
    CHECK(encode_error(&bad, FAULTLINE_WAL_MAX_RECORD_SIZE, FAULTLINE_WAL_INVALID_PAYLOAD) == EXIT_SUCCESS); } while (0)
    REJECT_FIELD(id, 0);
    REJECT_FIELD(task_type, (enum faultline_task_type)-1);
    REJECT_FIELD(state, FAULTLINE_JOB_RUNNING);
    REJECT_FIELD(worker_id, 0);
    REJECT_FIELD(attempt, UINT64_MAX);
    REJECT_FIELD(retry_count, 2);
    REJECT_FIELD(argument_size, SIZE_MAX);
    REJECT_FIELD(result_size, SIZE_MAX);
    REJECT_FIELD(created_at_ms, -1);
    REJECT_FIELD(updated_at_ms, 9);
    REJECT_FIELD(assigned_at_ms, -2);
    REJECT_FIELD(started_at_ms, -1);
    REJECT_FIELD(finished_at_ms, -1);
    REJECT_FIELD(failure, FAULTLINE_JOB_FAILURE_TASK);
#undef REJECT_FIELD
    bad = record; bad.sequence = 0;
    CHECK(encode_error(&bad, FAULTLINE_WAL_MAX_RECORD_SIZE, FAULTLINE_WAL_INVALID_SEQUENCE) == EXIT_SUCCESS);
    bad = record; bad.type = (enum faultline_wal_record_type)-1;
    CHECK(encode_error(&bad, FAULTLINE_WAL_MAX_RECORD_SIZE, FAULTLINE_WAL_UNKNOWN_RECORD_TYPE) == EXIT_SUCCESS);
    CHECK(make_record(&bad, FAULTLINE_WAL_WORKER_ID_ALLOCATED) == EXIT_SUCCESS);
    bad.payload.worker_id = 0;
    CHECK(encode_error(&bad, FAULTLINE_WAL_MAX_RECORD_SIZE, FAULTLINE_WAL_INVALID_PAYLOAD) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static int test_boundaries(void)
{
    struct faultline_wal_record record;
    CHECK(make_record(&record, FAULTLINE_WAL_WORKER_ID_ALLOCATED) == EXIT_SUCCESS);
    record.sequence = UINT64_MAX;
    record.payload.worker_id = UINT32_MAX;
    CHECK(roundtrip(&record) == EXIT_SUCCESS);
    for (size_t i = 1; i < 7; ++i) {
        CHECK(make_record(&record, (enum faultline_wal_record_type)(i + 1)) == EXIT_SUCCESS);
        struct faultline_job *job = &record.payload.job;
        record.sequence = UINT64_MAX;
        job->id = UINT64_MAX;
        job->max_retries = UINT32_MAX;
        job->created_at_ms = job->updated_at_ms = INT64_MAX;
        job->argument_size = FAULTLINE_JOB_MAX_ARGUMENT_SIZE;
        for (size_t j = 0; j < job->argument_size; ++j) { job->arguments[j] = (uint8_t)j; }
        if (record.type == FAULTLINE_WAL_JOB_REQUEUED) {
            job->retry_count = UINT32_MAX;
            job->attempt = UINT32_MAX;
        } else if (record.type != FAULTLINE_WAL_JOB_CREATED) {
            job->retry_count = UINT32_MAX;
            job->attempt = (uint64_t)UINT32_MAX + UINT64_C(1);
            job->worker_id = UINT32_MAX;
            job->assigned_at_ms = INT64_MAX;
            if (job->started_at_ms != -1) { job->started_at_ms = INT64_MAX; }
            if (job->finished_at_ms != -1) { job->finished_at_ms = INT64_MAX; }
        }
        if (record.type == FAULTLINE_WAL_JOB_COMPLETED) {
            job->result_size = FAULTLINE_JOB_MAX_RESULT_SIZE;
            for (size_t j = 0; j < job->result_size; ++j) { job->result[j] = (uint8_t)(255 - j % 256); }
        }
        CHECK(roundtrip(&record) == EXIT_SUCCESS);
        job->argument_size = job->result_size = 0;
        job->created_at_ms = job->updated_at_ms = 0;
        if (job->assigned_at_ms != -1) { job->assigned_at_ms = 0; }
        if (job->started_at_ms != -1) { job->started_at_ms = 0; }
        if (job->finished_at_ms != -1) { job->finished_at_ms = 0; }
        CHECK(roundtrip(&record) == EXIT_SUCCESS);
    }
    return EXIT_SUCCESS;
}

static int test_sequence_and_consecutive(void)
{
    uint8_t wire[2 * FAULTLINE_WAL_MAX_RECORD_SIZE];
    struct faultline_wal_record record, decoded;
    size_t first = 0, second = 0, consumed = 0;
    CHECK(make_record(&record, FAULTLINE_WAL_JOB_CREATED) == EXIT_SUCCESS);
    record.sequence = 1;
    CHECK(faultline_wal_record_encode(wire, sizeof(wire), &record, &first) == FAULTLINE_WAL_OK);
    CHECK(make_record(&record, FAULTLINE_WAL_JOB_ASSIGNED) == EXIT_SUCCESS);
    record.sequence = 2;
    CHECK(faultline_wal_record_encode(wire + first, sizeof(wire) - first, &record, &second) == FAULTLINE_WAL_OK);
    CHECK(faultline_wal_record_decode(wire, first + second, 1, &decoded, &consumed) == FAULTLINE_WAL_OK);
    CHECK(consumed == first && decoded.sequence == 1);
    CHECK(faultline_wal_record_decode(wire + consumed, second, 2, &decoded, &consumed) == FAULTLINE_WAL_OK);
    CHECK(consumed == second && decoded.sequence == 2);
    CHECK(decode_error(wire, first, 2, FAULTLINE_WAL_INVALID_SEQUENCE) == EXIT_SUCCESS);
    CHECK(decode_error(wire + first, second, 1, FAULTLINE_WAL_INVALID_SEQUENCE) == EXIT_SUCCESS);
    CHECK(decode_error(wire + first, second, 3, FAULTLINE_WAL_INVALID_SEQUENCE) == EXIT_SUCCESS);
    CHECK(decode_error(wire, first, 0, FAULTLINE_WAL_INVALID_ARGUMENT) == EXIT_SUCCESS);
    put_u64(wire + 16, 0);
    put_u32(wire + 28, reference_crc(wire, 28));
    CHECK(header_error(wire, 1, FAULTLINE_WAL_INVALID_SEQUENCE) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static int test_model_lifecycles(void)
{
    for (int task = FAULTLINE_TASK_SLEEP; task <= FAULTLINE_TASK_HASH; ++task) {
        for (uint32_t budget = 0; budget <= 2; ++budget) {
            for (int started = 0; started <= 1; ++started) {
                for (int failure = FAULTLINE_JOB_FAILURE_TASK; failure <= FAULTLINE_JOB_FAILURE_WORKER_LOST; ++failure) {
                    struct faultline_wal_record record = {.type = FAULTLINE_WAL_JOB_CREATED, .sequence = 1};
                    struct faultline_job *job = &record.payload.job;
                    CHECK(faultline_job_init(job, 1, (enum faultline_task_type)task,
                                            NULL, 0, budget, 0) == FAULTLINE_JOB_OK);
                    CHECK(roundtrip(&record) == EXIT_SUCCESS);
                    for (uint64_t attempt = 1; attempt <= (uint64_t)budget + 1; ++attempt) {
                        CHECK(faultline_job_assign(job, 1, 0) == FAULTLINE_JOB_OK);
                        record.type = FAULTLINE_WAL_JOB_ASSIGNED;
                        CHECK(roundtrip(&record) == EXIT_SUCCESS);
                        if (started) {
                            CHECK(faultline_job_start(job, 1, attempt, 0) == FAULTLINE_JOB_OK);
                            record.type = FAULTLINE_WAL_JOB_STARTED;
                            CHECK(roundtrip(&record) == EXIT_SUCCESS);
                            struct faultline_wal_record done = record;
                            CHECK(faultline_job_complete(&done.payload.job, 1, attempt, NULL, 0, 0) == FAULTLINE_JOB_OK);
                            done.type = FAULTLINE_WAL_JOB_COMPLETED;
                            CHECK(roundtrip(&done) == EXIT_SUCCESS);
                        }
                        CHECK(faultline_job_fail(job, 1, attempt, (enum faultline_job_failure)failure, 0) == FAULTLINE_JOB_OK);
                        record.type = job->state == FAULTLINE_JOB_QUEUED ?
                            FAULTLINE_WAL_JOB_REQUEUED : FAULTLINE_WAL_JOB_FAILED;
                        CHECK(roundtrip(&record) == EXIT_SUCCESS);
                    }
                }
            }
        }
    }
    return EXIT_SUCCESS;
}

static int test_arguments_and_owned_bytes(void)
{
    uint8_t wire[FAULTLINE_WAL_MAX_RECORD_SIZE], before[sizeof(wire)];
    struct faultline_wal_record record, decoded;
    struct faultline_wal_record_header header;
    size_t written = 99, consumed = 99;
    CHECK(make_record(&record, FAULTLINE_WAL_JOB_COMPLETED) == EXIT_SUCCESS);
    CHECK(faultline_wal_file_header_encode(NULL, 24) == FAULTLINE_WAL_INVALID_ARGUMENT);
    CHECK(faultline_wal_file_header_decode(NULL, 24) == FAULTLINE_WAL_INVALID_ARGUMENT);
    CHECK(faultline_wal_record_encode(NULL, sizeof(wire), &record, &written) == FAULTLINE_WAL_INVALID_ARGUMENT);
    CHECK(written == 99);
    memset(wire, 0xa5, sizeof(wire));
    memcpy(before, wire, sizeof(wire));
    CHECK(faultline_wal_record_encode(wire, sizeof(wire), NULL, &written) == FAULTLINE_WAL_INVALID_ARGUMENT);
    CHECK(faultline_wal_record_encode(wire, sizeof(wire), &record, NULL) == FAULTLINE_WAL_INVALID_ARGUMENT);
    CHECK(written == 99 && memcmp(wire, before, sizeof(wire)) == 0);
    CHECK(faultline_wal_record_header_decode(wire, 32, 1, NULL) == FAULTLINE_WAL_INVALID_ARGUMENT);
    CHECK(faultline_wal_record_header_decode(NULL, 32, 1, &header) == FAULTLINE_WAL_INVALID_ARGUMENT);
    CHECK(faultline_wal_record_decode(NULL, 32, 1, &decoded, &consumed) == FAULTLINE_WAL_INVALID_ARGUMENT);
    CHECK(faultline_wal_record_decode(wire, 32, 1, NULL, &consumed) == FAULTLINE_WAL_INVALID_ARGUMENT);
    CHECK(faultline_wal_record_decode(wire, 32, 1, &decoded, NULL) == FAULTLINE_WAL_INVALID_ARGUMENT);
    CHECK(consumed == 99);
    CHECK(faultline_wal_record_encode(wire, sizeof(wire), &record, &written) == FAULTLINE_WAL_OK);
    memcpy(before, wire, written);
    memset(record.payload.job.arguments + 3, 0xfe, sizeof(record.payload.job.arguments) - 3);
    memset(record.payload.job.result + 3, 0xfe, sizeof(record.payload.job.result) - 3);
    CHECK(faultline_wal_record_encode(wire, sizeof(wire), &record, &written) == FAULTLINE_WAL_OK);
    CHECK(memcmp(wire, before, written) == 0); /* Unused storage is never serialized. */
    CHECK(faultline_wal_record_decode(wire, written, SAMPLE_SEQUENCE, &decoded, &consumed) == FAULTLINE_WAL_OK);
    memset(wire, 0, sizeof(wire));
    CHECK(jobs_equal(&decoded.payload.job, &record.payload.job));
    return EXIT_SUCCESS;
}

int main(void)
{
    const struct {const char *name; int (*run)(void);} tests[] = {
        {"literal WAL headers and all record types", test_literal_bytes},
        {"every incomplete prefix and short output capacity", test_incomplete_and_capacity},
        {"single-bit corruption and untrusted lengths", test_corruption},
        {"invalid headers with valid checksums", test_invalid_headers},
        {"invalid payloads with valid checksums", test_invalid_payloads},
        {"invalid encode preserves all outputs", test_invalid_encode},
        {"payload, ID, retry, attempt, and timestamp boundaries", test_boundaries},
        {"sequence continuity and consecutive records", test_sequence_and_consecutive},
        {"job model lifecycle compatibility", test_model_lifecycles},
        {"null arguments and owned canonical bytes", test_arguments_and_owned_bytes}
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (tests[i].run() != EXIT_SUCCESS) { return EXIT_FAILURE; }
        printf("PASS: %s\n", tests[i].name);
    }
    return EXIT_SUCCESS;
}
