#ifndef FAULTLINE_WAL_REPLAY_INTERNAL_H
#define FAULTLINE_WAL_REPLAY_INTERNAL_H

#include "wal_replay.h"
#include "wal_writer_internal.h"

/* Internal fault-injection seam, not a runtime mode or an unchecked resume API. */
enum faultline_wal_replay_result faultline_wal_replay_open_with_io(
    struct faultline_wal_writer *writer, const char *path,
    struct faultline_wal_replay_state *state, struct faultline_wal_replay_report *report,
    const struct faultline_wal_writer_io *io, void *context);

#endif
