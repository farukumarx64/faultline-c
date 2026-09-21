#include "wal.h"

#include <string.h>

static const uint8_t file_magic[8] = {'F', 'L', 'I', 'N', 'W', 'A', 'L', 0};

_Static_assert(FAULTLINE_JOB_MAX_ARGUMENT_SIZE == 1024 && FAULTLINE_JOB_MAX_RESULT_SIZE == 1024,
               "WAL v1 payload bounds are fixed");
_Static_assert(FAULTLINE_JOB_TIME_UNSET == -1, "WAL v1 unset time is -1");
_Static_assert(FAULTLINE_TASK_SLEEP == 1 && FAULTLINE_TASK_PRIME_COUNT == 2 &&
               FAULTLINE_TASK_FIBONACCI == 3 && FAULTLINE_TASK_HASH == 4,
               "WAL v1 task IDs are fixed");
_Static_assert(FAULTLINE_JOB_QUEUED == 1 && FAULTLINE_JOB_ASSIGNED == 2 &&
               FAULTLINE_JOB_RUNNING == 3 && FAULTLINE_JOB_DONE == 4 && FAULTLINE_JOB_FAILED == 5,
               "WAL v1 state IDs are fixed");
_Static_assert(FAULTLINE_JOB_FAILURE_NONE == 0 && FAULTLINE_JOB_FAILURE_TASK == 1 &&
               FAULTLINE_JOB_FAILURE_WORKER_LOST == 2, "WAL v1 failure IDs are fixed");

static void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static void put_u64(uint8_t *out, uint64_t value)
{
    put_u32(out, (uint32_t)(value >> 32));
    put_u32(out + 4, (uint32_t)value);
}

static uint16_t get_u16(const uint8_t *in)
{
    return (uint16_t)(((uint16_t)in[0] << 8) | (uint16_t)in[1]);
}

static uint32_t get_u32(const uint8_t *in)
{
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

static uint64_t get_u64(const uint8_t *in)
{
    return ((uint64_t)get_u32(in) << 32) | (uint64_t)get_u32(in + 4);
}

/* CRC-32/ISO-HDLC: reflected polynomial, initial/final XOR 0xffffffff. */
static uint32_t crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) != 0 ? UINT32_C(0xedb88320) : 0);
        }
    }
    return crc ^ UINT32_MAX;
}

static enum faultline_wal_result payload_bounds(enum faultline_wal_record_type type,
                                               uint32_t *minimum, uint32_t *maximum)
{
    switch (type) {
    case FAULTLINE_WAL_WORKER_ID_ALLOCATED:
        *minimum = *maximum = 4;
        return FAULTLINE_WAL_OK;
    case FAULTLINE_WAL_JOB_CREATED:
    case FAULTLINE_WAL_JOB_ASSIGNED:
    case FAULTLINE_WAL_JOB_STARTED:
    case FAULTLINE_WAL_JOB_REQUEUED:
    case FAULTLINE_WAL_JOB_FAILED:
        *minimum = FAULTLINE_WAL_JOB_PREFIX_SIZE;
        *maximum = FAULTLINE_WAL_JOB_PREFIX_SIZE + FAULTLINE_JOB_MAX_ARGUMENT_SIZE;
        return FAULTLINE_WAL_OK;
    case FAULTLINE_WAL_JOB_COMPLETED:
        *minimum = FAULTLINE_WAL_JOB_PREFIX_SIZE;
        *maximum = FAULTLINE_WAL_MAX_PAYLOAD_SIZE;
        return FAULTLINE_WAL_OK;
    default:
        return FAULTLINE_WAL_UNKNOWN_RECORD_TYPE;
    }
}

static int valid_failure(enum faultline_job_failure failure)
{
    return failure == FAULTLINE_JOB_FAILURE_TASK || failure == FAULTLINE_JOB_FAILURE_WORKER_LOST;
}

