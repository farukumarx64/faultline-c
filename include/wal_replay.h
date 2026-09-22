#ifndef FAULTLINE_WAL_REPLAY_H
#define FAULTLINE_WAL_REPLAY_H

#include "scheduler.h"
#include "wal_writer.h"
#include "worker_registry.h"

struct faultline_wal_replay_state {
    struct faultline_scheduler scheduler;
    struct faultline_worker_registry workers; /* Empty live registry; restored allocator. */
    uint64_t last_sequence;
    uint64_t highest_job_id;
    uint32_t highest_worker_id;
    int64_t job_time_base_ms; /* Greatest recovered job time; downtime is not included. */
};

enum faultline_wal_replay_result {
    FAULTLINE_WAL_REPLAY_OK = 0,
    FAULTLINE_WAL_REPLAY_INVALID_ARGUMENT,
    FAULTLINE_WAL_REPLAY_INVALID_STATE,
    FAULTLINE_WAL_REPLAY_IO_ERROR,
    FAULTLINE_WAL_REPLAY_BAD_FORMAT,
    FAULTLINE_WAL_REPLAY_BAD_HISTORY,
    FAULTLINE_WAL_REPLAY_CAPACITY,
    FAULTLINE_WAL_REPLAY_NO_MEMORY
};

enum faultline_wal_history_error {
    FAULTLINE_WAL_HISTORY_OK = 0,
    FAULTLINE_WAL_HISTORY_ALLOCATION,
    FAULTLINE_WAL_HISTORY_UNKNOWN_JOB,
    FAULTLINE_WAL_HISTORY_UNKNOWN_WORKER,
    FAULTLINE_WAL_HISTORY_BUSY_WORKER,
    FAULTLINE_WAL_HISTORY_FIFO,
    FAULTLINE_WAL_HISTORY_TIME,
    FAULTLINE_WAL_HISTORY_TRANSITION,
    FAULTLINE_WAL_HISTORY_SNAPSHOT
};

struct faultline_wal_replay_report {
    uint64_t valid_bytes;     /* End of the fully validated prefix (including file header). */
    uint64_t tail_bytes;      /* Incomplete bytes found at actual EOF, if any. */
    uint64_t error_offset;    /* Start of the failing header/record, or valid_bytes at finalization. */
    uint64_t last_sequence;   /* Last accepted record; zero for no records. */
    enum faultline_wal_result format_error;
    enum faultline_wal_history_error history_error;
};

/*
 * Recover an EXISTING regular WAL under its exclusive lock. Never creates a file.
 * Validate all complete records before truncating only an incomplete final record;
 * sync the surviving file and parent directory before publishing state and making
 * the writer READY. Retain the same locked descriptor for subsequent appends.
 *
 * Initialize writer with FAULTLINE_WAL_WRITER_INIT. All pointers are required,
 * valid, and non-overlapping. state is large; heap allocation is recommended.
 * On any failure state is unchanged. With valid arguments, report gives progress
 * and error details. Once opening begins, failures disable writer: close it even
 * after failure. A truncate/sync error may already have changed the incomplete tail.
 *
 * ASSIGNED/RUNNING snapshots are preserved, with no restored live connections.
 * READY here permits storage appends (including future reconciliation), not live
 * coordinator scheduling. Startup must reconcile active attempts before serving.
 */
enum faultline_wal_replay_result faultline_wal_replay_open(
    struct faultline_wal_writer *writer, const char *path,
    struct faultline_wal_replay_state *state, struct faultline_wal_replay_report *report);

#endif
