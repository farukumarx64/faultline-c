#ifndef FAULTLINE_PROTOCOL_H
#define FAULTLINE_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#include "job.h"

/* Wire bytes 46 4c 49 4e spell "FLIN" in ASCII. */
#define FAULTLINE_PROTOCOL_MAGIC UINT32_C(0x464c494e)
#define FAULTLINE_PROTOCOL_VERSION UINT16_C(1)
#define FAULTLINE_HEADER_SIZE 12u
#define FAULTLINE_MAX_PAYLOAD_SIZE UINT32_C(1048576)
#define FAULTLINE_WORKER_REGISTER_PAYLOAD_SIZE 0u
#define FAULTLINE_WORKER_REGISTER_ACK_PAYLOAD_SIZE 4u
#define FAULTLINE_HEARTBEAT_PAYLOAD_SIZE 4u
#define FAULTLINE_WORKER_ID_UNASSIGNED UINT32_C(0)

/* Prefix sizes exclude both the header and any argument/result bytes. */
#define FAULTLINE_JOB_SUBMIT_PREFIX_SIZE 10u
#define FAULTLINE_JOB_SUBMIT_ACK_PAYLOAD_SIZE 8u
#define FAULTLINE_JOB_ASSIGN_PREFIX_SIZE 26u
#define FAULTLINE_JOB_STARTED_PAYLOAD_SIZE 20u
#define FAULTLINE_JOB_COMPLETED_PREFIX_SIZE 24u
#define FAULTLINE_JOB_FAILED_PAYLOAD_SIZE 22u
#define FAULTLINE_JOB_STATUS_REQUEST_PAYLOAD_SIZE 8u
#define FAULTLINE_JOB_STATUS_RESPONSE_PREFIX_SIZE 36u
#define FAULTLINE_JOB_STATUS_NOT_FOUND_PAYLOAD_SIZE 8u
#define FAULTLINE_JOBS_MAX_ENTRIES 256u
#define FAULTLINE_WORKERS_MAX_ENTRIES 64u
#define FAULTLINE_JOBS_PREFIX_SIZE 4u
#define FAULTLINE_JOB_SUMMARY_SIZE 38u
#define FAULTLINE_WORKERS_PREFIX_SIZE 8u
#define FAULTLINE_WORKER_SUMMARY_SIZE 30u
#define FAULTLINE_MESSAGE_MAX_FRAME_SIZE \
    (FAULTLINE_HEADER_SIZE + FAULTLINE_JOBS_PREFIX_SIZE + FAULTLINE_JOBS_MAX_ENTRIES * FAULTLINE_JOB_SUMMARY_SIZE)

enum faultline_message_type {
    FAULTLINE_MSG_PING = 1,
    FAULTLINE_MSG_PONG = 2,
    FAULTLINE_MSG_WORKER_REGISTER = 3,
    FAULTLINE_MSG_WORKER_REGISTER_ACK = 4,
    FAULTLINE_MSG_HEARTBEAT = 5,
    FAULTLINE_MSG_JOB_SUBMIT = 6,
    FAULTLINE_MSG_JOB_SUBMIT_ACK = 7,
    FAULTLINE_MSG_JOB_ASSIGN = 8,
    FAULTLINE_MSG_JOB_STARTED = 9,
    FAULTLINE_MSG_JOB_COMPLETED = 10,
    FAULTLINE_MSG_JOB_FAILED = 11,
    FAULTLINE_MSG_JOB_STATUS_REQUEST = 12,
    FAULTLINE_MSG_JOB_STATUS_RESPONSE = 13,
    FAULTLINE_MSG_JOB_STATUS_NOT_FOUND = 14,
    FAULTLINE_MSG_JOBS_REQUEST = 15,
    FAULTLINE_MSG_JOBS_RESPONSE = 16,
    FAULTLINE_MSG_WORKERS_REQUEST = 17,
    FAULTLINE_MSG_WORKERS_RESPONSE = 18
};

