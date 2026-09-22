#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "wal_writer.h"
#include "../src/coordinator/wal_writer_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: check failed: %s (errno=%d)\n", __FILE__, __LINE__, #condition, errno); \
    return EXIT_FAILURE; } } while (0)

struct files { char directory[64]; char path[96]; };

static int setup(struct files *files)
{
    (void)snprintf(files->directory, sizeof(files->directory), "/tmp/faultline-writer-XXXXXX");
    CHECK(mkdtemp(files->directory) != NULL);
    CHECK(snprintf(files->path, sizeof(files->path), "%s/state.wal", files->directory) > 0);
    return EXIT_SUCCESS;
}

static int cleanup(const struct files *files)
{
    CHECK(unlink(files->path) == 0 || errno == ENOENT);
    CHECK(rmdir(files->directory) == 0);
    return EXIT_SUCCESS;
}

static int read_file(const char *path, uint8_t *bytes, size_t capacity, size_t *size)
{
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    size_t used = 0;
    while (used < capacity) {
        ssize_t count = read(fd, bytes + used, capacity - used);
        if (count < 0 && errno == EINTR) { continue; }
        CHECK(count >= 0);
        if (count == 0) { break; }
        used += (size_t)count;
    }
    uint8_t extra;
    CHECK(read(fd, &extra, 1) == 0);
    CHECK(close(fd) == 0);
    *size = used;
    return EXIT_SUCCESS;
}

static struct faultline_wal_record allocation(uint64_t sequence)
{
    struct faultline_wal_record record = {
        .type = FAULTLINE_WAL_WORKER_ID_ALLOCATED, .sequence = sequence,
        .payload.worker_id = (uint32_t)sequence
    };
    return record;
}

/* Inject syscall outcomes while successful operations still touch real files.
 * No disk is filled, unmounted, or modified outside each test's private directory. */
struct step { ssize_t value; int error; };
struct injection {
    struct faultline_wal_writer *writer;
    struct step writes[8];
    size_t step_count, step_index, max_write;
    unsigned write_calls, sync_calls, close_calls, lock_calls;
    unsigned lock_interrupts, sync_interrupts, fail_sync_call, fail_close_call;
    int lock_error, sync_error, close_error;
    int canary, reuse_closed_fd;
    int observe, bad_observation;
    uint64_t expected_next, expected_synced;
    off_t expected_file_size;
    char trace[8192];
    size_t trace_size;
};

static void trace(struct injection *injection, char event)
{
    if (injection->trace_size + 1 < sizeof(injection->trace)) {
        injection->trace[injection->trace_size++] = event;
        injection->trace[injection->trace_size] = '\0';
    } else { injection->bad_observation = 1; }
    if (injection->observe &&
        (injection->writer->next_sequence != injection->expected_next ||
         injection->writer->synced_sequence != injection->expected_synced)) {
        injection->bad_observation = 1;
    }
}

static int injected_lock(void *context, int fd)
{
    struct injection *injection = context;
    ++injection->lock_calls;
    trace(injection, 'L');
    if (injection->lock_interrupts != 0) {
        --injection->lock_interrupts;
        errno = EINTR;
        return -1;
    }
    if (injection->lock_error != 0) { errno = injection->lock_error; return -1; }
    return flock(fd, LOCK_EX | LOCK_NB);
}

static ssize_t injected_write(void *context, int fd, const void *data, size_t size)
{
    struct injection *injection = context;
    ++injection->write_calls;
    trace(injection, 'W');
    if (injection->step_index < injection->step_count) {
        struct step step = injection->writes[injection->step_index++];
        if (step.value <= 0) { errno = step.error; return step.value; }
        if ((size_t)step.value < size) { size = (size_t)step.value; }
    }
    if (injection->max_write != 0 && size > injection->max_write) { size = injection->max_write; }
    return write(fd, data, size);
}

static int injected_sync(void *context, int fd)
{
    struct injection *injection = context;
    ++injection->sync_calls;
    trace(injection, fd == injection->writer->directory_fd ? 'D' : 'S');
    if (fd == injection->writer->fd && injection->expected_file_size != 0) {
        struct stat info;
        if (fstat(fd, &info) < 0 || info.st_size != injection->expected_file_size) {
            injection->bad_observation = 1;
        }
    }
    if (injection->sync_interrupts != 0) {
        --injection->sync_interrupts;
        errno = EINTR;
        return -1;
    }
    if (injection->fail_sync_call == injection->sync_calls) {
        errno = injection->sync_error;
        return -1;
    }
    return fsync(fd);
}

