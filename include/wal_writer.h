#ifndef FAULTLINE_WAL_WRITER_H
#define FAULTLINE_WAL_WRITER_H

#include "wal.h"

enum faultline_wal_writer_state {
    FAULTLINE_WAL_WRITER_CLOSED = 0,
    FAULTLINE_WAL_WRITER_READY,
    FAULTLINE_WAL_WRITER_FAILED,
    FAULTLINE_WAL_WRITER_RECOVERING /* Internal open/validation; appends are refused. */
};

enum faultline_wal_writer_result {
    FAULTLINE_WAL_WRITE_OK = 0,
    FAULTLINE_WAL_WRITE_INVALID_ARGUMENT,
    FAULTLINE_WAL_WRITE_INVALID_STATE,
    FAULTLINE_WAL_WRITE_INVALID_RECORD,
    FAULTLINE_WAL_WRITE_SEQUENCE_EXHAUSTED,
    FAULTLINE_WAL_WRITE_IO_ERROR
};

enum faultline_wal_writer_operation {
    FAULTLINE_WAL_IO_NONE = 0,
    FAULTLINE_WAL_IO_ALLOCATE_PATH,
    FAULTLINE_WAL_IO_OPEN_DIRECTORY,
    FAULTLINE_WAL_IO_CREATE_FILE,
    FAULTLINE_WAL_IO_LOCK,
    FAULTLINE_WAL_IO_WRITE_HEADER,
    FAULTLINE_WAL_IO_SYNC_HEADER,
    FAULTLINE_WAL_IO_SYNC_DIRECTORY,
    FAULTLINE_WAL_IO_WRITE_RECORD,
    FAULTLINE_WAL_IO_SYNC_RECORD,
    FAULTLINE_WAL_IO_CLOSE_FILE,
    FAULTLINE_WAL_IO_CLOSE_DIRECTORY,
    FAULTLINE_WAL_IO_OPEN_EXISTING,
    FAULTLINE_WAL_IO_STAT_FILE,
    FAULTLINE_WAL_IO_READ_FILE,
    FAULTLINE_WAL_IO_ALLOCATE_REPLAY,
    FAULTLINE_WAL_IO_VALIDATE_REPLAY,
    FAULTLINE_WAL_IO_TRUNCATE_TAIL,
    FAULTLINE_WAL_IO_SYNC_RECOVERY
};

struct faultline_wal_writer_io;

/*
 * Single-owner, synchronous handle; initialize with the macro below. Treat all
 * fields as read-only. Do not copy a live handle, share it between threads, use
 * it in a fork child, or close/seek/write its descriptors outside this module.
 */
struct faultline_wal_writer {
    int fd;
    int directory_fd;
    enum faultline_wal_writer_state state;
    uint64_t next_sequence;   /* Zero after committing UINT64_MAX. */
    uint64_t synced_sequence; /* Advances only after a complete record and fsync. */
    int system_error;         /* First fatal errno, preserved through cleanup. */
    enum faultline_wal_writer_operation failed_operation;
    const struct faultline_wal_writer_io *io; /* Private implementation details. */
    void *io_context;
};

#define FAULTLINE_WAL_WRITER_INIT { .fd = -1, .directory_fd = -1 }

/*
 * Explicitly initialize a NEW file in an existing directory (mode 0600, subject
 * to umask). Existing final paths, including symlinks, are never overwritten/followed.
 * Hold an exclusive advisory lock; write/sync the header, then sync its parent
 * directory before READY. Use faultline_wal_replay_open to recover an existing WAL.
 * On any I/O failure, close this handle even if create failed. Failed creation
 * may leave a partial/complete file, which is deliberately not deleted/reset.
 */
enum faultline_wal_writer_result faultline_wal_writer_create(
    struct faultline_wal_writer *writer, const char *path);

/*
 * Validate/encode before writing. record->sequence must equal next_sequence.
 * Write every byte, retry EINTR, then fsync. OK means the sync succeeded under
 * the durability contract; only then may a caller publish state or send an ACK.
 * Invalid input/exhaustion performs no I/O and leaves the handle usable.
 * Any ultimate write/sync error permanently fails the handle: no further writes
 * or sync retries through another append. The failed record's survival is unknown.
 */
enum faultline_wal_writer_result faultline_wal_writer_append(
    struct faultline_wal_writer *writer, const struct faultline_wal_record *record);

/*
 * Close descriptors once, without writes, fsync, truncation, unlink, or implicit
 * retries of uncertain appends. Report/latch close errors too. A previous fatal
 * failure remains FAILED and returns IO_ERROR, including on repeated close calls.
 * A successfully closed handle may initialize a different new file.
 */
enum faultline_wal_writer_result faultline_wal_writer_close(struct faultline_wal_writer *writer);

const char *faultline_wal_writer_operation_name(enum faultline_wal_writer_operation operation);

#endif
