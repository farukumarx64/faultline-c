/* Expose BSD flock alongside POSIX interfaces on macOS. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "wal_writer_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

static int system_lock(void *context, int fd)
{
    (void)context;
    return flock(fd, LOCK_EX | LOCK_NB);
}

static ssize_t system_write(void *context, int fd, const void *data, size_t size)
{
    (void)context;
    return write(fd, data, size);
}

static int system_sync(void *context, int fd)
{
    (void)context;
    return fsync(fd);
}

static int system_close(void *context, int fd)
{
    (void)context;
    return close(fd);
}

static const struct faultline_wal_writer_io system_io = {
    .lock = system_lock, .write = system_write, .sync = system_sync, .close = system_close
};

static enum faultline_wal_writer_result fail(
    struct faultline_wal_writer *writer, enum faultline_wal_writer_operation operation, int error)
{
    if (writer->state != FAULTLINE_WAL_WRITER_FAILED) {
        writer->system_error = error != 0 ? error : EIO;
        writer->failed_operation = operation;
        writer->state = FAULTLINE_WAL_WRITER_FAILED;
    }
    return FAULTLINE_WAL_WRITE_IO_ERROR;
}

static enum faultline_wal_writer_result write_all(
    struct faultline_wal_writer *writer, const uint8_t *bytes, size_t size,
    enum faultline_wal_writer_operation operation)
{
    size_t offset = 0;
    while (offset < size) {
        ssize_t written = writer->io->write(writer->io_context, writer->fd,
                                             bytes + offset, size - offset);
        if (written < 0) {
            if (errno == EINTR) { continue; }
            return fail(writer, operation, errno);
        }
        /* Zero progress must not spin forever. An oversized return violates the
         * syscall contract (also guarded here for injected I/O implementations). */
        if (written == 0 || (size_t)written > size - offset) { return fail(writer, operation, EIO); }
        offset += (size_t)written;
    }
    return FAULTLINE_WAL_WRITE_OK;
}

static enum faultline_wal_writer_result sync_fd(
    struct faultline_wal_writer *writer, int fd, enum faultline_wal_writer_operation operation)
{
    while (writer->io->sync(writer->io_context, fd) < 0) {
        if (errno != EINTR) { return fail(writer, operation, errno); }
    }
    return FAULTLINE_WAL_WRITE_OK;
}

