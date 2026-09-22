#ifndef FAULTLINE_WAL_WRITER_INTERNAL_H
#define FAULTLINE_WAL_WRITER_INTERNAL_H

#include "wal_writer.h"
#include <sys/types.h>

/* Internal dependency seam for deterministic fault tests, not a runtime option.
 * Functions follow syscall return/errno semantics. Storage must outlive the
 * writer, including close. Separate writers may use separate test contexts. */
struct faultline_wal_writer_io {
    int (*lock)(void *context, int fd);
    ssize_t (*write)(void *context, int fd, const void *data, size_t size);
    int (*sync)(void *context, int fd);
    int (*close)(void *context, int fd);
};

enum faultline_wal_writer_result faultline_wal_writer_create_with_io(
    struct faultline_wal_writer *writer, const char *path,
    const struct faultline_wal_writer_io *io, void *context);

#endif