static int injected_close(void *context, int fd)
{
    struct injection *injection = context;
    ++injection->close_calls;
    trace(injection, 'C');
    int result = close(fd);
    if (injection->close_calls == injection->fail_close_call) {
        if (injection->reuse_closed_fd) {
            injection->canary = open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (injection->canary != fd) { injection->bad_observation = 1; }
        }
        errno = injection->close_error;
        return -1;
    }
    return result;
}

static const struct faultline_wal_writer_io injected_io = {
    .lock = injected_lock, .write = injected_write, .sync = injected_sync, .close = injected_close
};

static int append_and_compare(struct faultline_wal_writer *writer, struct faultline_wal_record *record,
                              uint8_t *expected, size_t *used)
{
    size_t size = 0;
    record->sequence = writer->next_sequence;
    CHECK(faultline_wal_record_encode(expected + *used, FAULTLINE_WAL_MAX_RECORD_SIZE,
                                     record, &size) == FAULTLINE_WAL_OK);
    struct injection *injection = writer->io_context;
    injection->expected_file_size = (off_t)(*used + size);
    injection->expected_next = writer->next_sequence;
    injection->expected_synced = writer->synced_sequence;
    injection->observe = 1;
    CHECK(faultline_wal_writer_append(writer, record) == FAULTLINE_WAL_WRITE_OK);
    CHECK(!injection->bad_observation);
    injection->observe = 0;
    CHECK(writer->synced_sequence == record->sequence && writer->next_sequence == record->sequence + 1);
    *used += size;
    return EXIT_SUCCESS;
}