enum faultline_wal_writer_result faultline_wal_writer_create_with_io(
    struct faultline_wal_writer *writer, const char *path,
    const struct faultline_wal_writer_io *io, void *context)
{
    if (writer == NULL || path == NULL || path[0] == '\0' || path[strlen(path) - 1] == '/' ||
        io == NULL || io->lock == NULL || io->write == NULL || io->sync == NULL || io->close == NULL) {
        return FAULTLINE_WAL_WRITE_INVALID_ARGUMENT;
    }
    if (writer->state == FAULTLINE_WAL_WRITER_FAILED) { return FAULTLINE_WAL_WRITE_IO_ERROR; }
    if (writer->state != FAULTLINE_WAL_WRITER_CLOSED || writer->fd != -1 || writer->directory_fd != -1) {
        return FAULTLINE_WAL_WRITE_INVALID_STATE;
    }
    writer->io = io;
    writer->io_context = context;
    writer->next_sequence = writer->synced_sequence = 0;
    char *copy = strdup(path);
    if (copy == NULL) { return fail(writer, FAULTLINE_WAL_IO_ALLOCATE_PATH, ENOMEM); }
    char *slash = strrchr(copy, '/');
    const char *directory = ".", *name = copy;
    if (slash != NULL) {
        name = slash + 1;
        if (slash == copy) { directory = "/"; }
        else { *slash = '\0'; directory = copy; }
    }
    do {
        writer->directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    } while (writer->directory_fd < 0 && errno == EINTR);
    if (writer->directory_fd < 0) {
        int error = errno;
        free(copy);
        return fail(writer, FAULTLINE_WAL_IO_OPEN_DIRECTORY, error);
    }
    do {
        writer->fd = openat(writer->directory_fd, name,
                            O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    } while (writer->fd < 0 && errno == EINTR);
    int open_error = errno;
    free(copy);
    if (writer->fd < 0) { return fail(writer, FAULTLINE_WAL_IO_CREATE_FILE, open_error); }
    while (writer->io->lock(writer->io_context, writer->fd) < 0) {
        if (errno != EINTR) { return fail(writer, FAULTLINE_WAL_IO_LOCK, errno); }
    }
    uint8_t header[FAULTLINE_WAL_FILE_HEADER_SIZE];
    (void)faultline_wal_file_header_encode(header, sizeof(header));
    if (write_all(writer, header, sizeof(header), FAULTLINE_WAL_IO_WRITE_HEADER) != FAULTLINE_WAL_WRITE_OK ||
        sync_fd(writer, writer->fd, FAULTLINE_WAL_IO_SYNC_HEADER) != FAULTLINE_WAL_WRITE_OK ||
        sync_fd(writer, writer->directory_fd, FAULTLINE_WAL_IO_SYNC_DIRECTORY) != FAULTLINE_WAL_WRITE_OK) {
        return FAULTLINE_WAL_WRITE_IO_ERROR;
    }
    writer->next_sequence = 1;
    writer->state = FAULTLINE_WAL_WRITER_READY;
    return FAULTLINE_WAL_WRITE_OK;
}

enum faultline_wal_writer_result faultline_wal_writer_create(
    struct faultline_wal_writer *writer, const char *path)
{
    return faultline_wal_writer_create_with_io(writer, path, &system_io, NULL);
}

enum faultline_wal_writer_result faultline_wal_writer_append(
    struct faultline_wal_writer *writer, const struct faultline_wal_record *record)
{
    if (writer == NULL || record == NULL) { return FAULTLINE_WAL_WRITE_INVALID_ARGUMENT; }
    if (writer->state == FAULTLINE_WAL_WRITER_FAILED) { return FAULTLINE_WAL_WRITE_IO_ERROR; }
    if (writer->state != FAULTLINE_WAL_WRITER_READY) { return FAULTLINE_WAL_WRITE_INVALID_STATE; }
    if (writer->next_sequence == 0) { return FAULTLINE_WAL_WRITE_SEQUENCE_EXHAUSTED; }
    if (record->sequence != writer->next_sequence) { return FAULTLINE_WAL_WRITE_INVALID_RECORD; }
    uint8_t bytes[FAULTLINE_WAL_MAX_RECORD_SIZE];
    size_t size = 0;
    if (faultline_wal_record_encode(bytes, sizeof(bytes), record, &size) != FAULTLINE_WAL_OK) {
        return FAULTLINE_WAL_WRITE_INVALID_RECORD;
    }
    if (write_all(writer, bytes, size, FAULTLINE_WAL_IO_WRITE_RECORD) != FAULTLINE_WAL_WRITE_OK ||
        sync_fd(writer, writer->fd, FAULTLINE_WAL_IO_SYNC_RECORD) != FAULTLINE_WAL_WRITE_OK) {
        return FAULTLINE_WAL_WRITE_IO_ERROR;
    }
    writer->synced_sequence = record->sequence;
    writer->next_sequence = record->sequence == UINT64_MAX ? 0 : record->sequence + 1;
    return FAULTLINE_WAL_WRITE_OK;
}

enum faultline_wal_writer_result faultline_wal_writer_close(struct faultline_wal_writer *writer)
{
    if (writer == NULL) { return FAULTLINE_WAL_WRITE_INVALID_ARGUMENT; }
    if (writer->io == NULL && (writer->fd != -1 || writer->directory_fd != -1)) {
        return FAULTLINE_WAL_WRITE_INVALID_STATE;
    }
    /* Do not retry close, including EINTR: on supported macOS/Linux targets a
     * descriptor may already be released and reused. Never close its replacement. */
    int fd = writer->fd;
    writer->fd = -1;
    if (fd != -1 && writer->io->close(writer->io_context, fd) < 0) {
        (void)fail(writer, FAULTLINE_WAL_IO_CLOSE_FILE, errno);
    }
    fd = writer->directory_fd;
    writer->directory_fd = -1;
    if (fd != -1 && writer->io->close(writer->io_context, fd) < 0) {
        (void)fail(writer, FAULTLINE_WAL_IO_CLOSE_DIRECTORY, errno);
    }
    if (writer->state == FAULTLINE_WAL_WRITER_FAILED) { return FAULTLINE_WAL_WRITE_IO_ERROR; }
    writer->state = FAULTLINE_WAL_WRITER_CLOSED;
    return FAULTLINE_WAL_WRITE_OK;
}

const char *faultline_wal_writer_operation_name(enum faultline_wal_writer_operation operation)
{
    switch (operation) {
    case FAULTLINE_WAL_IO_NONE: return "none";
    case FAULTLINE_WAL_IO_ALLOCATE_PATH: return "allocate_path";
    case FAULTLINE_WAL_IO_OPEN_DIRECTORY: return "open_directory";
    case FAULTLINE_WAL_IO_CREATE_FILE: return "create_file";
    case FAULTLINE_WAL_IO_LOCK: return "lock";
    case FAULTLINE_WAL_IO_WRITE_HEADER: return "write_header";
    case FAULTLINE_WAL_IO_SYNC_HEADER: return "sync_header";
    case FAULTLINE_WAL_IO_SYNC_DIRECTORY: return "sync_directory";
    case FAULTLINE_WAL_IO_WRITE_RECORD: return "write_record";
    case FAULTLINE_WAL_IO_SYNC_RECORD: return "sync_record";
    case FAULTLINE_WAL_IO_CLOSE_FILE: return "close_file";
    case FAULTLINE_WAL_IO_CLOSE_DIRECTORY: return "close_directory";
    default: return "unknown";
    }
}
