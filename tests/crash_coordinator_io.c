/* Linked only into tests/crash-coordinator. The production main is compiled
 * separately with its store-open call redirected here; every other operation
 * uses the normal coordinator and storage objects. */
#include "../src/coordinator/coordinator_store_internal.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum faultline_store_result faultline_test_store_open(
    struct faultline_coordinator_store *store, const char *path, int initialize, int64_t now_ms);

static struct faultline_wal_writer_io crash_io;
static const struct faultline_wal_writer_io *real_io;
static const char *point;
static unsigned target_kind, target_occurrence, occurrences;
static int target_record;

static void stop_at_boundary(void)
{
    printf("[TEST] crash_boundary kind=%u point=%s occurrence=%u\n",
           target_kind, point, occurrences);
    (void)fflush(stdout);
    (void)raise(SIGSTOP);
    /* Even an accidental SIGCONT cannot publish the interrupted operation.
     * The owning Python test terminates this process with SIGKILL. */
    for (;;) { (void)pause(); }
}

static ssize_t crash_write(void *context, int fd, const void *data, size_t size)
{
    (void)context;
    const unsigned char *bytes = data;
    /* A fresh encoded record arrives before write_all handles short writes. */
    if (size >= FAULTLINE_WAL_RECORD_HEADER_SIZE && memcmp(bytes, "FLWR", 4) == 0) {
        unsigned kind = ((unsigned)bytes[6] << 8) | bytes[7];
        if (kind == target_kind && ++occurrences == target_occurrence) {
            target_record = 1;
            if (strcmp(point, "before_write") == 0) { stop_at_boundary(); }
            if (strcmp(point, "partial_header") == 0 || strcmp(point, "partial_payload") == 0) {
                size_t count = strcmp(point, "partial_header") == 0 ? 7 : size - 1;
                size_t written = 0;
                while (written < count) {
                    ssize_t n = real_io->write(NULL, fd, bytes + written, count - written);
                    if (n < 0 && errno == EINTR) { continue; }
                    if (n <= 0) { return n; }
                    written += (size_t)n;
                }
                stop_at_boundary();
            }
        }
    }
    return real_io->write(NULL, fd, data, size);
}

static int crash_sync(void *context, int fd)
{
    (void)context;
    if (target_record && strcmp(point, "before_sync") == 0) { stop_at_boundary(); }
    int result = real_io->sync(NULL, fd);
    if (result == 0 && target_record && strcmp(point, "after_sync") == 0) { stop_at_boundary(); }
    return result;
}

enum faultline_store_result faultline_test_store_open(
    struct faultline_coordinator_store *store, const char *path, int initialize, int64_t now_ms)
{
    const char *kind = getenv("FAULTLINE_TEST_WAL_KIND");
    const char *occurrence = getenv("FAULTLINE_TEST_WAL_OCCURRENCE");
    point = getenv("FAULTLINE_TEST_WAL_POINT");
    if (kind == NULL || strlen(kind) != 1 || kind[0] < '1' || kind[0] > '7' ||
        occurrence == NULL || strlen(occurrence) != 1 || occurrence[0] < '1' || occurrence[0] > '9' ||
        point == NULL || (strcmp(point, "before_write") != 0 && strcmp(point, "partial_header") != 0 &&
                          strcmp(point, "partial_payload") != 0 && strcmp(point, "before_sync") != 0 &&
                          strcmp(point, "after_sync") != 0)) {
        fputs("[TEST] invalid crash boundary configuration\n", stderr);
        return FAULTLINE_STORE_FATAL;
    }
    target_kind = (unsigned)(kind[0] - '0');
    target_occurrence = (unsigned)(occurrence[0] - '0');
    real_io = faultline_wal_writer_system_io();
    crash_io = *real_io;
    crash_io.write = crash_write;
    crash_io.sync = crash_sync;
    return faultline_store_open_with_io(store, path, initialize, now_ms, &crash_io, NULL);
}
