#include "wal_replay_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(off_t) >= sizeof(int64_t), "WAL replay requires 64-bit file offsets");

/* Allocations increase but may have gaps. A high-water mark alone cannot prove
 * that an old worker ID was actually allocated. Adjacent IDs share one range. */
struct worker_range { uint32_t first, last; };
struct worker_index { struct worker_range *ranges; size_t count, capacity; };

static enum faultline_wal_replay_result history_error(
    struct faultline_wal_replay_report *report, enum faultline_wal_history_error error)
{
    report->history_error = error;
    return FAULTLINE_WAL_REPLAY_BAD_HISTORY;
}

static enum faultline_wal_replay_result format_error(
    struct faultline_wal_replay_report *report, enum faultline_wal_result error)
{
    report->format_error = error;
    return FAULTLINE_WAL_REPLAY_BAD_FORMAT;
}

static enum faultline_wal_replay_result allocate_worker(
    struct faultline_wal_replay_state *state, struct worker_index *index,
    uint32_t id, struct faultline_wal_replay_report *report)
{
    if (id <= state->highest_worker_id) { return history_error(report, FAULTLINE_WAL_HISTORY_ALLOCATION); }
    if (index->count != 0 && (uint64_t)index->ranges[index->count - 1].last + 1 == id) {
        index->ranges[index->count - 1].last = id;
    } else {
        if (index->count == index->capacity) {
            if (index->capacity > SIZE_MAX / 2 / sizeof(*index->ranges)) { return FAULTLINE_WAL_REPLAY_NO_MEMORY; }
            size_t capacity = index->capacity == 0 ? 16 : index->capacity * 2;
            struct worker_range *ranges = realloc(index->ranges, capacity * sizeof(*ranges));
            if (ranges == NULL) { return FAULTLINE_WAL_REPLAY_NO_MEMORY; }
            index->ranges = ranges;
            index->capacity = capacity;
        }
        index->ranges[index->count++] = (struct worker_range){id, id};
    }
    state->highest_worker_id = id;
    state->workers.next_worker_id = id == UINT32_MAX ? 0 : id + 1;
    return FAULTLINE_WAL_REPLAY_OK;
}

static int worker_was_allocated(const struct worker_index *index, uint32_t id)
{
    size_t low = 0, high = index->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        const struct worker_range *range = &index->ranges[middle];
        if (id < range->first) { high = middle; }
        else if (id > range->last) { low = middle + 1; }
        else { return 1; }
    }
    return 0;
}

/* Compare canonical bytes, never C struct padding. Reuse the model to derive
 * the expected post-transition snapshot, then require every persisted field. */
static int matches_snapshot(const struct faultline_job *candidate,
                            const struct faultline_wal_record *record,
                            const uint8_t *wire, size_t size)
{
    struct faultline_wal_record expected = {
        .type = record->type, .sequence = record->sequence, .payload.job = *candidate
    };
    uint8_t bytes[FAULTLINE_WAL_MAX_RECORD_SIZE];
    size_t written = 0;
    return faultline_wal_record_encode(bytes, sizeof(bytes), &expected, &written) == FAULTLINE_WAL_OK &&
           written == size && memcmp(bytes, wire, size) == 0;
}

