#include "protocol.h"

#include <string.h>
#include <limits.h>

_Static_assert(FAULTLINE_HEADER_SIZE + FAULTLINE_STATS_PAYLOAD_SIZE <= FAULTLINE_MESSAGE_MAX_FRAME_SIZE,
               "maximum frame size must fit statistics");

_Static_assert(FAULTLINE_HEADER_SIZE + FAULTLINE_JOB_STATUS_RESPONSE_PREFIX_SIZE +
               FAULTLINE_JOB_MAX_RESULT_SIZE <= FAULTLINE_MESSAGE_MAX_FRAME_SIZE,
               "maximum frame size must fit status results");
_Static_assert(FAULTLINE_HEADER_SIZE + FAULTLINE_WORKERS_PREFIX_SIZE +
               FAULTLINE_WORKERS_MAX_ENTRIES * FAULTLINE_WORKER_SUMMARY_SIZE <= FAULTLINE_MESSAGE_MAX_FRAME_SIZE,
               "maximum frame size must fit worker lists");
_Static_assert(FAULTLINE_HEADER_SIZE + FAULTLINE_JOB_COMPLETED_PREFIX_SIZE +
               FAULTLINE_JOB_MAX_RESULT_SIZE <= FAULTLINE_MESSAGE_MAX_FRAME_SIZE,
               "maximum frame size must also fit completed results");
_Static_assert(FAULTLINE_HEADER_SIZE + FAULTLINE_JOB_ASSIGN_PREFIX_SIZE +
               FAULTLINE_JOB_MAX_ARGUMENT_SIZE <= FAULTLINE_MESSAGE_MAX_FRAME_SIZE,
               "maximum frame size must also fit assignments");
_Static_assert(FAULTLINE_JOB_QUEUED == 1 && FAULTLINE_JOB_ASSIGNED == 2 &&
               FAULTLINE_JOB_RUNNING == 3 && FAULTLINE_JOB_DONE == 4 && FAULTLINE_JOB_FAILED == 5,
               "job state IDs are part of the status wire format");
_Static_assert(FAULTLINE_JOB_FAILURE_NONE == 0 && FAULTLINE_JOB_FAILURE_TASK == 1 &&
               FAULTLINE_JOB_FAILURE_WORKER_LOST == 2,
               "failure IDs are part of the status wire format");