static int test_real_records(void)
{
    struct files files;
    struct faultline_wal_writer writer = FAULTLINE_WAL_WRITER_INIT;
    struct injection injection = {.writer = &writer, .observe = 1, .expected_file_size = 24};
    uint8_t expected[24 + 11 * FAULTLINE_WAL_MAX_RECORD_SIZE], actual[sizeof(expected)];
    size_t used = 24, size = 0;
    CHECK(setup(&files) == EXIT_SUCCESS);
    CHECK(faultline_wal_writer_create_with_io(&writer, files.path, &injected_io, &injection) == FAULTLINE_WAL_WRITE_OK);
    CHECK(strcmp(injection.trace, "LWSD") == 0 && !injection.bad_observation);
    injection.observe = 0;
    injection.max_write = 17;
    CHECK(writer.state == FAULTLINE_WAL_WRITER_READY && writer.next_sequence == 1 && writer.synced_sequence == 0);
    CHECK(faultline_wal_file_header_encode(expected, sizeof(expected)) == FAULTLINE_WAL_OK);
    struct faultline_wal_record record = allocation(1);
    CHECK(append_and_compare(&writer, &record, expected, &used) == EXIT_SUCCESS);
    uint8_t arguments[1024], result[1024];
    for (size_t i = 0; i < 1024; ++i) { arguments[i] = (uint8_t)i; result[i] = (uint8_t)(255 - i % 256); }
    record.type = FAULTLINE_WAL_JOB_CREATED;
    CHECK(faultline_job_init(&record.payload.job, 1, FAULTLINE_TASK_HASH, arguments, 1024, 1, 0) == FAULTLINE_JOB_OK);
    CHECK(append_and_compare(&writer, &record, expected, &used) == EXIT_SUCCESS);
    for (uint64_t attempt = 1; attempt <= 2; ++attempt) {
        CHECK(faultline_job_assign(&record.payload.job, 1, 1) == FAULTLINE_JOB_OK);
        record.type = FAULTLINE_WAL_JOB_ASSIGNED;
        CHECK(append_and_compare(&writer, &record, expected, &used) == EXIT_SUCCESS);
        CHECK(faultline_job_start(&record.payload.job, 1, attempt, 1) == FAULTLINE_JOB_OK);
        record.type = FAULTLINE_WAL_JOB_STARTED;
        CHECK(append_and_compare(&writer, &record, expected, &used) == EXIT_SUCCESS);
        if (attempt == 1) {
            CHECK(faultline_job_fail(&record.payload.job, 1, attempt, FAULTLINE_JOB_FAILURE_TASK, 1) == FAULTLINE_JOB_OK);
            record.type = FAULTLINE_WAL_JOB_REQUEUED;
        } else {
            CHECK(faultline_job_complete(&record.payload.job, 1, attempt, result, 1024, 1) == FAULTLINE_JOB_OK);
            record.type = FAULTLINE_WAL_JOB_COMPLETED;
        }
        CHECK(append_and_compare(&writer, &record, expected, &used) == EXIT_SUCCESS);
    }
    CHECK(faultline_job_init(&record.payload.job, 2, FAULTLINE_TASK_SLEEP, NULL, 0, 0, 1) == FAULTLINE_JOB_OK);
    record.type = FAULTLINE_WAL_JOB_CREATED;
    CHECK(append_and_compare(&writer, &record, expected, &used) == EXIT_SUCCESS);
    CHECK(faultline_job_assign(&record.payload.job, 1, 1) == FAULTLINE_JOB_OK);
    record.type = FAULTLINE_WAL_JOB_ASSIGNED;
    CHECK(append_and_compare(&writer, &record, expected, &used) == EXIT_SUCCESS);
    CHECK(faultline_job_fail(&record.payload.job, 1, 1, FAULTLINE_JOB_FAILURE_WORKER_LOST, 1) == FAULTLINE_JOB_OK);
    record.type = FAULTLINE_WAL_JOB_FAILED;
    CHECK(append_and_compare(&writer, &record, expected, &used) == EXIT_SUCCESS);
    CHECK(writer.synced_sequence == 11 && injection.sync_calls == 13);
    CHECK(read_file(files.path, actual, sizeof(actual), &size) == EXIT_SUCCESS);
    CHECK(size == used && memcmp(expected, actual, size) == 0);
    size_t offset = 24;
    for (uint64_t sequence = 1; sequence <= 11; ++sequence) {
        size_t consumed = 0;
        CHECK(faultline_wal_record_decode(actual + offset, size - offset, sequence,
                                          &record, &consumed) == FAULTLINE_WAL_OK);
        offset += consumed;
    }
    CHECK(offset == size);
    unsigned writes = injection.write_calls, syncs = injection.sync_calls;
    CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_OK);
    CHECK(injection.write_calls == writes && injection.sync_calls == syncs && injection.close_calls == 2);
    CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_OK && injection.close_calls == 2);
    CHECK(cleanup(&files) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static int test_paths_and_flags(void)
{
    struct files files;
    struct faultline_wal_writer writer = FAULTLINE_WAL_WRITER_INIT;
    CHECK(setup(&files) == EXIT_SUCCESS);
    /* A relative filename uses the opened current directory as its parent. */
    int cwd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(cwd >= 0 && chdir(files.directory) == 0);
    CHECK(faultline_wal_writer_create(&writer, "state.wal") == FAULTLINE_WAL_WRITE_OK);
    CHECK(fchdir(cwd) == 0 && close(cwd) == 0);
    CHECK((fcntl(writer.fd, F_GETFL) & O_APPEND) != 0);
    CHECK((fcntl(writer.fd, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((fcntl(writer.directory_fd, F_GETFD) & FD_CLOEXEC) != 0);
    struct stat info;
    CHECK(fstat(writer.fd, &info) == 0 && S_ISREG(info.st_mode) && (info.st_mode & 0077) == 0);
    struct faultline_wal_record record = allocation(1);
    CHECK(faultline_wal_writer_append(&writer, &record) == FAULTLINE_WAL_WRITE_OK);
    CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_OK);
    uint8_t before[60], after[60];
    size_t size;
    CHECK(read_file(files.path, before, sizeof(before), &size) == EXIT_SUCCESS && size == 60);
    CHECK(faultline_wal_writer_create(&writer, files.path) == FAULTLINE_WAL_WRITE_IO_ERROR);
    CHECK(writer.system_error == EEXIST && writer.failed_operation == FAULTLINE_WAL_IO_CREATE_FILE);
    CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_IO_ERROR);
    CHECK(read_file(files.path, after, sizeof(after), &size) == EXIT_SUCCESS && memcmp(before, after, 60) == 0);
    CHECK(unlink(files.path) == 0);
    /* Neither a dangling symlink, a directory, nor a FIFO may be initialized. */
    for (int kind = 0; kind < 3; ++kind) {
        if (kind == 0) { CHECK(symlink("missing-target", files.path) == 0); }
        if (kind == 1) { CHECK(mkdir(files.path, 0700) == 0); }
        if (kind == 2) { CHECK(mkfifo(files.path, 0600) == 0); }
        struct faultline_wal_writer other = FAULTLINE_WAL_WRITER_INIT;
        CHECK(faultline_wal_writer_create(&other, files.path) == FAULTLINE_WAL_WRITE_IO_ERROR);
        CHECK(other.system_error == EEXIST && other.failed_operation == FAULTLINE_WAL_IO_CREATE_FILE);
        CHECK(faultline_wal_writer_close(&other) == FAULTLINE_WAL_WRITE_IO_ERROR);
        if (kind == 1) { CHECK(rmdir(files.path) == 0); }
        else { CHECK(unlink(files.path) == 0); }
    }
    char absent[160];
    (void)snprintf(absent, sizeof(absent), "%s/missing/state.wal", files.directory);
    struct faultline_wal_writer missing = FAULTLINE_WAL_WRITER_INIT;
    CHECK(faultline_wal_writer_create(&missing, absent) == FAULTLINE_WAL_WRITE_IO_ERROR);
    CHECK(missing.system_error == ENOENT && missing.failed_operation == FAULTLINE_WAL_IO_OPEN_DIRECTORY);
    CHECK(faultline_wal_writer_close(&missing) == FAULTLINE_WAL_WRITE_IO_ERROR);
    CHECK(cleanup(&files) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static int wait_child(pid_t child, int *status)
{
    pid_t result;
    do { result = waitpid(child, status, 0); } while (result < 0 && errno == EINTR);
    return result == child ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int test_exclusive_lock(void)
{
    struct files files;
    struct faultline_wal_writer writer = FAULTLINE_WAL_WRITER_INIT;
    CHECK(setup(&files) == EXIT_SUCCESS);
    CHECK(faultline_wal_writer_create(&writer, files.path) == FAULTLINE_WAL_WRITE_OK);
    int other = open(files.path, O_RDWR | O_CLOEXEC);
    CHECK(other >= 0);
    CHECK(flock(other, LOCK_EX | LOCK_NB) < 0 && (errno == EWOULDBLOCK || errno == EAGAIN));
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        (void)close(writer.fd);
        (void)close(writer.directory_fd);
        (void)close(other);
        int fd = open(files.path, O_RDWR);
        int denied = fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) < 0 &&
                     (errno == EWOULDBLOCK || errno == EAGAIN);
        if (fd >= 0) { (void)close(fd); }
        _exit(denied ? EXIT_SUCCESS : EXIT_FAILURE);
    }
    int status;
    CHECK(wait_child(child, &status) == EXIT_SUCCESS && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_OK);
    CHECK(flock(other, LOCK_EX | LOCK_NB) == 0 && close(other) == 0);
    CHECK(cleanup(&files) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static int test_partial_and_interrupted_io(void)
{
    /* Every split of a 36-byte allocation record, with EINTR at the split. */
    for (size_t split = 1; split < 36; ++split) {
        struct files files;
        struct faultline_wal_writer writer = FAULTLINE_WAL_WRITER_INIT;
        struct injection injection = {.writer = &writer, .max_write = 1, .lock_interrupts = 2,
                                       .sync_interrupts = 2};
        CHECK(setup(&files) == EXIT_SUCCESS);
        CHECK(faultline_wal_writer_create_with_io(&writer, files.path, &injected_io, &injection) == FAULTLINE_WAL_WRITE_OK);
        CHECK(injection.write_calls == 24 && injection.lock_calls == 3 && injection.sync_calls == 4);
        injection.step_count = 3;
        injection.writes[0] = (struct step){(ssize_t)split, 0};
        injection.writes[1] = injection.writes[2] = (struct step){-1, EINTR};
        injection.max_write = 0;
        injection.sync_interrupts = 2;
        injection.observe = 1;
        injection.expected_next = 1;
        injection.expected_synced = 0;
        struct faultline_wal_record record = allocation(1), decoded;
        CHECK(faultline_wal_writer_append(&writer, &record) == FAULTLINE_WAL_WRITE_OK);
        CHECK(!injection.bad_observation && injection.step_index == 3 && injection.sync_calls == 7);
        injection.observe = 0;
        CHECK(writer.synced_sequence == 1 && writer.next_sequence == 2);
        uint8_t bytes[60];
        size_t size, consumed = 0;
        CHECK(read_file(files.path, bytes, sizeof(bytes), &size) == EXIT_SUCCESS && size == 60);
        CHECK(faultline_wal_file_header_decode(bytes, size) == FAULTLINE_WAL_OK);
        CHECK(faultline_wal_record_decode(bytes + 24, size - 24, 1, &decoded, &consumed) == FAULTLINE_WAL_OK);
        CHECK(consumed == 36 && decoded.payload.worker_id == 1);
        CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_OK);
        CHECK(cleanup(&files) == EXIT_SUCCESS);
    }
    return EXIT_SUCCESS;
}

static int check_failed(struct faultline_wal_writer *writer, struct injection *injection,
                        enum faultline_wal_writer_operation operation, int error)
{
    CHECK(writer->state == FAULTLINE_WAL_WRITER_FAILED && writer->failed_operation == operation);
    CHECK(writer->system_error == error);
    unsigned writes = injection->write_calls, syncs = injection->sync_calls, locks = injection->lock_calls;
    struct faultline_wal_record record = allocation(writer->next_sequence);
    CHECK(faultline_wal_writer_append(writer, &record) == FAULTLINE_WAL_WRITE_IO_ERROR);
    CHECK(faultline_wal_writer_close(writer) == FAULTLINE_WAL_WRITE_IO_ERROR);
    CHECK(writer->fd == -1 && writer->directory_fd == -1);
    CHECK(faultline_wal_writer_create(writer, "must-not-create.wal") == FAULTLINE_WAL_WRITE_IO_ERROR);
    unsigned closes = injection->close_calls;
    CHECK(faultline_wal_writer_close(writer) == FAULTLINE_WAL_WRITE_IO_ERROR && injection->close_calls == closes);
    CHECK(writer->failed_operation == operation && writer->system_error == error);
    CHECK(injection->write_calls == writes && injection->sync_calls == syncs && injection->lock_calls == locks);
    return EXIT_SUCCESS;
}

static int test_write_failures(void)
{
    const size_t prefixes[] = {0, 1, 31, 32, 35};
    const int errors[] = {ENOSPC, EIO, EDQUOT, EFBIG, 0};
    for (size_t p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); ++p) {
        for (size_t e = 0; e < sizeof(errors) / sizeof(errors[0]); ++e) {
            struct files files;
            struct faultline_wal_writer writer = FAULTLINE_WAL_WRITER_INIT;
            struct injection injection = {.writer = &writer};
            CHECK(setup(&files) == EXIT_SUCCESS);
            CHECK(faultline_wal_writer_create_with_io(&writer, files.path, &injected_io, &injection) == FAULTLINE_WAL_WRITE_OK);
            struct faultline_wal_record record = allocation(1);
            CHECK(faultline_wal_writer_append(&writer, &record) == FAULTLINE_WAL_WRITE_OK);
            record = allocation(2);
            if (prefixes[p] != 0) { injection.writes[injection.step_count++] = (struct step){(ssize_t)prefixes[p], 0}; }
            injection.writes[injection.step_count++] = (struct step){errors[e] == 0 ? 0 : -1, errors[e]};
            CHECK(faultline_wal_writer_append(&writer, &record) == FAULTLINE_WAL_WRITE_IO_ERROR);
            CHECK(writer.synced_sequence == 1 && writer.next_sequence == 2 && injection.sync_calls == 3);
            int other = open(files.path, O_RDWR);
            CHECK(other >= 0 && flock(other, LOCK_EX | LOCK_NB) < 0 && (errno == EWOULDBLOCK || errno == EAGAIN));
            CHECK(close(other) == 0);
            CHECK(check_failed(&writer, &injection, FAULTLINE_WAL_IO_WRITE_RECORD,
                                errors[e] == 0 ? EIO : errors[e]) == EXIT_SUCCESS);
            uint8_t actual[96], expected[96];
            size_t size, written;
            CHECK(read_file(files.path, actual, sizeof(actual), &size) == EXIT_SUCCESS && size == 60 + prefixes[p]);
            CHECK(faultline_wal_file_header_encode(expected, sizeof(expected)) == FAULTLINE_WAL_OK);
            record = allocation(1);
            CHECK(faultline_wal_record_encode(expected + 24, 72, &record, &written) == FAULTLINE_WAL_OK);
            record = allocation(2);
            CHECK(faultline_wal_record_encode(expected + 60, 36, &record, &written) == FAULTLINE_WAL_OK);
            CHECK(memcmp(actual, expected, size) == 0);
            CHECK(cleanup(&files) == EXIT_SUCCESS);
        }
    }
    return EXIT_SUCCESS;
}

static int test_sync_failure(void)
{
    struct files files;
    struct faultline_wal_writer writer = FAULTLINE_WAL_WRITER_INIT;
    struct injection injection = {.writer = &writer, .fail_sync_call = 4, .sync_error = EIO};
    CHECK(setup(&files) == EXIT_SUCCESS);
    CHECK(faultline_wal_writer_create_with_io(&writer, files.path, &injected_io, &injection) == FAULTLINE_WAL_WRITE_OK);
    struct faultline_wal_record record = allocation(1), decoded;
    CHECK(faultline_wal_writer_append(&writer, &record) == FAULTLINE_WAL_WRITE_OK);
    record = allocation(2);
    CHECK(faultline_wal_writer_append(&writer, &record) == FAULTLINE_WAL_WRITE_IO_ERROR);
    CHECK(writer.synced_sequence == 1 && writer.next_sequence == 2);
    CHECK(check_failed(&writer, &injection, FAULTLINE_WAL_IO_SYNC_RECORD, EIO) == EXIT_SUCCESS);
    uint8_t bytes[96];
    size_t size, consumed;
    CHECK(read_file(files.path, bytes, sizeof(bytes), &size) == EXIT_SUCCESS && size == 96);
    /* A failed fsync does NOT imply that the complete record is absent. */
    CHECK(faultline_wal_record_decode(bytes + 60, 36, 2, &decoded, &consumed) == FAULTLINE_WAL_OK);
    CHECK(decoded.payload.worker_id == 2 && consumed == 36);
    CHECK(cleanup(&files) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static int test_initialization_failures(void)
{
    const struct {int kind; size_t prefix; enum faultline_wal_writer_operation operation; int error;} cases[] = {
        {0, 0, FAULTLINE_WAL_IO_LOCK, EWOULDBLOCK},
        {1, 0, FAULTLINE_WAL_IO_WRITE_HEADER, ENOSPC},
        {1, 1, FAULTLINE_WAL_IO_WRITE_HEADER, ENOSPC},
        {1, 8, FAULTLINE_WAL_IO_WRITE_HEADER, ENOSPC},
        {1, 23, FAULTLINE_WAL_IO_WRITE_HEADER, ENOSPC},
        {2, 24, FAULTLINE_WAL_IO_SYNC_HEADER, EIO},
        {3, 24, FAULTLINE_WAL_IO_SYNC_DIRECTORY, EIO}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct files files;
        struct faultline_wal_writer writer = FAULTLINE_WAL_WRITER_INIT;
        struct injection injection = {.writer = &writer};
        CHECK(setup(&files) == EXIT_SUCCESS);
        if (cases[i].kind == 0) { injection.lock_error = cases[i].error; }
        if (cases[i].kind == 1) {
            if (cases[i].prefix != 0) { injection.writes[injection.step_count++] = (struct step){(ssize_t)cases[i].prefix, 0}; }
            injection.writes[injection.step_count++] = (struct step){-1, cases[i].error};
        }
        if (cases[i].kind >= 2) { injection.fail_sync_call = (unsigned)(cases[i].kind - 1); injection.sync_error = EIO; }
        CHECK(faultline_wal_writer_create_with_io(&writer, files.path, &injected_io, &injection) == FAULTLINE_WAL_WRITE_IO_ERROR);
        CHECK(writer.next_sequence == 0 && writer.synced_sequence == 0);
        CHECK(injection.sync_calls == (cases[i].kind >= 2 ? (unsigned)(cases[i].kind - 1) : 0));
        CHECK(check_failed(&writer, &injection, cases[i].operation, cases[i].error) == EXIT_SUCCESS);
        uint8_t actual[24], expected[24];
        size_t size;
        CHECK(read_file(files.path, actual, sizeof(actual), &size) == EXIT_SUCCESS && size == cases[i].prefix);
        CHECK(faultline_wal_file_header_encode(expected, sizeof(expected)) == FAULTLINE_WAL_OK);
        CHECK(memcmp(actual, expected, size) == 0);
        CHECK(cleanup(&files) == EXIT_SUCCESS);
    }
    return EXIT_SUCCESS;
}

static int test_invalid_inputs(void)
{
    struct files files;
    struct faultline_wal_writer writer = FAULTLINE_WAL_WRITER_INIT;
    struct injection injection = {.writer = &writer};
    struct faultline_wal_record record = allocation(1);
    CHECK(setup(&files) == EXIT_SUCCESS);
    CHECK(faultline_wal_writer_create(NULL, files.path) == FAULTLINE_WAL_WRITE_INVALID_ARGUMENT);
    CHECK(faultline_wal_writer_create(&writer, NULL) == FAULTLINE_WAL_WRITE_INVALID_ARGUMENT);
    CHECK(faultline_wal_writer_create(&writer, "") == FAULTLINE_WAL_WRITE_INVALID_ARGUMENT);
    CHECK(faultline_wal_writer_create(&writer, "/tmp/") == FAULTLINE_WAL_WRITE_INVALID_ARGUMENT);
    CHECK(faultline_wal_writer_append(NULL, &record) == FAULTLINE_WAL_WRITE_INVALID_ARGUMENT);
    CHECK(faultline_wal_writer_append(&writer, NULL) == FAULTLINE_WAL_WRITE_INVALID_ARGUMENT);
    CHECK(faultline_wal_writer_append(&writer, &record) == FAULTLINE_WAL_WRITE_INVALID_STATE);
    CHECK(faultline_wal_writer_close(NULL) == FAULTLINE_WAL_WRITE_INVALID_ARGUMENT);
    CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_OK);
    CHECK(faultline_wal_writer_create_with_io(&writer, files.path, NULL, NULL) == FAULTLINE_WAL_WRITE_INVALID_ARGUMENT);
    struct faultline_wal_writer_io bad_io = injected_io;
    bad_io.sync = NULL;
    CHECK(faultline_wal_writer_create_with_io(&writer, files.path, &bad_io, &injection) == FAULTLINE_WAL_WRITE_INVALID_ARGUMENT);
    CHECK(access(files.path, F_OK) < 0 && errno == ENOENT);
    CHECK(faultline_wal_writer_create_with_io(&writer, files.path, &injected_io, &injection) == FAULTLINE_WAL_WRITE_OK);
    CHECK(faultline_wal_writer_create(&writer, files.path) == FAULTLINE_WAL_WRITE_INVALID_STATE);
    for (int kind = 0; kind < 4; ++kind) {
        struct faultline_wal_record invalid = record;
        if (kind == 0) { invalid.sequence = 0; }
        if (kind == 1) { invalid.sequence = 2; }
        if (kind == 2) { invalid.type = (enum faultline_wal_record_type)99; }
        if (kind == 3) { invalid.payload.worker_id = 0; }
        CHECK(faultline_wal_writer_append(&writer, &invalid) == FAULTLINE_WAL_WRITE_INVALID_RECORD);
    }
    CHECK(writer.state == FAULTLINE_WAL_WRITER_READY && writer.next_sequence == 1 && writer.synced_sequence == 0);
    CHECK(injection.write_calls == 1 && injection.sync_calls == 2 && injection.lock_calls == 1);
    CHECK(faultline_wal_writer_append(&writer, &record) == FAULTLINE_WAL_WRITE_OK);
    CHECK(faultline_wal_writer_append(&writer, &record) == FAULTLINE_WAL_WRITE_INVALID_RECORD);
    CHECK(injection.write_calls == 2 && injection.sync_calls == 3);
    CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_OK);
    CHECK(faultline_wal_writer_append(&writer, &record) == FAULTLINE_WAL_WRITE_INVALID_STATE);
    CHECK(cleanup(&files) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static int test_sequence_exhaustion(void)
{
    struct files files;
    struct faultline_wal_writer writer = FAULTLINE_WAL_WRITER_INIT;
    struct injection injection = {.writer = &writer};
    CHECK(setup(&files) == EXIT_SUCCESS);
    CHECK(faultline_wal_writer_create_with_io(&writer, files.path, &injected_io, &injection) == FAULTLINE_WAL_WRITE_OK);
    /* Seed internal counters for a boundary test, not a valid replay history. */
    writer.next_sequence = UINT64_MAX;
    writer.synced_sequence = UINT64_MAX - 1;
    struct faultline_wal_record record = allocation(UINT64_MAX);
    CHECK(faultline_wal_writer_append(&writer, &record) == FAULTLINE_WAL_WRITE_OK);
    CHECK(writer.next_sequence == 0 && writer.synced_sequence == UINT64_MAX);
    CHECK(faultline_wal_writer_append(&writer, &record) == FAULTLINE_WAL_WRITE_SEQUENCE_EXHAUSTED);
    CHECK(injection.write_calls == 2 && injection.sync_calls == 3);
    CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_OK);
    CHECK(cleanup(&files) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

static int test_close_errors(void)
{
    for (int kind = 0; kind < 3; ++kind) {
        struct files files;
        struct faultline_wal_writer writer = FAULTLINE_WAL_WRITER_INIT;
        struct injection injection = {.writer = &writer, .fail_close_call = kind == 1 ? 2u : 1u,
                                       .close_error = EINTR, .reuse_closed_fd = 1, .canary = -1};
        CHECK(setup(&files) == EXIT_SUCCESS);
        CHECK(faultline_wal_writer_create_with_io(&writer, files.path, &injected_io, &injection) == FAULTLINE_WAL_WRITE_OK);
        struct faultline_wal_record record = allocation(1);
        if (kind == 2) {
            injection.writes[0] = (struct step){-1, ENOSPC};
            injection.step_count = 1;
            CHECK(faultline_wal_writer_append(&writer, &record) == FAULTLINE_WAL_WRITE_IO_ERROR);
        }
        CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_IO_ERROR);
        CHECK(writer.system_error == (kind == 2 ? ENOSPC : EINTR));
        CHECK(writer.failed_operation == (kind == 0 ? FAULTLINE_WAL_IO_CLOSE_FILE :
              kind == 1 ? FAULTLINE_WAL_IO_CLOSE_DIRECTORY : FAULTLINE_WAL_IO_WRITE_RECORD));
        CHECK(injection.close_calls == 2 && injection.sync_calls == 2 && !injection.bad_observation);
        CHECK(injection.canary >= 0 && fcntl(injection.canary, F_GETFD) >= 0);
        CHECK(faultline_wal_writer_close(&writer) == FAULTLINE_WAL_WRITE_IO_ERROR && injection.close_calls == 2);
        CHECK(fcntl(injection.canary, F_GETFD) >= 0 && close(injection.canary) == 0);
        CHECK(cleanup(&files) == EXIT_SUCCESS);
    }
    return EXIT_SUCCESS;
}

static int test_process_crash(void)
{
    struct files files;
    int ready[2];
    CHECK(setup(&files) == EXIT_SUCCESS && pipe(ready) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        (void)close(ready[0]);
        struct faultline_wal_writer writer = FAULTLINE_WAL_WRITER_INIT;
        struct faultline_wal_record record = allocation(1);
        if (faultline_wal_writer_create(&writer, files.path) != FAULTLINE_WAL_WRITE_OK ||
            faultline_wal_writer_append(&writer, &record) != FAULTLINE_WAL_WRITE_OK ||
            write(ready[1], "Y", 1) != 1) { _exit(EXIT_FAILURE); }
        for (;;) { pause(); }
    }
    CHECK(close(ready[1]) == 0);
    struct pollfd poll_fd = {.fd = ready[0], .events = POLLIN};
    int polled;
    do { polled = poll(&poll_fd, 1, 5000); } while (polled < 0 && errno == EINTR);
    char ack = 0;
    ssize_t received = polled > 0 ? read(ready[0], &ack, 1) : -1;
    int killed = kill(child, SIGKILL), status;
    CHECK(wait_child(child, &status) == EXIT_SUCCESS);
    CHECK(close(ready[0]) == 0);
    CHECK(received == 1 && ack == 'Y' && killed == 0 && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    uint8_t bytes[60];
    size_t size, consumed;
    struct faultline_wal_record record;
    CHECK(read_file(files.path, bytes, sizeof(bytes), &size) == EXIT_SUCCESS && size == 60);
    CHECK(faultline_wal_file_header_decode(bytes, size) == FAULTLINE_WAL_OK);
    CHECK(faultline_wal_record_decode(bytes + 24, 36, 1, &record, &consumed) == FAULTLINE_WAL_OK);
    CHECK(record.payload.worker_id == 1 && consumed == 36);
    int fd = open(files.path, O_RDWR);
    CHECK(fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0 && close(fd) == 0);
    CHECK(cleanup(&files) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}

int main(void)
{
    const struct {const char *name; int (*run)(void);} tests[] = {
        {"real WAL records, sync ordering, exact bytes, and maximum payload", test_real_records},
        {"path safety, exclusive creation, file permissions, and descriptor flags", test_paths_and_flags},
        {"exclusive lock across handles and processes", test_exclusive_lock},
        {"short writes and interrupted writes, locks, and syncs", test_partial_and_interrupted_io},
        {"partial append storage errors, zero progress, and failed-handle rejection", test_write_failures},
        {"failed sync preserves confirmed sequence and uncertain record", test_sync_failure},
        {"initialization failure boundaries and retained file prefixes", test_initialization_failures},
        {"invalid arguments and records cause no storage I/O", test_invalid_inputs},
        {"sequence exhaustion without wraparound", test_sequence_exhaustion},
        {"close errors preserve first failure and never close a reused descriptor", test_close_errors},
        {"synced record survives writer SIGKILL without close", test_process_crash}
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (tests[i].run() != EXIT_SUCCESS) { return EXIT_FAILURE; }
        printf("PASS: %s\n", tests[i].name);
    }
    return EXIT_SUCCESS;
}