static enum faultline_wal_replay_result apply_record(
    struct faultline_wal_replay_state *state, struct worker_index *index,
    const struct faultline_wal_record *record, const uint8_t *wire, size_t size,
    struct faultline_wal_replay_report *report)
{
    if (record->type == FAULTLINE_WAL_WORKER_ID_ALLOCATED) {
        return allocate_worker(state, index, record->payload.worker_id, report);
    }
    const struct faultline_job *saved = &record->payload.job;
    struct faultline_scheduler *scheduler = &state->scheduler;
    if (saved->updated_at_ms < state->job_time_base_ms) {
        return history_error(report, FAULTLINE_WAL_HISTORY_TIME);
    }
    size_t slot = 0;
    while (slot < scheduler->count && scheduler->jobs[slot].id != saved->id) { ++slot; }
    struct faultline_job candidate;
    enum faultline_job_result result;
    if (record->type == FAULTLINE_WAL_JOB_CREATED) {
        if (saved->id <= state->highest_job_id) { return history_error(report, FAULTLINE_WAL_HISTORY_ALLOCATION); }
        if (scheduler->count == FAULTLINE_JOB_STORE_CAPACITY) { return FAULTLINE_WAL_REPLAY_CAPACITY; }
        result = faultline_job_init(&candidate, saved->id, saved->task_type, saved->arguments,
                                    saved->argument_size, saved->max_retries, saved->created_at_ms);
    } else {
        if (slot == scheduler->count) { return history_error(report, FAULTLINE_WAL_HISTORY_UNKNOWN_JOB); }
        candidate = scheduler->jobs[slot];
        switch (record->type) {
        case FAULTLINE_WAL_JOB_ASSIGNED: {
            uint64_t head;
            if (!worker_was_allocated(index, saved->worker_id)) {
                return history_error(report, FAULTLINE_WAL_HISTORY_UNKNOWN_WORKER);
            }
            if (faultline_scheduler_active(scheduler, saved->worker_id) != NULL) {
                return history_error(report, FAULTLINE_WAL_HISTORY_BUSY_WORKER);
            }
            if (faultline_job_queue_peek(&scheduler->pending, &head) != FAULTLINE_JOB_QUEUE_OK || head != saved->id) {
                return history_error(report, FAULTLINE_WAL_HISTORY_FIFO);
            }
            result = faultline_job_assign(&candidate, saved->worker_id, saved->updated_at_ms);
            break;
        }
        case FAULTLINE_WAL_JOB_STARTED:
            result = faultline_job_start(&candidate, saved->worker_id, saved->attempt, saved->updated_at_ms);
            break;
        case FAULTLINE_WAL_JOB_COMPLETED:
            result = faultline_job_complete(&candidate, saved->worker_id, saved->attempt,
                                            saved->result, saved->result_size, saved->updated_at_ms);
            break;
        case FAULTLINE_WAL_JOB_REQUEUED:
        case FAULTLINE_WAL_JOB_FAILED:
            result = faultline_job_fail(&candidate, candidate.worker_id, saved->attempt,
                                        saved->failure, saved->updated_at_ms);
            break;
        default:
            return history_error(report, FAULTLINE_WAL_HISTORY_TRANSITION);
        }
    }
    if (result != FAULTLINE_JOB_OK) { return history_error(report, FAULTLINE_WAL_HISTORY_TRANSITION); }
    if (!matches_snapshot(&candidate, record, wire, size)) {
        return history_error(report, FAULTLINE_WAL_HISTORY_SNAPSHOT);
    }
    if (record->type == FAULTLINE_WAL_JOB_ASSIGNED) {
        uint64_t removed;
        if (faultline_job_queue_pop(&scheduler->pending, &removed) != FAULTLINE_JOB_QUEUE_OK || removed != saved->id) {
            return history_error(report, FAULTLINE_WAL_HISTORY_FIFO);
        }
    } else if (candidate.state == FAULTLINE_JOB_QUEUED &&
               faultline_job_queue_push(&scheduler->pending, &candidate) != FAULTLINE_JOB_QUEUE_OK) {
        return history_error(report, FAULTLINE_WAL_HISTORY_FIFO);
    }
    scheduler->jobs[slot] = candidate;
    if (record->type == FAULTLINE_WAL_JOB_CREATED) {
        ++scheduler->count;
        state->highest_job_id = saved->id;
        scheduler->next_job_id = saved->id == UINT64_MAX ? 0 : saved->id + 1;
    }
    state->job_time_base_ms = saved->updated_at_ms;
    return FAULTLINE_WAL_REPLAY_OK;
}