/* Validate one snapshot. Its relation to earlier records is a replay responsibility. */
static int valid_job(enum faultline_wal_record_type type, const struct faultline_job *job)
{
    if (job->id == 0 || job->task_type < FAULTLINE_TASK_SLEEP || job->task_type > FAULTLINE_TASK_HASH ||
        job->argument_size > FAULTLINE_JOB_MAX_ARGUMENT_SIZE ||
        job->result_size > FAULTLINE_JOB_MAX_RESULT_SIZE || job->retry_count > job->max_retries ||
        job->created_at_ms < 0 || job->updated_at_ms < job->created_at_ms) {
        return 0;
    }
    if (type == FAULTLINE_WAL_JOB_CREATED || type == FAULTLINE_WAL_JOB_REQUEUED) {
        if (job->state != FAULTLINE_JOB_QUEUED || job->worker_id != 0 || job->result_size != 0 ||
            job->assigned_at_ms != -1 || job->started_at_ms != -1 || job->finished_at_ms != -1) {
            return 0;
        }
        if (type == FAULTLINE_WAL_JOB_CREATED) {
            return job->attempt == 0 && job->retry_count == 0 &&
                   job->failure == FAULTLINE_JOB_FAILURE_NONE &&
                   job->updated_at_ms == job->created_at_ms;
        }
        return job->retry_count != 0 && job->attempt == job->retry_count && valid_failure(job->failure);
    }
    if (job->worker_id == 0 || job->attempt != (uint64_t)job->retry_count + UINT64_C(1) ||
        job->assigned_at_ms < job->created_at_ms || job->updated_at_ms < job->assigned_at_ms) {
        return 0;
    }
    switch (type) {
    case FAULTLINE_WAL_JOB_ASSIGNED:
        return job->state == FAULTLINE_JOB_ASSIGNED && job->failure == FAULTLINE_JOB_FAILURE_NONE &&
               job->result_size == 0 && job->updated_at_ms == job->assigned_at_ms &&
               job->started_at_ms == -1 && job->finished_at_ms == -1;
    case FAULTLINE_WAL_JOB_STARTED:
        return job->state == FAULTLINE_JOB_RUNNING && job->failure == FAULTLINE_JOB_FAILURE_NONE &&
               job->result_size == 0 && job->started_at_ms >= job->assigned_at_ms &&
               job->updated_at_ms == job->started_at_ms && job->finished_at_ms == -1;
    case FAULTLINE_WAL_JOB_COMPLETED:
        return job->state == FAULTLINE_JOB_DONE && job->failure == FAULTLINE_JOB_FAILURE_NONE &&
               job->started_at_ms >= job->assigned_at_ms && job->finished_at_ms >= job->started_at_ms &&
               job->updated_at_ms == job->finished_at_ms;
    case FAULTLINE_WAL_JOB_FAILED:
        return job->state == FAULTLINE_JOB_FAILED && valid_failure(job->failure) &&
               job->retry_count == job->max_retries && job->result_size == 0 &&
               (job->started_at_ms == -1 || job->started_at_ms >= job->assigned_at_ms) &&
               job->finished_at_ms >= job->assigned_at_ms && job->finished_at_ms >= job->started_at_ms &&
               job->updated_at_ms == job->finished_at_ms;
    default:
        return 0;
    }
}

enum faultline_wal_result faultline_wal_file_header_encode(uint8_t *wire, size_t capacity)
{
    if (wire == NULL) { return FAULTLINE_WAL_INVALID_ARGUMENT; }
    if (capacity < FAULTLINE_WAL_FILE_HEADER_SIZE) { return FAULTLINE_WAL_BUFFER_TOO_SMALL; }
    memcpy(wire, file_magic, sizeof(file_magic));
    put_u16(wire + 8, FAULTLINE_WAL_VERSION);
    put_u16(wire + 10, FAULTLINE_WAL_FILE_HEADER_SIZE);
    put_u32(wire + 12, 0);
    put_u32(wire + 16, 0);
    put_u32(wire + 20, crc32(wire, 20));
    return FAULTLINE_WAL_OK;
}

enum faultline_wal_result faultline_wal_file_header_decode(const uint8_t *wire, size_t available)
{
    if (wire == NULL) { return FAULTLINE_WAL_INVALID_ARGUMENT; }
    if (available < FAULTLINE_WAL_FILE_HEADER_SIZE) { return FAULTLINE_WAL_INCOMPLETE; }
    if (get_u32(wire + 20) != crc32(wire, 20)) { return FAULTLINE_WAL_BAD_CHECKSUM; }
    if (memcmp(wire, file_magic, sizeof(file_magic)) != 0) { return FAULTLINE_WAL_BAD_MAGIC; }
    if (get_u16(wire + 8) != FAULTLINE_WAL_VERSION) { return FAULTLINE_WAL_UNSUPPORTED_VERSION; }
    if (get_u16(wire + 10) != FAULTLINE_WAL_FILE_HEADER_SIZE ||
        get_u32(wire + 12) != 0 || get_u32(wire + 16) != 0) {
        return FAULTLINE_WAL_INVALID_HEADER;
    }
    return FAULTLINE_WAL_OK;
}