/* Host-order values only. Never send this struct directly over a socket. */
struct faultline_header {
    uint32_t magic;
    uint16_t version;
    uint16_t message_type;
    uint32_t payload_length;
};

/* All three values are nonzero. Validation of current ownership is up to handlers. */
struct faultline_job_identity {
    uint64_t job_id;
    uint32_t worker_id;
    uint64_t attempt;
};

struct faultline_job_submit_payload {
    uint16_t task_type;
    uint32_t max_retries;
    size_t argument_size;
    uint8_t arguments[FAULTLINE_JOB_MAX_ARGUMENT_SIZE];
};

struct faultline_job_assign_payload {
    struct faultline_job_identity identity;
    uint16_t task_type;
    size_t argument_size;
    uint8_t arguments[FAULTLINE_JOB_MAX_ARGUMENT_SIZE];
};

struct faultline_job_completed_payload {
    struct faultline_job_identity identity;
    size_t result_size;
    uint8_t result[FAULTLINE_JOB_MAX_RESULT_SIZE];
};

struct faultline_job_failed_payload {
    struct faultline_job_identity identity;
    uint16_t failure; /* Only TASK is a worker report; WORKER_LOST is coordinator-decided. */
};

/* A read-only snapshot, not an assignment or a worker execution report.
 * QUEUED has no owner; DONE/FAILED retain their last worker as history.
 * Only DONE carries result bytes. A requeued job retains its last failure.
 * See docs/job-status-protocol.md for state/counter consistency rules. */
struct faultline_job_status_payload {
    uint64_t job_id;
    uint16_t state;
    uint32_t worker_id;
    uint64_t attempt;
    uint32_t retry_count;
    uint32_t max_retries;
    uint16_t failure;
    size_t result_size;
    uint8_t result[FAULTLINE_JOB_MAX_RESULT_SIZE];
};

/* Bounded, ID-sorted snapshots. Jobs omit argument/result bytes; status retrieves
 * an individual result. Worker ages are coordinator-computed durations, not
 * timestamps in another machine's clock domain. See docs/listings.md. */
struct faultline_job_summary {
    uint64_t job_id;
    uint16_t task_type;
    uint16_t state;
    uint32_t worker_id;
    uint64_t attempt;
    uint32_t retry_count;
    uint32_t max_retries;
    uint16_t failure;
    uint32_t result_size;
};

enum faultline_worker_view_state {
    FAULTLINE_WORKER_VIEW_ALIVE = 1,
    FAULTLINE_WORKER_VIEW_DEAD = 2
};

struct faultline_worker_summary {
    uint32_t worker_id;
    uint16_t state;
    uint64_t heartbeat_age_ms;
    uint64_t job_id; /* Zero when idle/dead; otherwise the current active job. */
    uint64_t attempt; /* Zero iff job_id is zero. */
};

struct faultline_jobs_payload {
    size_t count;
    struct faultline_job_summary entries[FAULTLINE_JOBS_MAX_ENTRIES];
};

struct faultline_workers_payload {
    size_t count;
    uint32_t heartbeat_timeout_ms;
    struct faultline_worker_summary entries[FAULTLINE_WORKERS_MAX_ENTRIES];
};

/*
 * Host-order tagged union: initialize/read only the member named by message_type.
 * Empty messages use payload.worker_id = 0; worker ACK/HEARTBEAT use a nonzero ID.
 * Job byte arrays are owned copies, not strings or borrowed buffer pointers.
 * Never send this struct directly. Wire offsets are in docs/job-protocol.md
 * and docs/job-status-protocol.md.
 */