/* Short reads are progress, not EOF. Only a zero return establishes EOF. */
static int read_exact(struct faultline_wal_writer *writer, uint8_t *bytes, size_t size, size_t *received)
{
    *received = 0;
    while (*received < size) {
        ssize_t count = writer->io->read(writer->io_context, writer->fd, bytes + *received, size - *received);
        if (count < 0) {
            if (errno == EINTR) { continue; }
            (void)faultline_wal_writer_fail(writer, FAULTLINE_WAL_IO_READ_FILE, errno);
            return -1;
        }
        if (count == 0) { break; }
        if ((size_t)count > size - *received) {
            (void)faultline_wal_writer_fail(writer, FAULTLINE_WAL_IO_READ_FILE, EIO);
            return -1;
        }
        *received += (size_t)count;
    }
    return 0;
}

static enum faultline_wal_replay_result read_history(
    struct faultline_wal_writer *writer, struct faultline_wal_replay_state *state,
    struct worker_index *index, struct faultline_wal_replay_report *report)
{
    uint8_t bytes[FAULTLINE_WAL_MAX_RECORD_SIZE];
    size_t received;
    if (read_exact(writer, bytes, FAULTLINE_WAL_FILE_HEADER_SIZE, &received) < 0) { return FAULTLINE_WAL_REPLAY_IO_ERROR; }
    enum faultline_wal_result format = faultline_wal_file_header_decode(bytes, received);
    if (format != FAULTLINE_WAL_OK) { return format_error(report, format); }
    report->valid_bytes = FAULTLINE_WAL_FILE_HEADER_SIZE;
    for (;;) {
        report->error_offset = report->valid_bytes;
        if (read_exact(writer, bytes, FAULTLINE_WAL_RECORD_HEADER_SIZE, &received) < 0) { return FAULTLINE_WAL_REPLAY_IO_ERROR; }
        if (received == 0) { return FAULTLINE_WAL_REPLAY_OK; }
        if (state->last_sequence == UINT64_MAX) { return format_error(report, FAULTLINE_WAL_INVALID_SEQUENCE); }
        if (received < FAULTLINE_WAL_RECORD_HEADER_SIZE) {
            report->tail_bytes = received;
            return FAULTLINE_WAL_REPLAY_OK;
        }
        struct faultline_wal_record_header header;
        uint64_t sequence = state->last_sequence + 1;
        format = faultline_wal_record_header_decode(bytes, received, sequence, &header);
        if (format != FAULTLINE_WAL_OK) { return format_error(report, format); }
        if (read_exact(writer, bytes + FAULTLINE_WAL_RECORD_HEADER_SIZE, header.payload_length, &received) < 0) {
            return FAULTLINE_WAL_REPLAY_IO_ERROR;
        }
        if (received < header.payload_length) {
            report->tail_bytes = FAULTLINE_WAL_RECORD_HEADER_SIZE + received;
            return FAULTLINE_WAL_REPLAY_OK;
        }
        struct faultline_wal_record record;
        size_t size = FAULTLINE_WAL_RECORD_HEADER_SIZE + received, consumed;
        format = faultline_wal_record_decode(bytes, size, sequence, &record, &consumed);
        if (format != FAULTLINE_WAL_OK) { return format_error(report, format); }
        if (report->valid_bytes > (uint64_t)INT64_MAX - size) {
            (void)faultline_wal_writer_fail(writer, FAULTLINE_WAL_IO_READ_FILE, EOVERFLOW);
            return FAULTLINE_WAL_REPLAY_IO_ERROR;
        }
        enum faultline_wal_replay_result result = apply_record(state, index, &record, bytes, size, report);
        if (result != FAULTLINE_WAL_REPLAY_OK) { return result; }
        state->last_sequence = sequence;
        report->last_sequence = sequence;
        report->valid_bytes += size;
    }
}

