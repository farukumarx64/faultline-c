#ifndef FAULTLINE_COORDINATOR_STORE_INTERNAL_H
#define FAULTLINE_COORDINATOR_STORE_INTERNAL_H

#include "coordinator_store.h"
#include "wal_writer_internal.h"

/* Test seam only; production uses real I/O, with no environment fault switches. */
enum faultline_store_result faultline_store_open_with_io(
    struct faultline_coordinator_store *store, const char *path, int initialize, int64_t now_ms,
    const struct faultline_wal_writer_io *io, void *context);

#endif