struct faultline_message {
    uint16_t message_type;
    union {
        uint32_t worker_id;
        struct faultline_job_submit_payload job_submit;
        uint64_t job_submit_ack; /* Successful acceptance: coordinator-issued job ID. */
        struct faultline_job_assign_payload job_assign;
        struct faultline_job_identity job_started;
        struct faultline_job_completed_payload job_completed;
        struct faultline_job_failed_payload job_failed;
        uint64_t job_status_request;   /* Nonzero job ID to inspect. */
        struct faultline_job_status_payload job_status_response;
        uint64_t job_status_not_found; /* Echoes the nonzero requested ID. */
        struct faultline_jobs_payload jobs;
        struct faultline_workers_payload workers;
    } payload;
};

enum faultline_protocol_result {
    FAULTLINE_PROTOCOL_OK = 0,
    FAULTLINE_PROTOCOL_INVALID_ARGUMENT,
    FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL,
    FAULTLINE_PROTOCOL_BAD_MAGIC,
    FAULTLINE_PROTOCOL_UNSUPPORTED_VERSION,
    FAULTLINE_PROTOCOL_UNKNOWN_MESSAGE_TYPE,
    FAULTLINE_PROTOCOL_PAYLOAD_TOO_LARGE,
    FAULTLINE_PROTOCOL_INVALID_PAYLOAD_LENGTH,
    FAULTLINE_PROTOCOL_INVALID_WORKER_ID,
    FAULTLINE_PROTOCOL_INVALID_JOB_ID,
    FAULTLINE_PROTOCOL_INVALID_ATTEMPT,
    FAULTLINE_PROTOCOL_INVALID_TASK_TYPE,
    FAULTLINE_PROTOCOL_INVALID_FAILURE,
    FAULTLINE_PROTOCOL_INVALID_JOB_STATE,
    FAULTLINE_PROTOCOL_INVALID_RETRY_COUNT,
    FAULTLINE_PROTOCOL_INVALID_LIST_COUNT,
    FAULTLINE_PROTOCOL_INVALID_LIST_ORDER,
    FAULTLINE_PROTOCOL_INVALID_WORKER_STATE,
    FAULTLINE_PROTOCOL_INVALID_HEARTBEAT_TIMEOUT
};

/*
 * Validate and encode one header as exactly 12 big-endian bytes.
 * wire_size is the available output capacity. Extra bytes are untouched.
 * On failure, no output is changed. Non-null arguments must refer to valid,
 * non-overlapping storage. No memory is allocated and no socket I/O is done.
 */
enum faultline_protocol_result faultline_header_encode(
    uint8_t *wire, size_t wire_size, const struct faultline_header *header);

/*
 * Decode and validate the first 12 bytes; any following bytes are ignored.
 * A short input returns BUFFER_TOO_SMALL: the caller must gather more bytes.
 * Success validates only the header, not payload availability or contents.
 * On failure, *header is unchanged. The same storage rules as encode apply.
 */
enum faultline_protocol_result faultline_header_decode(
    const uint8_t *wire, size_t wire_size, struct faultline_header *header);

/*
 * Encode a complete message, deriving its header and exact payload length.
 * wire_size is output capacity; *written receives the frame size on success.
 * All pointers are required and storage must be valid and non-overlapping.
 * No output (including *written) changes on failure. Extra capacity is untouched.
 * No allocation, socket I/O, ID assignment, or connection-state checks are done.
 */
enum faultline_protocol_result faultline_message_encode(
    uint8_t *wire, size_t wire_size, const struct faultline_message *message,
    size_t *written);

/*
 * Decode one complete frame from wire_size available bytes. A partial header
 * or payload returns BUFFER_TOO_SMALL unless an available declaration is already
 * invalid. Variable data is bounded and its length must match the outer header.
 * On success, *consumed reports this frame's size so the caller can retain
 * following frames. All output stays unchanged on error.
 * The same pointer/storage rules as message_encode apply.
 */
enum faultline_protocol_result faultline_message_decode(
    const uint8_t *wire, size_t wire_size, struct faultline_message *message,
    size_t *consumed);

#endif