enum faultline_wal_replay_result faultline_wal_replay_open_with_io(
    struct faultline_wal_writer *writer, const char *path,
    struct faultline_wal_replay_state *state, struct faultline_wal_replay_report *report,
    const struct faultline_wal_writer_io *io, void *context)
{
    if (writer == NULL || path == NULL || path[0] == '\0' || path[strlen(path) - 1] == '/' ||
        state == NULL || report == NULL || io == NULL || io->read == NULL || io->truncate == NULL ||
        io->lock == NULL || io->write == NULL || io->sync == NULL || io->close == NULL) {
        return FAULTLINE_WAL_REPLAY_INVALID_ARGUMENT;
    }
    *report = (struct faultline_wal_replay_report){0};
    enum faultline_wal_writer_result opened = faultline_wal_writer_open_file(writer, path, io, context, 0);
    if (opened != FAULTLINE_WAL_WRITE_OK) {
        return opened == FAULTLINE_WAL_WRITE_INVALID_STATE ? FAULTLINE_WAL_REPLAY_INVALID_STATE : FAULTLINE_WAL_REPLAY_IO_ERROR;
    }
    struct faultline_wal_replay_state *candidate = calloc(1, sizeof(*candidate));
    struct worker_index index = {0};
    enum faultline_wal_replay_result result;
    if (candidate == NULL) { result = FAULTLINE_WAL_REPLAY_NO_MEMORY; }
    else {
        faultline_scheduler_init(&candidate->scheduler);
        faultline_worker_registry_init(&candidate->workers);
        result = read_history(writer, candidate, &index, report);
    }
    free(index.ranges);
    if (result == FAULTLINE_WAL_REPLAY_OK && report->tail_bytes != 0) {
        while (writer->io->truncate(writer->io_context, writer->fd, (off_t)report->valid_bytes) < 0) {
            if (errno == EINTR) { continue; }
            (void)faultline_wal_writer_fail(writer, FAULTLINE_WAL_IO_TRUNCATE_TAIL, errno);
            result = FAULTLINE_WAL_REPLAY_IO_ERROR;
            break;
        }
    }
    if (result == FAULTLINE_WAL_REPLAY_OK &&
        (faultline_wal_writer_sync(writer, writer->fd, FAULTLINE_WAL_IO_SYNC_RECOVERY) != FAULTLINE_WAL_WRITE_OK ||
         faultline_wal_writer_sync(writer, writer->directory_fd, FAULTLINE_WAL_IO_SYNC_DIRECTORY) != FAULTLINE_WAL_WRITE_OK)) {
        result = FAULTLINE_WAL_REPLAY_IO_ERROR;
    }
    if (result == FAULTLINE_WAL_REPLAY_OK) {
        *state = *candidate;
        writer->synced_sequence = state->last_sequence;
        writer->next_sequence = state->last_sequence == UINT64_MAX ? 0 : state->last_sequence + 1;
        writer->state = FAULTLINE_WAL_WRITER_READY;
    } else if (writer->state != FAULTLINE_WAL_WRITER_FAILED) {
        (void)faultline_wal_writer_fail(writer,
            result == FAULTLINE_WAL_REPLAY_NO_MEMORY ? FAULTLINE_WAL_IO_ALLOCATE_REPLAY : FAULTLINE_WAL_IO_VALIDATE_REPLAY,
            result == FAULTLINE_WAL_REPLAY_NO_MEMORY ? ENOMEM : result == FAULTLINE_WAL_REPLAY_CAPACITY ? EOVERFLOW : EILSEQ);
    }
    free(candidate);
    return result;
}

enum faultline_wal_replay_result faultline_wal_replay_open(
    struct faultline_wal_writer *writer, const char *path,
    struct faultline_wal_replay_state *state, struct faultline_wal_replay_report *report)
{
    return faultline_wal_replay_open_with_io(writer, path, state, report, faultline_wal_writer_system_io(), NULL);
}
