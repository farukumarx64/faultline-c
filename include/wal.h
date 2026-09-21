#ifndef FAULTLINE_WAL_H
#define FAULTLINE_WAL_H

#include "job.h"

/* Version 1 disk format. All multibyte fields, including CRCs, are big-endian. */
#define FAULTLINE_WAL_VERSION 1u
#define FAULTLINE_WAL_FILE_HEADER_SIZE 24u
#define FAULTLINE_WAL_RECORD_MAGIC UINT32_C(0x464c5752) /* FLWR */
#define FAULTLINE_WAL_RECORD_HEADER_SIZE 32u
#define FAULTLINE_WAL_JOB_PREFIX_SIZE 84u
#define FAULTLINE_WAL_MAX_PAYLOAD_SIZE \
    (FAULTLINE_WAL_JOB_PREFIX_SIZE + FAULTLINE_JOB_MAX_ARGUMENT_SIZE + FAULTLINE_JOB_MAX_RESULT_SIZE)
#define FAULTLINE_WAL_MAX_RECORD_SIZE \
    (FAULTLINE_WAL_RECORD_HEADER_SIZE + FAULTLINE_WAL_MAX_PAYLOAD_SIZE)

enum faultline_wal_record_type {
    FAULTLINE_WAL_WORKER_ID_ALLOCATED = 1,
    FAULTLINE_WAL_JOB_CREATED = 2,
    FAULTLINE_WAL_JOB_ASSIGNED = 3,
    FAULTLINE_WAL_JOB_STARTED = 4,
    FAULTLINE_WAL_JOB_COMPLETED = 5,
    FAULTLINE_WAL_JOB_REQUEUED = 6,
    FAULTLINE_WAL_JOB_FAILED = 7
};

/* Host representations, not disk layouts. Job records own the post-transition snapshot. */
struct faultline_wal_record {
    enum faultline_wal_record_type type;
    uint64_t sequence;
    union {
        uint32_t worker_id;
        struct faultline_job job;
    } payload;
};

struct faultline_wal_record_header {
    enum faultline_wal_record_type type;
    uint64_t sequence;
    uint32_t payload_length;
    uint32_t payload_crc32;
};

enum faultline_wal_result {
    FAULTLINE_WAL_OK = 0,
    FAULTLINE_WAL_INVALID_ARGUMENT,
    FAULTLINE_WAL_BUFFER_TOO_SMALL, /* Encoder capacity; never an input-tail classification. */
    FAULTLINE_WAL_INCOMPLETE,       /* Decoder needs bytes; caller decides EOF policy. */
    FAULTLINE_WAL_BAD_CHECKSUM,
    FAULTLINE_WAL_BAD_MAGIC,
    FAULTLINE_WAL_UNSUPPORTED_VERSION,
    FAULTLINE_WAL_INVALID_HEADER,
    FAULTLINE_WAL_UNKNOWN_RECORD_TYPE,
    FAULTLINE_WAL_INVALID_SEQUENCE,
    FAULTLINE_WAL_INVALID_PAYLOAD_LENGTH,
    FAULTLINE_WAL_INVALID_PAYLOAD
};

/*
 * Pure codecs: no allocation, file/socket I/O, fsync, replay, or live-state mutation.
 * All pointer arguments are required and must refer to valid non-overlapping storage.
 * On any error all outputs remain unchanged. Encoders leave extra capacity untouched;
 * decoders ignore following bytes (record_decode reports this record's consumed size).
 */
enum faultline_wal_result faultline_wal_file_header_encode(uint8_t *wire, size_t capacity);
enum faultline_wal_result faultline_wal_file_header_decode(const uint8_t *wire, size_t available);

/*
 * Validate the complete fixed header and its CRC BEFORE trusting any length.
 * expected_sequence must be nonzero; the caller starts at 1 and advances only
 * after a complete accepted record. UINT64_MAX is allowed, then no successor exists.
 * Success here does not validate payload availability, CRC, or contents.
 */
enum faultline_wal_result faultline_wal_record_header_decode(
    const uint8_t *wire, size_t available, uint64_t expected_sequence,
    struct faultline_wal_record_header *header);

enum faultline_wal_result faultline_wal_record_encode(
    uint8_t *wire, size_t capacity, const struct faultline_wal_record *record, size_t *written);
enum faultline_wal_result faultline_wal_record_decode(
    const uint8_t *wire, size_t available, uint64_t expected_sequence,
    struct faultline_wal_record *record, size_t *consumed);

/*
 * INCOMPLETE is not permission to truncate a file. Future file reading must first
 * validate the file header and every preceding record and observe actual EOF.
 * An incomplete file header is a startup error. Complete corrupt records are errors,
 * even at EOF. Cross-record transitions/IDs/FIFO and durability belong to replay/I/O.
 */

#endif