enum faultline_wal_result faultline_wal_record_header_decode(
    const uint8_t *wire, size_t available, uint64_t expected_sequence,
    struct faultline_wal_record_header *header)
{
    uint32_t minimum, maximum;
    if (wire == NULL || header == NULL || expected_sequence == 0) { return FAULTLINE_WAL_INVALID_ARGUMENT; }
    if (available < FAULTLINE_WAL_RECORD_HEADER_SIZE) { return FAULTLINE_WAL_INCOMPLETE; }
    /* A damaged length must never turn a complete corrupt record into INCOMPLETE. */
    if (get_u32(wire + 28) != crc32(wire, 28)) { return FAULTLINE_WAL_BAD_CHECKSUM; }
    if (get_u32(wire) != FAULTLINE_WAL_RECORD_MAGIC) { return FAULTLINE_WAL_BAD_MAGIC; }
    if (get_u16(wire + 4) != FAULTLINE_WAL_VERSION) { return FAULTLINE_WAL_UNSUPPORTED_VERSION; }
    if (get_u32(wire + 12) != 0) { return FAULTLINE_WAL_INVALID_HEADER; }
    struct faultline_wal_record_header decoded = {
        .type = (enum faultline_wal_record_type)get_u16(wire + 6),
        .payload_length = get_u32(wire + 8),
        .sequence = get_u64(wire + 16),
        .payload_crc32 = get_u32(wire + 24)
    };
    enum faultline_wal_result result = payload_bounds(decoded.type, &minimum, &maximum);
    if (result != FAULTLINE_WAL_OK) { return result; }
    if (decoded.payload_length < minimum || decoded.payload_length > maximum) {
        return FAULTLINE_WAL_INVALID_PAYLOAD_LENGTH;
    }
    if (decoded.sequence != expected_sequence) { return FAULTLINE_WAL_INVALID_SEQUENCE; }
    *header = decoded;
    return FAULTLINE_WAL_OK;
}

static void encode_job(uint8_t *out, const struct faultline_job *job)
{
    put_u64(out, job->id);
    put_u16(out + 8, (uint16_t)job->task_type);
    put_u16(out + 10, (uint16_t)job->state);
    put_u32(out + 12, job->worker_id);
    put_u64(out + 16, job->attempt);
    put_u32(out + 24, job->retry_count);
    put_u32(out + 28, job->max_retries);
    put_u64(out + 32, (uint64_t)job->created_at_ms);
    put_u64(out + 40, (uint64_t)job->updated_at_ms);
    put_u64(out + 48, (uint64_t)job->assigned_at_ms);
    put_u64(out + 56, (uint64_t)job->started_at_ms);
    put_u64(out + 64, (uint64_t)job->finished_at_ms);
    put_u16(out + 72, (uint16_t)job->failure);
    put_u16(out + 74, 0);
    put_u32(out + 76, (uint32_t)job->argument_size);
    put_u32(out + 80, (uint32_t)job->result_size);
    memcpy(out + FAULTLINE_WAL_JOB_PREFIX_SIZE, job->arguments, job->argument_size);
    memcpy(out + FAULTLINE_WAL_JOB_PREFIX_SIZE + job->argument_size, job->result, job->result_size);
}

enum faultline_wal_result faultline_wal_record_encode(
    uint8_t *wire, size_t capacity, const struct faultline_wal_record *record, size_t *written)
{
    uint32_t minimum, maximum;
    if (wire == NULL || record == NULL || written == NULL) { return FAULTLINE_WAL_INVALID_ARGUMENT; }
    enum faultline_wal_result result = payload_bounds(record->type, &minimum, &maximum);
    if (result != FAULTLINE_WAL_OK) { return result; }
    if (record->sequence == 0) { return FAULTLINE_WAL_INVALID_SEQUENCE; }
    size_t payload_size = minimum;
    if (record->type == FAULTLINE_WAL_WORKER_ID_ALLOCATED) {
        if (record->payload.worker_id == 0) { return FAULTLINE_WAL_INVALID_PAYLOAD; }
    } else {
        if (!valid_job(record->type, &record->payload.job)) { return FAULTLINE_WAL_INVALID_PAYLOAD; }
        payload_size += record->payload.job.argument_size + record->payload.job.result_size;
    }
    size_t total = FAULTLINE_WAL_RECORD_HEADER_SIZE + payload_size;
    if (capacity < total) { return FAULTLINE_WAL_BUFFER_TOO_SMALL; }
    uint8_t *payload = wire + FAULTLINE_WAL_RECORD_HEADER_SIZE;
    if (record->type == FAULTLINE_WAL_WORKER_ID_ALLOCATED) {
        put_u32(payload, record->payload.worker_id);
    } else {
        encode_job(payload, &record->payload.job);
    }
    put_u32(wire, FAULTLINE_WAL_RECORD_MAGIC);
    put_u16(wire + 4, FAULTLINE_WAL_VERSION);
    put_u16(wire + 6, (uint16_t)record->type);
    put_u32(wire + 8, (uint32_t)payload_size);
    put_u32(wire + 12, 0);
    put_u64(wire + 16, record->sequence);
    put_u32(wire + 24, crc32(payload, payload_size));
    put_u32(wire + 28, crc32(wire, 28));
    *written = total;
    return FAULTLINE_WAL_OK;
}