static void write_u16_be(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

static void write_u32_be(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static uint16_t read_u16_be(const uint8_t *in)
{
    return (uint16_t)(((uint16_t)in[0] << 8) | (uint16_t)in[1]);
}

static uint32_t read_u32_be(const uint8_t *in)
{
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

static void write_u64_be(uint8_t *out, uint64_t value)
{
    write_u32_be(out, (uint32_t)(value >> 32));
    write_u32_be(out + 4, (uint32_t)value);
}

static uint64_t read_u64_be(const uint8_t *in)
{
    return ((uint64_t)read_u32_be(in) << 32) | (uint64_t)read_u32_be(in + 4);
}

static enum faultline_protocol_result message_payload_bounds(
    uint16_t message_type, uint32_t *minimum, uint32_t *maximum)
{
    uint32_t extra = 0;

    switch (message_type) {
    case FAULTLINE_MSG_PING:
    case FAULTLINE_MSG_PONG:
    case FAULTLINE_MSG_WORKER_REGISTER:
    case FAULTLINE_MSG_JOBS_REQUEST:
    case FAULTLINE_MSG_WORKERS_REQUEST:
    case FAULTLINE_MSG_STATS_REQUEST:
        *minimum = 0;
        break;
    case FAULTLINE_MSG_WORKER_REGISTER_ACK:
    case FAULTLINE_MSG_HEARTBEAT:
        *minimum = 4;
        break;
    case FAULTLINE_MSG_JOB_SUBMIT:
        *minimum = FAULTLINE_JOB_SUBMIT_PREFIX_SIZE;
        extra = FAULTLINE_JOB_MAX_ARGUMENT_SIZE;
        break;
    case FAULTLINE_MSG_JOB_SUBMIT_ACK:
        *minimum = FAULTLINE_JOB_SUBMIT_ACK_PAYLOAD_SIZE;
        break;
    case FAULTLINE_MSG_JOB_ASSIGN:
        *minimum = FAULTLINE_JOB_ASSIGN_PREFIX_SIZE;
        extra = FAULTLINE_JOB_MAX_ARGUMENT_SIZE;
        break;
    case FAULTLINE_MSG_JOB_STARTED:
        *minimum = FAULTLINE_JOB_STARTED_PAYLOAD_SIZE;
        break;
    case FAULTLINE_MSG_JOB_COMPLETED:
        *minimum = FAULTLINE_JOB_COMPLETED_PREFIX_SIZE;
        extra = FAULTLINE_JOB_MAX_RESULT_SIZE;
        break;
    case FAULTLINE_MSG_JOB_FAILED:
        *minimum = FAULTLINE_JOB_FAILED_PAYLOAD_SIZE;
        break;
    case FAULTLINE_MSG_JOB_STATUS_REQUEST:
        *minimum = FAULTLINE_JOB_STATUS_REQUEST_PAYLOAD_SIZE;
        break;
    case FAULTLINE_MSG_JOB_STATUS_RESPONSE:
        *minimum = FAULTLINE_JOB_STATUS_RESPONSE_PREFIX_SIZE;
        extra = FAULTLINE_JOB_MAX_RESULT_SIZE;
        break;
    case FAULTLINE_MSG_JOB_STATUS_NOT_FOUND:
        *minimum = FAULTLINE_JOB_STATUS_NOT_FOUND_PAYLOAD_SIZE;
        break;
    case FAULTLINE_MSG_JOBS_RESPONSE:
        *minimum = FAULTLINE_JOBS_PREFIX_SIZE;
        extra = FAULTLINE_JOBS_MAX_ENTRIES * FAULTLINE_JOB_SUMMARY_SIZE;
        break;
    case FAULTLINE_MSG_WORKERS_RESPONSE:
        *minimum = FAULTLINE_WORKERS_PREFIX_SIZE;
        extra = FAULTLINE_WORKERS_MAX_ENTRIES * FAULTLINE_WORKER_SUMMARY_SIZE;
        break;
    case FAULTLINE_MSG_STATS_RESPONSE:
        *minimum = FAULTLINE_STATS_PAYLOAD_SIZE;
        break;
    default:
        return FAULTLINE_PROTOCOL_UNKNOWN_MESSAGE_TYPE;
    }
    *maximum = *minimum + extra;
    return FAULTLINE_PROTOCOL_OK;
}

static enum faultline_protocol_result validate_header(
    const struct faultline_header *header)
{
    uint32_t minimum, maximum;

    if (header->magic != FAULTLINE_PROTOCOL_MAGIC) {
        return FAULTLINE_PROTOCOL_BAD_MAGIC;
    }
    if (header->version != FAULTLINE_PROTOCOL_VERSION) {
        return FAULTLINE_PROTOCOL_UNSUPPORTED_VERSION;
    }
    if (message_payload_bounds(header->message_type, &minimum, &maximum) !=
        FAULTLINE_PROTOCOL_OK) {
        return FAULTLINE_PROTOCOL_UNKNOWN_MESSAGE_TYPE;
    }
    if (header->payload_length > FAULTLINE_MAX_PAYLOAD_SIZE) {
        return FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE;
    }

    return FAULTLINE_PROTOCOL_OK;
}

enum faultline_protocol_result faultline_header_encode(
    uint8_t *wire, size_t wire_size, const struct faultline_header *header)
{
    enum faultline_protocol_result result;

    if (wire == NULL || header == NULL) {
        return FAULTLINE_PROTOCOL_INVALID_ARGUMENT;
    }
    if (wire_size < FAULTLINE_HEADER_SIZE) {
        return FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL;
    }

    result = validate_header(header);
    if (result != FAULTLINE_PROTOCOL_OK) {
        return result;
    }

    write_u32_be(wire, header->magic);
    write_u16_be(wire + 4, header->version);
    write_u16_be(wire + 6, header->message_type);
    write_u32_be(wire + 8, header->payload_length);
    return FAULTLINE_PROTOCOL_OK;
}

enum faultline_protocol_result faultline_header_decode(
    const uint8_t *wire, size_t wire_size, struct faultline_header *header)
{
    struct faultline_header decoded;
    enum faultline_protocol_result result;

    if (wire == NULL || header == NULL) {
        return FAULTLINE_PROTOCOL_INVALID_ARGUMENT;
    }
    if (wire_size < FAULTLINE_HEADER_SIZE) {
        return FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL;
    }

    decoded.magic = read_u32_be(wire);
    decoded.version = read_u16_be(wire + 4);
    decoded.message_type = read_u16_be(wire + 6);
    decoded.payload_length = read_u32_be(wire + 8);

    result = validate_header(&decoded);
    if (result != FAULTLINE_PROTOCOL_OK) {
        return result;
    }

    *header = decoded;
    return FAULTLINE_PROTOCOL_OK;
}

static int valid_task(uint16_t task_type)
{
    return task_type == FAULTLINE_TASK_SLEEP || task_type == FAULTLINE_TASK_PRIME_COUNT ||
           task_type == FAULTLINE_TASK_FIBONACCI || task_type == FAULTLINE_TASK_HASH;
}

static enum faultline_protocol_result validate_identity(
    const struct faultline_job_identity *identity)
{
    if (identity->job_id == 0) {
        return FAULTLINE_PROTOCOL_INVALID_JOB_ID;
    }
    if (identity->worker_id == FAULTLINE_WORKER_ID_UNASSIGNED) {
        return FAULTLINE_PROTOCOL_INVALID_WORKER_ID;
    }
    if (identity->attempt == 0) {
        return FAULTLINE_PROTOCOL_INVALID_ATTEMPT;
    }
    return FAULTLINE_PROTOCOL_OK;
}

static enum faultline_protocol_result validate_job_status(
    const struct faultline_job_status_payload *status)
{
    if (status->job_id == 0) { return FAULTLINE_PROTOCOL_INVALID_JOB_ID; }
    if (status->state < FAULTLINE_JOB_QUEUED || status->state > FAULTLINE_JOB_FAILED) {
        return FAULTLINE_PROTOCOL_INVALID_JOB_STATE;
    }
    if (status->retry_count > status->max_retries ||
        (status->state == FAULTLINE_JOB_FAILED && status->retry_count != status->max_retries)) {
        return FAULTLINE_PROTOCOL_INVALID_RETRY_COUNT;
    }
    int queued = status->state == FAULTLINE_JOB_QUEUED;
    if ((queued && status->worker_id != 0) || (!queued && status->worker_id == 0)) {
        return FAULTLINE_PROTOCOL_INVALID_WORKER_ID;
    }
    /* Widen before adding: UINT32_MAX retries still permits attempt 2^32. */
    uint64_t expected_attempt = (uint64_t)status->retry_count + (queued ? 0u : 1u);
    if (status->attempt != expected_attempt) { return FAULTLINE_PROTOCOL_INVALID_ATTEMPT; }
    int has_failure = status->state == FAULTLINE_JOB_FAILED || (queued && status->attempt != 0);
    if (has_failure ? (status->failure != FAULTLINE_JOB_FAILURE_TASK &&
                       status->failure != FAULTLINE_JOB_FAILURE_WORKER_LOST) :
                      status->failure != FAULTLINE_JOB_FAILURE_NONE) {
        return FAULTLINE_PROTOCOL_INVALID_FAILURE;
    }
    if (status->state != FAULTLINE_JOB_DONE && status->result_size != 0) {
        return FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH;
    }
    return FAULTLINE_PROTOCOL_OK;
}

static enum faultline_protocol_result validate_jobs(const struct faultline_jobs_payload *list)
{
    if (list->count > FAULTLINE_JOBS_MAX_ENTRIES) { return FAULTLINE_PROTOCOL_INVALID_LIST_COUNT; }
    for (size_t i = 0; i < list->count; ++i) {
        const struct faultline_job_summary *entry = &list->entries[i];
        const struct faultline_job_status_payload status = {
            .job_id = entry->job_id, .state = entry->state, .worker_id = entry->worker_id,
            .attempt = entry->attempt, .retry_count = entry->retry_count,
            .max_retries = entry->max_retries, .failure = entry->failure, .result_size = entry->result_size
        };
        enum faultline_protocol_result result = validate_job_status(&status);
        if (result != FAULTLINE_PROTOCOL_OK) { return result; }
        if (!valid_task(entry->task_type)) { return FAULTLINE_PROTOCOL_INVALID_TASK_TYPE; }
        if (entry->result_size > FAULTLINE_JOB_MAX_RESULT_SIZE) { return FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE; }
        if (i != 0 && entry->job_id <= list->entries[i - 1].job_id) {
            return FAULTLINE_PROTOCOL_INVALID_LIST_ORDER;
        }
    }
    return FAULTLINE_PROTOCOL_OK;
}

static enum faultline_protocol_result validate_workers(const struct faultline_workers_payload *list)
{
    if (list->count > FAULTLINE_WORKERS_MAX_ENTRIES) { return FAULTLINE_PROTOCOL_INVALID_LIST_COUNT; }
    if (list->heartbeat_timeout_ms == 0) { return FAULTLINE_PROTOCOL_INVALID_HEARTBEAT_TIMEOUT; }
    for (size_t i = 0; i < list->count; ++i) {
        const struct faultline_worker_summary *entry = &list->entries[i];
        if (entry->worker_id == 0) { return FAULTLINE_PROTOCOL_INVALID_WORKER_ID; }
        if (entry->state != FAULTLINE_WORKER_VIEW_ALIVE && entry->state != FAULTLINE_WORKER_VIEW_DEAD) {
            return FAULTLINE_PROTOCOL_INVALID_WORKER_STATE;
        }
        if ((entry->job_id == 0) != (entry->attempt == 0)) { return FAULTLINE_PROTOCOL_INVALID_ATTEMPT; }
        if (entry->state == FAULTLINE_WORKER_VIEW_DEAD && entry->job_id != 0) {
            return FAULTLINE_PROTOCOL_INVALID_JOB_ID;
        }
        if (i != 0 && entry->worker_id <= list->entries[i - 1].worker_id) {
            return FAULTLINE_PROTOCOL_INVALID_LIST_ORDER;
        }
    }
    return FAULTLINE_PROTOCOL_OK;
}

static enum faultline_protocol_result validate_stats(const struct faultline_stats_payload *s)
{
    /* Bound operands before arithmetic: malicious uint64 values must not wrap. */
    if (s->jobs_submitted_total > FAULTLINE_JOBS_MAX_ENTRIES ||
        s->jobs_queued > s->jobs_submitted_total || s->jobs_assigned > s->jobs_submitted_total ||
        s->jobs_running > s->jobs_submitted_total || s->jobs_completed_total > s->jobs_submitted_total ||
        s->jobs_failed_total > s->jobs_submitted_total ||
        s->job_retries_total > s->jobs_submitted_total * UINT32_MAX ||
        s->workers_retained > FAULTLINE_WORKERS_MAX_ENTRIES ||
        s->workers_alive > s->workers_retained || s->workers_expired > s->workers_retained ||
        s->workers_dead > s->workers_retained || s->workers_busy > s->workers_alive ||
        s->workers_idle > s->workers_alive) {
        return FAULTLINE_PROTOCOL_INVALID_STATS;
    }
    if (s->jobs_queued + s->jobs_assigned + s->jobs_running + s->jobs_completed_total +
        s->jobs_failed_total != s->jobs_submitted_total ||
        s->job_attempts_total != s->job_retries_total + s->jobs_submitted_total - s->jobs_queued ||
        s->workers_alive + s->workers_expired + s->workers_dead != s->workers_retained ||
        s->workers_busy + s->workers_idle != s->workers_alive ||
        s->workers_busy > s->jobs_assigned + s->jobs_running ||
        s->startup_jobs_recovered > s->jobs_submitted_total ||
        s->startup_interrupted_jobs > s->startup_jobs_recovered ||
        s->session_jobs_submitted != s->jobs_submitted_total - s->startup_jobs_recovered ||
        s->session_jobs_completed > s->jobs_completed_total ||
        s->session_jobs_failed > s->jobs_failed_total ||
        s->session_job_retries > s->job_retries_total ||
        s->completed_latency_avg_ms > INT64_MAX ||
        (s->jobs_completed_total == 0 && s->completed_latency_avg_ms != 0) ||
        s->session_uptime_ms > INT64_MAX || s->startup_duration_ms > INT64_MAX ||
        s->heartbeat_timeout_ms == 0 || s->heartbeat_timeout_ms > INT_MAX) {
        return FAULTLINE_PROTOCOL_INVALID_STATS;
    }
    return FAULTLINE_PROTOCOL_OK;
}

/* Validate host values and derive the exact length before touching output bytes. */
static enum faultline_protocol_result validate_message(
    const struct faultline_message *message, uint32_t *payload_size)
{
    const struct faultline_job_identity *identity = NULL;
    uint32_t minimum, maximum;
    size_t data_size = 0;
    enum faultline_protocol_result result =
        message_payload_bounds(message->message_type, &minimum, &maximum);

    if (result != FAULTLINE_PROTOCOL_OK) {
        return result;
    }
    switch (message->message_type) {
    case FAULTLINE_MSG_PING:
    case FAULTLINE_MSG_PONG:
    case FAULTLINE_MSG_WORKER_REGISTER:
    case FAULTLINE_MSG_JOBS_REQUEST:
    case FAULTLINE_MSG_WORKERS_REQUEST:
    case FAULTLINE_MSG_STATS_REQUEST:
        if (message->payload.worker_id != 0) {
            return FAULTLINE_PROTOCOL_INVALID_WORKER_ID;
        }
        break;
    case FAULTLINE_MSG_WORKER_REGISTER_ACK:
    case FAULTLINE_MSG_HEARTBEAT:
        if (message->payload.worker_id == 0) {
            return FAULTLINE_PROTOCOL_INVALID_WORKER_ID;
        }
        break;
    case FAULTLINE_MSG_JOB_SUBMIT:
        if (!valid_task(message->payload.job_submit.task_type)) {
            return FAULTLINE_PROTOCOL_INVALID_TASK_TYPE;
        }
        data_size = message->payload.job_submit.argument_size;
        break;
    case FAULTLINE_MSG_JOB_SUBMIT_ACK:
        if (message->payload.job_submit_ack == 0) {
            return FAULTLINE_PROTOCOL_INVALID_JOB_ID;
        }
        break;
    case FAULTLINE_MSG_JOB_ASSIGN:
        identity = &message->payload.job_assign.identity;
        if (!valid_task(message->payload.job_assign.task_type)) {
            return FAULTLINE_PROTOCOL_INVALID_TASK_TYPE;
        }
        data_size = message->payload.job_assign.argument_size;
        break;
    case FAULTLINE_MSG_JOB_STARTED:
        identity = &message->payload.job_started;
        break;
    case FAULTLINE_MSG_JOB_COMPLETED:
        identity = &message->payload.job_completed.identity;
        data_size = message->payload.job_completed.result_size;
        break;
    case FAULTLINE_MSG_JOB_FAILED:
        identity = &message->payload.job_failed.identity;
        if (message->payload.job_failed.failure != FAULTLINE_JOB_FAILURE_TASK) {
            return FAULTLINE_PROTOCOL_INVALID_FAILURE;
        }
        break;
    case FAULTLINE_MSG_JOB_STATUS_REQUEST:
        if (message->payload.job_status_request == 0) { return FAULTLINE_PROTOCOL_INVALID_JOB_ID; }
        break;
    case FAULTLINE_MSG_JOB_STATUS_RESPONSE:
        result = validate_job_status(&message->payload.job_status_response);
        if (result != FAULTLINE_PROTOCOL_OK) { return result; }
        data_size = message->payload.job_status_response.result_size;
        break;
    case FAULTLINE_MSG_JOB_STATUS_NOT_FOUND:
        if (message->payload.job_status_not_found == 0) { return FAULTLINE_PROTOCOL_INVALID_JOB_ID; }
        break;
    case FAULTLINE_MSG_JOBS_RESPONSE:
        result = validate_jobs(&message->payload.jobs);
        if (result != FAULTLINE_PROTOCOL_OK) { return result; }
        data_size = message->payload.jobs.count * FAULTLINE_JOB_SUMMARY_SIZE;
        break;
    case FAULTLINE_MSG_WORKERS_RESPONSE:
        result = validate_workers(&message->payload.workers);
        if (result != FAULTLINE_PROTOCOL_OK) { return result; }
        data_size = message->payload.workers.count * FAULTLINE_WORKER_SUMMARY_SIZE;
        break;
    case FAULTLINE_MSG_STATS_RESPONSE:
        result = validate_stats(&message->payload.stats);
        if (result != FAULTLINE_PROTOCOL_OK) { return result; }
        break;
    default:
        return FAULTLINE_PROTOCOL_UNKNOWN_MESSAGE_TYPE;
    }
    if (identity != NULL) {
        result = validate_identity(identity);
        if (result != FAULTLINE_PROTOCOL_OK) {
            return result;
        }
    }
    /* Bound size_t before narrowing it or adding prefix bytes (including SIZE_MAX). */
    if (data_size > (size_t)(maximum - minimum)) {
        return FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE;
    }
    *payload_size = minimum + (uint32_t)data_size;
    return FAULTLINE_PROTOCOL_OK;
}

static void write_identity(uint8_t *out, const struct faultline_job_identity *identity)
{
    write_u64_be(out, identity->job_id);
    write_u32_be(out + 8, identity->worker_id);
    write_u64_be(out + 12, identity->attempt);
}

static void read_identity(const uint8_t *in, struct faultline_job_identity *identity)
{
    identity->job_id = read_u64_be(in);
    identity->worker_id = read_u32_be(in + 8);
    identity->attempt = read_u64_be(in + 12);
}

enum faultline_protocol_result faultline_message_encode(
    uint8_t *wire, size_t wire_size, const struct faultline_message *message,
    size_t *written)
{
    struct faultline_header header = {
        .magic = FAULTLINE_PROTOCOL_MAGIC,
        .version = FAULTLINE_PROTOCOL_VERSION
    };
    enum faultline_protocol_result result;
    size_t frame_size;
    uint8_t *payload;

    if (wire == NULL || message == NULL || written == NULL) {
        return FAULTLINE_PROTOCOL_INVALID_ARGUMENT;
    }
    header.message_type = message->message_type;
    result = validate_message(message, &header.payload_length);
    if (result != FAULTLINE_PROTOCOL_OK) {
        return result;
    }
    frame_size = FAULTLINE_HEADER_SIZE + (size_t)header.payload_length;
    if (wire_size < frame_size) {
        return FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL;
    }
    result = faultline_header_encode(wire, wire_size, &header);
    if (result != FAULTLINE_PROTOCOL_OK) {
        return result;
    }

    payload = wire + FAULTLINE_HEADER_SIZE;
    switch (message->message_type) {
    case FAULTLINE_MSG_WORKER_REGISTER_ACK:
    case FAULTLINE_MSG_HEARTBEAT:
        write_u32_be(payload, message->payload.worker_id);
        break;
    case FAULTLINE_MSG_JOB_SUBMIT: {
        const struct faultline_job_submit_payload *submit = &message->payload.job_submit;
        write_u16_be(payload, submit->task_type);
        write_u32_be(payload + 2, submit->max_retries);
        write_u32_be(payload + 6, (uint32_t)submit->argument_size);
        memcpy(payload + 10, submit->arguments, submit->argument_size);
        break;
    }
    case FAULTLINE_MSG_JOB_SUBMIT_ACK:
        write_u64_be(payload, message->payload.job_submit_ack);
        break;
    case FAULTLINE_MSG_JOB_ASSIGN: {
        const struct faultline_job_assign_payload *assign = &message->payload.job_assign;
        write_identity(payload, &assign->identity);
        write_u16_be(payload + 20, assign->task_type);
        write_u32_be(payload + 22, (uint32_t)assign->argument_size);
        memcpy(payload + 26, assign->arguments, assign->argument_size);
        break;
    }
    case FAULTLINE_MSG_JOB_STARTED:
        write_identity(payload, &message->payload.job_started);
        break;
    case FAULTLINE_MSG_JOB_COMPLETED: {
        const struct faultline_job_completed_payload *completed = &message->payload.job_completed;
        write_identity(payload, &completed->identity);
        write_u32_be(payload + 20, (uint32_t)completed->result_size);
        memcpy(payload + 24, completed->result, completed->result_size);
        break;
    }
    case FAULTLINE_MSG_JOB_FAILED:
        write_identity(payload, &message->payload.job_failed.identity);
        write_u16_be(payload + 20, message->payload.job_failed.failure);
        break;
    case FAULTLINE_MSG_JOB_STATUS_REQUEST:
        write_u64_be(payload, message->payload.job_status_request);
        break;
    case FAULTLINE_MSG_JOB_STATUS_RESPONSE: {
        const struct faultline_job_status_payload *status = &message->payload.job_status_response;
        write_u64_be(payload, status->job_id);
        write_u16_be(payload + 8, status->state);
        write_u32_be(payload + 10, status->worker_id);
        write_u64_be(payload + 14, status->attempt);
        write_u32_be(payload + 22, status->retry_count);
        write_u32_be(payload + 26, status->max_retries);
        write_u16_be(payload + 30, status->failure);
        write_u32_be(payload + 32, (uint32_t)status->result_size);
        memcpy(payload + 36, status->result, status->result_size);
        break;
    }
    case FAULTLINE_MSG_JOB_STATUS_NOT_FOUND:
        write_u64_be(payload, message->payload.job_status_not_found);
        break;
    case FAULTLINE_MSG_JOBS_RESPONSE:
        write_u32_be(payload, (uint32_t)message->payload.jobs.count);
        for (size_t i = 0; i < message->payload.jobs.count; ++i) {
            uint8_t *out = payload + FAULTLINE_JOBS_PREFIX_SIZE + i * FAULTLINE_JOB_SUMMARY_SIZE;
            const struct faultline_job_summary *entry = &message->payload.jobs.entries[i];
            write_u64_be(out, entry->job_id);
            write_u16_be(out + 8, entry->task_type);
            write_u16_be(out + 10, entry->state);
            write_u32_be(out + 12, entry->worker_id);
            write_u64_be(out + 16, entry->attempt);
            write_u32_be(out + 24, entry->retry_count);
            write_u32_be(out + 28, entry->max_retries);
            write_u16_be(out + 32, entry->failure);
            write_u32_be(out + 34, entry->result_size);
        }
        break;
    case FAULTLINE_MSG_WORKERS_RESPONSE:
        write_u32_be(payload, (uint32_t)message->payload.workers.count);
        write_u32_be(payload + 4, message->payload.workers.heartbeat_timeout_ms);
        for (size_t i = 0; i < message->payload.workers.count; ++i) {
            uint8_t *out = payload + FAULTLINE_WORKERS_PREFIX_SIZE + i * FAULTLINE_WORKER_SUMMARY_SIZE;
            const struct faultline_worker_summary *entry = &message->payload.workers.entries[i];
            write_u32_be(out, entry->worker_id);
            write_u16_be(out + 4, entry->state);
            write_u64_be(out + 6, entry->heartbeat_age_ms);
            write_u64_be(out + 14, entry->job_id);
            write_u64_be(out + 22, entry->attempt);
        }
        break;
    case FAULTLINE_MSG_STATS_RESPONSE: {
        const struct faultline_stats_payload *stats = &message->payload.stats;
        write_u64_be(payload + 0, stats->jobs_submitted_total);
        write_u64_be(payload + 8, stats->jobs_queued);
        write_u64_be(payload + 16, stats->jobs_assigned);
        write_u64_be(payload + 24, stats->jobs_running);
        write_u64_be(payload + 32, stats->jobs_completed_total);
        write_u64_be(payload + 40, stats->jobs_failed_total);
        write_u64_be(payload + 48, stats->job_attempts_total);
        write_u64_be(payload + 56, stats->job_retries_total);
        write_u64_be(payload + 64, stats->completed_latency_avg_ms);
        write_u64_be(payload + 72, stats->workers_retained);
        write_u64_be(payload + 80, stats->workers_alive);
        write_u64_be(payload + 88, stats->workers_expired);
        write_u64_be(payload + 96, stats->workers_dead);
        write_u64_be(payload + 104, stats->workers_busy);
        write_u64_be(payload + 112, stats->workers_idle);
        write_u64_be(payload + 120, stats->session_uptime_ms);
        write_u64_be(payload + 128, stats->session_jobs_submitted);
        write_u64_be(payload + 136, stats->session_jobs_completed);
        write_u64_be(payload + 144, stats->session_jobs_failed);
        write_u64_be(payload + 152, stats->session_job_retries);
        write_u64_be(payload + 160, stats->startup_jobs_recovered);
        write_u64_be(payload + 168, stats->startup_interrupted_jobs);
        write_u64_be(payload + 176, stats->startup_duration_ms);
        write_u64_be(payload + 184, stats->heartbeat_timeout_ms);
        break;
    }
    default: /* Validated empty messages have nothing to write. */
        break;
    }
    *written = frame_size;
    return FAULTLINE_PROTOCOL_OK;
}

enum faultline_protocol_result faultline_message_decode(
    const uint8_t *wire, size_t wire_size, struct faultline_message *message,
    size_t *consumed)
{
    struct faultline_header header;
    struct faultline_message decoded = {0};
    enum faultline_protocol_result result;
    uint32_t minimum, maximum, expected_size;
    size_t frame_size;
    const uint8_t *payload;
    uint8_t *data = NULL;

    if (wire == NULL || message == NULL || consumed == NULL) {
        return FAULTLINE_PROTOCOL_INVALID_ARGUMENT;
    }
    result = faultline_header_decode(wire, wire_size, &header);
    if (result != FAULTLINE_PROTOCOL_OK) {
        return result;
    }
    result = message_payload_bounds(header.message_type, &minimum, &maximum);
    if (result != FAULTLINE_PROTOCOL_OK) {
        return result;
    }
    if (header.payload_length < minimum ||
        (minimum == maximum && header.payload_length != minimum)) {
        return FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH;
    }
    if (header.payload_length > maximum) {
        return FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE;
    }
    if (wire_size < FAULTLINE_HEADER_SIZE + (size_t)minimum) {
        return FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL;
    }

    /* The whole fixed prefix is present. Do not read variable data yet. */
    payload = wire + FAULTLINE_HEADER_SIZE;
    decoded.message_type = header.message_type;
    switch (header.message_type) {
    case FAULTLINE_MSG_WORKER_REGISTER_ACK:
    case FAULTLINE_MSG_HEARTBEAT:
        decoded.payload.worker_id = read_u32_be(payload);
        break;
    case FAULTLINE_MSG_JOB_SUBMIT:
        decoded.payload.job_submit.task_type = read_u16_be(payload);
        decoded.payload.job_submit.max_retries = read_u32_be(payload + 2);
        decoded.payload.job_submit.argument_size = read_u32_be(payload + 6);
        data = decoded.payload.job_submit.arguments;
        break;
    case FAULTLINE_MSG_JOB_SUBMIT_ACK:
        decoded.payload.job_submit_ack = read_u64_be(payload);
        break;
    case FAULTLINE_MSG_JOB_ASSIGN:
        read_identity(payload, &decoded.payload.job_assign.identity);
        decoded.payload.job_assign.task_type = read_u16_be(payload + 20);
        decoded.payload.job_assign.argument_size = read_u32_be(payload + 22);
        data = decoded.payload.job_assign.arguments;
        break;
    case FAULTLINE_MSG_JOB_STARTED:
        read_identity(payload, &decoded.payload.job_started);
        break;
    case FAULTLINE_MSG_JOB_COMPLETED:
        read_identity(payload, &decoded.payload.job_completed.identity);
        decoded.payload.job_completed.result_size = read_u32_be(payload + 20);
        data = decoded.payload.job_completed.result;
        break;
    case FAULTLINE_MSG_JOB_FAILED:
        read_identity(payload, &decoded.payload.job_failed.identity);
        decoded.payload.job_failed.failure = read_u16_be(payload + 20);
        break;
    case FAULTLINE_MSG_JOB_STATUS_REQUEST:
        decoded.payload.job_status_request = read_u64_be(payload);
        break;
    case FAULTLINE_MSG_JOB_STATUS_RESPONSE: {
        struct faultline_job_status_payload *status = &decoded.payload.job_status_response;
        status->job_id = read_u64_be(payload);
        status->state = read_u16_be(payload + 8);
        status->worker_id = read_u32_be(payload + 10);
        status->attempt = read_u64_be(payload + 14);
        status->retry_count = read_u32_be(payload + 22);
        status->max_retries = read_u32_be(payload + 26);
        status->failure = read_u16_be(payload + 30);
        status->result_size = read_u32_be(payload + 32);
        data = status->result;
        break;
    }
    case FAULTLINE_MSG_JOB_STATUS_NOT_FOUND:
        decoded.payload.job_status_not_found = read_u64_be(payload);
        break;
    case FAULTLINE_MSG_JOBS_RESPONSE:
    case FAULTLINE_MSG_WORKERS_RESPONSE: {
        int jobs = header.message_type == FAULTLINE_MSG_JOBS_RESPONSE;
        uint32_t count = read_u32_be(payload);
        uint32_t limit = jobs ? FAULTLINE_JOBS_MAX_ENTRIES : FAULTLINE_WORKERS_MAX_ENTRIES;
        uint32_t stride = jobs ? FAULTLINE_JOB_SUMMARY_SIZE : FAULTLINE_WORKER_SUMMARY_SIZE;
        if (count > limit) { return FAULTLINE_PROTOCOL_INVALID_LIST_COUNT; }
        if (header.payload_length != minimum + count * stride) { return FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH; }
        if (!jobs && read_u32_be(payload + 4) == 0) { return FAULTLINE_PROTOCOL_INVALID_HEARTBEAT_TIMEOUT; }
        if (wire_size < FAULTLINE_HEADER_SIZE + (size_t)header.payload_length) {
            return FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL;
        }
        if (jobs) { decoded.payload.jobs.count = count; }
        else {
            decoded.payload.workers.count = count;
            decoded.payload.workers.heartbeat_timeout_ms = read_u32_be(payload + 4);
        }
        for (size_t i = 0; i < count; ++i) {
            const uint8_t *in = payload + minimum + i * stride;
            if (jobs) {
                decoded.payload.jobs.entries[i] = (struct faultline_job_summary){
                    .job_id = read_u64_be(in), .task_type = read_u16_be(in + 8),
                    .state = read_u16_be(in + 10), .worker_id = read_u32_be(in + 12),
                    .attempt = read_u64_be(in + 16), .retry_count = read_u32_be(in + 24),
                    .max_retries = read_u32_be(in + 28), .failure = read_u16_be(in + 32),
                    .result_size = read_u32_be(in + 34)
                };
            } else {
                decoded.payload.workers.entries[i] = (struct faultline_worker_summary){
                    .worker_id = read_u32_be(in), .state = read_u16_be(in + 4),
                    .heartbeat_age_ms = read_u64_be(in + 6), .job_id = read_u64_be(in + 14),
                    .attempt = read_u64_be(in + 22)
                };
            }
        }
        break;
    }
    case FAULTLINE_MSG_STATS_RESPONSE: {
        struct faultline_stats_payload *stats = &decoded.payload.stats;
        stats->jobs_submitted_total = read_u64_be(payload + 0);
        stats->jobs_queued = read_u64_be(payload + 8);
        stats->jobs_assigned = read_u64_be(payload + 16);
        stats->jobs_running = read_u64_be(payload + 24);
        stats->jobs_completed_total = read_u64_be(payload + 32);
        stats->jobs_failed_total = read_u64_be(payload + 40);
        stats->job_attempts_total = read_u64_be(payload + 48);
        stats->job_retries_total = read_u64_be(payload + 56);
        stats->completed_latency_avg_ms = read_u64_be(payload + 64);
        stats->workers_retained = read_u64_be(payload + 72);
        stats->workers_alive = read_u64_be(payload + 80);
        stats->workers_expired = read_u64_be(payload + 88);
        stats->workers_dead = read_u64_be(payload + 96);
        stats->workers_busy = read_u64_be(payload + 104);
        stats->workers_idle = read_u64_be(payload + 112);
        stats->session_uptime_ms = read_u64_be(payload + 120);
        stats->session_jobs_submitted = read_u64_be(payload + 128);
        stats->session_jobs_completed = read_u64_be(payload + 136);
        stats->session_jobs_failed = read_u64_be(payload + 144);
        stats->session_job_retries = read_u64_be(payload + 152);
        stats->startup_jobs_recovered = read_u64_be(payload + 160);
        stats->startup_interrupted_jobs = read_u64_be(payload + 168);
        stats->startup_duration_ms = read_u64_be(payload + 176);
        stats->heartbeat_timeout_ms = read_u64_be(payload + 184);
        break;
    }
    default: /* Validated empty messages keep worker_id zero. */
        break;
    }
    result = validate_message(&decoded, &expected_size);
    if (result != FAULTLINE_PROTOCOL_OK) {
        return result;
    }
    if (header.payload_length != expected_size) {
        return FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH;
    }
    frame_size = FAULTLINE_HEADER_SIZE + (size_t)expected_size;
    if (wire_size < frame_size) {
        return FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL;
    }
    if (data != NULL) {
        memcpy(data, payload + minimum, (size_t)(expected_size - minimum));
    }
    *message = decoded;
    *consumed = frame_size;
    return FAULTLINE_PROTOCOL_OK;
}
