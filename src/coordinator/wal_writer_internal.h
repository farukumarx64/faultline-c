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
    ssize_t (*read)(void *context, int fd, void *data, size_t size);
    int (*truncate)(void *context, int fd, off_t length);
};

enum faultline_wal_writer_result faultline_wal_writer_create_with_io(
    struct faultline_wal_writer *writer, const char *path,
    const struct faultline_wal_writer_io *io, void *context);

/* Shared storage primitives. Opening alone never enables appends. */
const struct faultline_wal_writer_io *faultline_wal_writer_system_io(void);
enum faultline_wal_writer_result faultline_wal_writer_open_file(
    struct faultline_wal_writer *writer, const char *path,
    const struct faultline_wal_writer_io *io, void *context, int create);
enum faultline_wal_writer_result faultline_wal_writer_fail(
    struct faultline_wal_writer *writer, enum faultline_wal_writer_operation operation, int error);
enum faultline_wal_writer_result faultline_wal_writer_sync(
    struct faultline_wal_writer *writer, int fd, enum faultline_wal_writer_operation operation);

#endif