static int decode_time(const uint8_t *in, int64_t *time_ms)
{
    uint64_t value = get_u64(in);
    if (value == UINT64_MAX) {
        *time_ms = FAULTLINE_JOB_TIME_UNSET;
    } else if (value <= INT64_MAX) {
        *time_ms = (int64_t)value;
    } else {
        return 0;
    }
    return 1;
}

static enum faultline_wal_result decode_job(const uint8_t *in, size_t size,
                                           enum faultline_wal_record_type type,
                                           struct faultline_job *job)
{
    uint32_t arguments = get_u32(in + 76), results = get_u32(in + 80);
    if (arguments > FAULTLINE_JOB_MAX_ARGUMENT_SIZE || results > FAULTLINE_JOB_MAX_RESULT_SIZE ||
        size != FAULTLINE_WAL_JOB_PREFIX_SIZE + arguments + results) {
        return FAULTLINE_WAL_INVALID_PAYLOAD_LENGTH;
    }
    if (get_u16(in + 74) != 0) { return FAULTLINE_WAL_INVALID_PAYLOAD; }
    *job = (struct faultline_job){
        .id = get_u64(in), .task_type = (enum faultline_task_type)get_u16(in + 8),
        .state = (enum faultline_job_state)get_u16(in + 10), .worker_id = get_u32(in + 12),
        .attempt = get_u64(in + 16), .retry_count = get_u32(in + 24), .max_retries = get_u32(in + 28),
        .failure = (enum faultline_job_failure)get_u16(in + 72),
        .argument_size = arguments, .result_size = results
    };
    if (!decode_time(in + 32, &job->created_at_ms) || !decode_time(in + 40, &job->updated_at_ms) ||
        !decode_time(in + 48, &job->assigned_at_ms) || !decode_time(in + 56, &job->started_at_ms) ||
        !decode_time(in + 64, &job->finished_at_ms) || !valid_job(type, job)) {
        return FAULTLINE_WAL_INVALID_PAYLOAD;
    }
    memcpy(job->arguments, in + FAULTLINE_WAL_JOB_PREFIX_SIZE, arguments);
    memcpy(job->result, in + FAULTLINE_WAL_JOB_PREFIX_SIZE + arguments, results);
    return FAULTLINE_WAL_OK;
}

enum faultline_wal_result faultline_wal_record_decode(
    const uint8_t *wire, size_t available, uint64_t expected_sequence,
    struct faultline_wal_record *record, size_t *consumed)
{
    struct faultline_wal_record_header header;
    if (wire == NULL || record == NULL || consumed == NULL) { return FAULTLINE_WAL_INVALID_ARGUMENT; }
    enum faultline_wal_result result =
        faultline_wal_record_header_decode(wire, available, expected_sequence, &header);
    if (result != FAULTLINE_WAL_OK) { return result; }
    size_t total = FAULTLINE_WAL_RECORD_HEADER_SIZE + (size_t)header.payload_length;
    if (available < total) { return FAULTLINE_WAL_INCOMPLETE; }
    const uint8_t *payload = wire + FAULTLINE_WAL_RECORD_HEADER_SIZE;
    if (crc32(payload, header.payload_length) != header.payload_crc32) { return FAULTLINE_WAL_BAD_CHECKSUM; }
    struct faultline_wal_record decoded = {.type = header.type, .sequence = header.sequence};
    if (header.type == FAULTLINE_WAL_WORKER_ID_ALLOCATED) {
        decoded.payload.worker_id = get_u32(payload);
        if (decoded.payload.worker_id == 0) { return FAULTLINE_WAL_INVALID_PAYLOAD; }
    } else {
        result = decode_job(payload, header.payload_length, header.type, &decoded.payload.job);
        if (result != FAULTLINE_WAL_OK) { return result; }
    }
    *record = decoded;
    *consumed = total;
    return FAULTLINE_WAL_OK;
}
