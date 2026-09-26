#include "net.h"
#include "protocol.h"
#include "worker_registry.h"
#include "coordinator_store.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CLIENTS 64
#define CLIENT_FRAME_CAPACITY FAULTLINE_MESSAGE_MAX_FRAME_SIZE

_Static_assert(FAULTLINE_JOB_STORE_CAPACITY <= FAULTLINE_JOBS_MAX_ENTRIES,
               "job listing must fit every retained job");
_Static_assert(FAULTLINE_MAX_WORKERS <= FAULTLINE_WORKERS_MAX_ENTRIES,
               "worker listing must fit the entire registry");

enum client_phase { READING_MESSAGE, WRITING_REPLY };

struct client {
    int fd;
    enum client_phase phase;
    uint32_t worker_id;
    int job_client; /* Submission/status connections cannot become workers. */
    uint8_t input[CLIENT_FRAME_CAPACITY];
    uint8_t output[CLIENT_FRAME_CAPACITY];
    size_t received;
    size_t expected;
    size_t sent;
    size_t output_size;
    uint16_t reply_type;
    int64_t last_progress_ms;
};

static volatile sig_atomic_t stopping = 0;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static const char *job_state_name(enum faultline_job_state state)
{
    switch (state) {
    case FAULTLINE_JOB_QUEUED: return "QUEUED";
    case FAULTLINE_JOB_ASSIGNED: return "ASSIGNED";
    case FAULTLINE_JOB_RUNNING: return "RUNNING";
    case FAULTLINE_JOB_DONE: return "DONE";
    case FAULTLINE_JOB_FAILED: return "FAILED";
    default: return "INVALID";
    }
}

static void log_job(const char *event, const struct faultline_job *job,
                    const struct faultline_scheduler *jobs)
{
    printf("[INFO] coordinator %s job_id=%" PRIu64 " state=%s worker_id=%" PRIu32
           " attempt=%" PRIu64 " retry_count=%" PRIu32 " pending=%zu result_bytes=%zu",
           event, job->id, job_state_name(job->state), job->worker_id,
           job->attempt, job->retry_count, jobs->pending.count, job->result_size);
    if (job->state == FAULTLINE_JOB_DONE) {
        fputs(" result=\"", stdout);
        for (size_t i = 0; i < job->result_size; ++i) {
            unsigned int byte = job->result[i];
            if (byte >= 0x20 && byte <= 0x7e && byte != '"' && byte != '\\') {
                (void)putchar((int)byte);
            } else {
                printf("\\x%02x", byte);
            }
        }
        (void)putchar('"');
    }
    (void)putchar('\n');
}

static void close_client(struct client *client,
                          struct faultline_coordinator_store *store,
                          const char *reason)
{
    struct faultline_worker_registry *registry = &store->workers;
    struct faultline_scheduler *jobs = &store->jobs;
    if (client->worker_id != FAULTLINE_WORKER_ID_UNASSIGNED) {
        const struct faultline_job *active = faultline_scheduler_active(jobs, client->worker_id);
        if (active != NULL && store->failure == FAULTLINE_STORE_FAILURE_NONE) {
            uint64_t job_id = active->id;
            if (faultline_store_worker_lost(store, client->worker_id, faultline_monotonic_ms()) ==
                FAULTLINE_STORE_OK) {
                log_job("job_worker_lost", faultline_scheduler_find(jobs, job_id), jobs);
            }
            /* Any fatal failure is latched; the event loop will stop. */
        }
        enum faultline_registry_result result =
            faultline_worker_mark_dead(registry, client->worker_id, client->fd);

        if (result == FAULTLINE_REGISTRY_OK) {
            const struct faultline_worker *worker =
                faultline_worker_find(registry, client->worker_id);
            printf("[INFO] coordinator worker_dead worker_id=%" PRIu32
                   " fd=%d state=DEAD last_heartbeat_ms=%" PRId64 " reason=%s\n",
                   worker->id, client->fd, worker->last_heartbeat_ms, reason);
        } else {
            fprintf(stderr, "[ERROR] coordinator registry_disconnect_failed code=%d\n",
                    (int)result);
        }
    }
    /* Detach the worker before close() allows the OS to reuse this descriptor. */
    (void)close(client->fd);
    client->fd = -1;
    client->worker_id = FAULTLINE_WORKER_ID_UNASSIGNED;
}

static void reset_input(struct client *client)
{
    client->received = 0;
    client->expected = FAULTLINE_HEADER_SIZE;
    client->phase = READING_MESSAGE;
}

static void queue_message(struct client *client, struct faultline_coordinator_store *store,
                          const struct faultline_message *message, int64_t now)
{
    if (faultline_message_encode(client->output, sizeof(client->output), message,
                                 &client->output_size) != FAULTLINE_PROTOCOL_OK) {
        fputs("[ERROR] coordinator could not encode message\n", stderr);
        close_client(client, store, "encode_error");
        return;
    }
    /* A preceding synchronous commit may have taken substantial time. */
    now = faultline_monotonic_ms();
    if (now < 0) { close_client(client, store, "clock_error"); return; }
    client->sent = 0;
    client->reply_type = message->message_type;
    client->last_progress_ms = now;
    client->phase = WRITING_REPLY;
}

static void queue_reply(struct client *client, struct faultline_coordinator_store *store, uint16_t message_type,
                        uint32_t worker_id, int64_t now)
{
    const struct faultline_message reply = {
        .message_type = message_type, .payload.worker_id = worker_id
    };
    queue_message(client, store, &reply, now);
}

static int compare_job_summaries(const void *left, const void *right)
{
    uint64_t a = ((const struct faultline_job_summary *)left)->job_id;
    uint64_t b = ((const struct faultline_job_summary *)right)->job_id;
    return (a > b) - (a < b);
}

static int compare_worker_summaries(const void *left, const void *right)
{
    uint32_t a = ((const struct faultline_worker_summary *)left)->worker_id;
    uint32_t b = ((const struct faultline_worker_summary *)right)->worker_id;
    return (a > b) - (a < b);
}

static void queue_listing(struct client *client, struct faultline_coordinator_store *store,
                          uint16_t request_type, int heartbeat_timeout_ms)
{
    struct faultline_message reply = {0};
    int64_t now = faultline_monotonic_ms();
    if (now < 0) { close_client(client, store, "clock_error"); return; }
    client->job_client = 1;
    if (request_type == FAULTLINE_MSG_JOBS_REQUEST) {
        reply.message_type = FAULTLINE_MSG_JOBS_RESPONSE;
        struct faultline_jobs_payload *list = &reply.payload.jobs;
        list->count = store->jobs.count;
        for (size_t i = 0; i < list->count; ++i) {
            const struct faultline_job *job = &store->jobs.jobs[i];
            list->entries[i] = (struct faultline_job_summary){
                .job_id = job->id, .task_type = (uint16_t)job->task_type, .state = (uint16_t)job->state,
                .worker_id = job->worker_id, .attempt = job->attempt, .retry_count = job->retry_count,
                .max_retries = job->max_retries, .failure = (uint16_t)job->failure,
                .result_size = (uint32_t)job->result_size
            };
        }
        qsort(list->entries, list->count, sizeof(list->entries[0]), compare_job_summaries);
    } else {
        reply.message_type = FAULTLINE_MSG_WORKERS_RESPONSE;
        struct faultline_workers_payload *list = &reply.payload.workers;
        list->heartbeat_timeout_ms = (uint32_t)heartbeat_timeout_ms;
        for (size_t i = 0; i < FAULTLINE_MAX_WORKERS; ++i) {
            const struct faultline_worker *worker = &store->workers.workers[i];
            if (worker->state == FAULTLINE_WORKER_UNUSED) { continue; }
            if (worker->last_heartbeat_ms < 0 || now < worker->last_heartbeat_ms) {
                close_client(client, store, "clock_error");
                return;
            }
            const struct faultline_job *active = worker->state == FAULTLINE_WORKER_ALIVE ?
                faultline_scheduler_active(&store->jobs, worker->id) : NULL;
            list->entries[list->count++] = (struct faultline_worker_summary){
                .worker_id = worker->id,
                .state = worker->state == FAULTLINE_WORKER_ALIVE ?
                         FAULTLINE_WORKER_VIEW_ALIVE : FAULTLINE_WORKER_VIEW_DEAD,
                .heartbeat_age_ms = (uint64_t)(now - worker->last_heartbeat_ms),
                .job_id = active == NULL ? 0 : active->id,
                .attempt = active == NULL ? 0 : active->attempt
            };
        }
        /* Registry slot reuse can put a newer ID before an older one. */
        qsort(list->entries, list->count, sizeof(list->entries[0]), compare_worker_summaries);
    }
    queue_message(client, store, &reply, now);
}

static void handle_message(struct client *client,
                            struct faultline_coordinator_store *store,
                            const struct faultline_message *message, int64_t now, int heartbeat_timeout_ms)
{
    struct faultline_worker_registry *registry = &store->workers;
    struct faultline_scheduler *jobs = &store->jobs;
    enum faultline_registry_result result;

    switch (message->message_type) {
    case FAULTLINE_MSG_PING:
        printf("[INFO] coordinator ping_received fd=%d\n", client->fd);
        queue_reply(client, store, FAULTLINE_MSG_PONG, 0, now);
        break;
    case FAULTLINE_MSG_WORKER_REGISTER:
        if (client->job_client) {
            close_client(client, store, "job_client_cannot_register");
            return;
        }
        enum faultline_store_result registered = faultline_store_register(
            store, client->fd, faultline_monotonic_ms(), &client->worker_id);
        if (registered == FAULTLINE_STORE_FATAL) { return; }
        if (registered != FAULTLINE_STORE_OK) {
            fprintf(stderr, "[WARN] coordinator registration_rejected fd=%d code=%d\n",
                    client->fd, (int)registered);
            close_client(client, store, "registration_rejected");
            return;
        }
        printf("[INFO] coordinator worker_registered worker_id=%" PRIu32
               " fd=%d state=ALIVE last_heartbeat_ms=%" PRId64 "\n",
               client->worker_id, client->fd,
               faultline_worker_find(registry, client->worker_id)->last_heartbeat_ms);
        queue_reply(client, store, FAULTLINE_MSG_WORKER_REGISTER_ACK, client->worker_id, now);
        break;
    case FAULTLINE_MSG_HEARTBEAT:
        if (client->worker_id == FAULTLINE_WORKER_ID_UNASSIGNED ||
            message->payload.worker_id != client->worker_id) {
            close_client(client, store, "heartbeat_identity_mismatch");
            return;
        }
        result = faultline_worker_heartbeat(registry, message->payload.worker_id, client->fd, now);
        if (result != FAULTLINE_REGISTRY_OK) {
            close_client(client, store, "heartbeat_rejected");
            return;
        }
        printf("[INFO] coordinator heartbeat_received worker_id=%" PRIu32
               " fd=%d last_heartbeat_ms=%" PRId64 "\n",
               client->worker_id, client->fd, now);
        reset_input(client);
        break;
    case FAULTLINE_MSG_JOB_SUBMIT: {
        struct faultline_message ack = {.message_type = FAULTLINE_MSG_JOB_SUBMIT_ACK};
        if (client->worker_id != 0) {
            close_client(client, store, "worker_cannot_submit");
            return;
        }
        enum faultline_store_result accepted = faultline_store_submit(
            store, &message->payload.job_submit, faultline_monotonic_ms(), &ack.payload.job_submit_ack);
        if (accepted == FAULTLINE_STORE_FATAL) { return; }
        if (accepted != FAULTLINE_STORE_OK) {
            fprintf(stderr, "[WARN] coordinator submission_rejected fd=%d code=%d\n",
                    client->fd, (int)accepted);
            close_client(client, store, "submission_rejected");
            return;
        }
        client->job_client = 1;
        log_job("job_submitted", faultline_scheduler_find(jobs, ack.payload.job_submit_ack), jobs);
        queue_message(client, store, &ack, now);
        break;
    }
    case FAULTLINE_MSG_JOB_STATUS_REQUEST: {
        uint64_t id = message->payload.job_status_request;
        const struct faultline_job *job = faultline_scheduler_find(jobs, id);
        struct faultline_message reply = {.message_type = FAULTLINE_MSG_JOB_STATUS_NOT_FOUND,
                                          .payload.job_status_not_found = id};
        client->job_client = 1;
        if (job != NULL) {
            /* The event loop owns the published store. Copy one snapshot before
             * another event can change the job; queries require no WAL write. */
            reply.message_type = FAULTLINE_MSG_JOB_STATUS_RESPONSE;
            reply.payload.job_status_response = (struct faultline_job_status_payload){
                .job_id = job->id, .state = (uint16_t)job->state,
                .worker_id = job->worker_id, .attempt = job->attempt,
                .retry_count = job->retry_count, .max_retries = job->max_retries,
                .failure = (uint16_t)job->failure, .result_size = job->result_size
            };
            memcpy(reply.payload.job_status_response.result, job->result, job->result_size);
        }
        queue_message(client, store, &reply, now);
        break;
    }
    case FAULTLINE_MSG_JOB_STARTED:
    case FAULTLINE_MSG_JOB_COMPLETED:
    case FAULTLINE_MSG_JOB_FAILED: {
        const struct faultline_job *active = faultline_scheduler_active(jobs, client->worker_id);
        uint64_t id = active == NULL ? 0 : active->id;
        enum faultline_store_result reported = faultline_store_report(
            store, client->worker_id, message, faultline_monotonic_ms());
        if (reported == FAULTLINE_STORE_FATAL) { return; }
        if (reported != FAULTLINE_STORE_OK) {
            close_client(client, store, "invalid_job_report");
            return;
        }
        const char *event = message->message_type == FAULTLINE_MSG_JOB_STARTED ? "job_started" :
                            message->message_type == FAULTLINE_MSG_JOB_COMPLETED ? "job_completed" : "job_failed";
        log_job(event, faultline_scheduler_find(jobs, id), jobs);
        reset_input(client);
        break;
    }
    case FAULTLINE_MSG_JOBS_REQUEST:
    case FAULTLINE_MSG_WORKERS_REQUEST:
        queue_listing(client, store, message->message_type, heartbeat_timeout_ms);
        break;
    default:
        close_client(client, store, "unexpected_message");
        break;
    }
}

static void read_message(struct client *client,
                          struct faultline_coordinator_store *store, int64_t now, int heartbeat_timeout_ms)
{
    ssize_t count = recv(client->fd, client->input + client->received,
                            client->expected - client->received, 0);
    struct faultline_message message;
    enum faultline_protocol_result result;
    size_t consumed;

    if (count == 0) {
        if (client->received != 0) {
            fprintf(stderr, "[WARN] coordinator truncated_message fd=%d bytes=%zu\n",
                    client->fd, client->received);
        }
        close_client(client, store, client->received == 0 ? "eof" : "truncated_message");
        return;
    }
    if (count < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            perror("coordinator: recv");
            close_client(client, store, "recv_error");
        }
        return;
    }
    client->received += (size_t)count;
    client->last_progress_ms = now;
    if (client->received < client->expected) {
        return;
    }

    if (client->received == FAULTLINE_HEADER_SIZE) {
        struct faultline_header header;
        if (faultline_header_decode(client->input, client->received, &header) != FAULTLINE_PROTOCOL_OK) {
            close_client(client, store, "invalid_header");
            return;
        }
        int allowed = header.message_type == FAULTLINE_MSG_PING ||
            (header.message_type == FAULTLINE_MSG_WORKER_REGISTER && !client->job_client) ||
            ((header.message_type == FAULTLINE_MSG_JOB_SUBMIT ||
              header.message_type == FAULTLINE_MSG_JOB_STATUS_REQUEST ||
              header.message_type == FAULTLINE_MSG_JOBS_REQUEST ||
              header.message_type == FAULTLINE_MSG_WORKERS_REQUEST) && client->worker_id == 0) ||
            ((header.message_type == FAULTLINE_MSG_HEARTBEAT ||
              header.message_type == FAULTLINE_MSG_JOB_STARTED ||
              header.message_type == FAULTLINE_MSG_JOB_COMPLETED ||
              header.message_type == FAULTLINE_MSG_JOB_FAILED) && client->worker_id != 0);
        if (!allowed) {
            close_client(client, store, "unexpected_message");
            return;
        }
    }
    result = faultline_message_decode(client->input, client->received, &message, &consumed);
    if (result == FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL) {
        struct faultline_header header;

        /* The complete header declares a valid payload we have not read yet. */
        if (faultline_header_decode(client->input, client->received, &header) !=
            FAULTLINE_PROTOCOL_OK ||
            header.payload_length > sizeof(client->input) - FAULTLINE_HEADER_SIZE) {
            close_client(client, store, "unsupported_payload");
            return;
        }
        client->expected = FAULTLINE_HEADER_SIZE + (size_t)header.payload_length;
        return;
    }
    if (result != FAULTLINE_PROTOCOL_OK) {
        fprintf(stderr, "[WARN] coordinator invalid_message fd=%d code=%d\n",
                client->fd, (int)result);
        close_client(client, store, "invalid_message");
        return;
    }
    handle_message(client, store, &message, now, heartbeat_timeout_ms);
}

static void write_reply(struct client *client,
                         struct faultline_coordinator_store *store, int64_t now)
{
    ssize_t count = send(client->fd, client->output + client->sent,
                         client->output_size - client->sent, 0);

    if (count < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            perror("coordinator: send");
            close_client(client, store, "send_error");
        }
        return;
    }
    if (count == 0) {
        close_client(client, store, "send_closed");
        return;
    }
    client->sent += (size_t)count;
    client->last_progress_ms = now;
    if (client->sent == client->output_size) {
        if (client->reply_type == FAULTLINE_MSG_PONG) {
            printf("[INFO] coordinator pong_sent fd=%d\n", client->fd);
        } else if (client->reply_type == FAULTLINE_MSG_WORKER_REGISTER_ACK) {
            printf("[INFO] coordinator worker_register_ack_sent worker_id=%" PRIu32
                   " fd=%d\n", client->worker_id, client->fd);
        }
        reset_input(client);
    }
}

static void accept_clients(int listener, struct client clients[MAX_CLIENTS],
                           int64_t now)
{
    /* Bound each batch so an incoming connection flood cannot monopolize it. */
    for (size_t accepted = 0; accepted < MAX_CLIENTS && !stopping; ++accepted) {
        size_t slot;
        int fd = accept(listener, NULL, NULL);

        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("coordinator: accept");
            }
            return;
        }
        for (slot = 0; slot < MAX_CLIENTS; ++slot) {
            if (clients[slot].fd < 0) {
                break;
            }
        }
        if (slot == MAX_CLIENTS) {
            fputs("[WARN] coordinator client_limit_reached\n", stderr);
            (void)close(fd);
        } else if (faultline_set_nonblocking(fd) < 0) {
            perror("coordinator: nonblocking client");
            (void)close(fd);
        } else {
            clients[slot] = (struct client){
                .fd = fd, .phase = READING_MESSAGE, .expected = FAULTLINE_HEADER_SIZE,
                .last_progress_ms = now
            };
            printf("[INFO] coordinator client_connected fd=%d\n", fd);
        }
    }
}

static int heartbeat_poll_timeout(const struct faultline_worker_registry *registry,
                                   int64_t now, int timeout_ms)
{
    int wait_ms = 1000;

    for (size_t i = 0; i < FAULTLINE_MAX_WORKERS; ++i) {
        const struct faultline_worker *worker = &registry->workers[i];

        if (worker->state == FAULTLINE_WORKER_ALIVE) {
            int64_t remaining = timeout_ms - (now - worker->last_heartbeat_ms);

            if (remaining <= 0) {
                return 0;
            }
            if (remaining < wait_ms) {
                wait_ms = (int)remaining;
            }
        }
    }
    return wait_ms;
}

static int schedule_jobs(struct client clients[MAX_CLIENTS],
                          struct faultline_coordinator_store *store, int64_t now, int timeout_ms)
{
    struct faultline_worker_registry *registry = &store->workers;
    struct faultline_scheduler *jobs = &store->jobs;
    for (size_t i = 0; i < MAX_CLIENTS && jobs->pending.count != 0 && !stopping &&
         store->failure == FAULTLINE_STORE_FAILURE_NONE; ++i) {
        now = faultline_monotonic_ms();
        if (now < 0) {
            perror("coordinator: clock");
            return EXIT_FAILURE;
        }
        struct client *client = &clients[i];
        const struct faultline_worker *worker = faultline_worker_find(registry, client->worker_id);
        /* Never overwrite an ACK/reply or discard an incoming partial frame. */
        if (client->fd < 0 || worker == NULL || worker->state != FAULTLINE_WORKER_ALIVE ||
            worker->fd != client->fd || faultline_worker_timed_out(worker, now, timeout_ms) ||
            client->phase != READING_MESSAGE || client->received != 0 ||
            faultline_scheduler_active(jobs, client->worker_id) != NULL) {
            continue;
        }
        struct faultline_message assignment;
        if (faultline_store_assign(store, client->worker_id, now, &assignment) != FAULTLINE_STORE_OK) {
            fputs("[ERROR] coordinator schedule_failed\n", stderr);
            return EXIT_FAILURE;
        }
        log_job("job_assigned", faultline_scheduler_find(jobs, assignment.payload.job_assign.identity.job_id), jobs);
        queue_message(client, store, &assignment, now);
    }
    return store->failure == FAULTLINE_STORE_FAILURE_NONE ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int run_coordinator(int listener, int heartbeat_timeout_ms,
                           struct faultline_coordinator_store *store)
{
    struct faultline_worker_registry *registry = &store->workers;
    struct client clients[MAX_CLIENTS];
    struct pollfd descriptors[MAX_CLIENTS + 1];
    int status = EXIT_SUCCESS;

    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        clients[i] = (struct client){.fd = -1};
    }
    while (!stopping && store->failure == FAULTLINE_STORE_FAILURE_NONE) {
        int ready;
        int64_t now = faultline_monotonic_ms();

        if (now < 0) {
            perror("coordinator: clock");
            status = EXIT_FAILURE;
            break;
        }

        descriptors[0] = (struct pollfd){.fd = listener, .events = POLLIN};
        for (size_t i = 0; i < MAX_CLIENTS; ++i) {
            descriptors[i + 1] = (struct pollfd){
                .fd = clients[i].fd,
                .events = clients[i].phase == READING_MESSAGE ? POLLIN : POLLOUT
            };
        }
        ready = poll(descriptors, MAX_CLIENTS + 1,
                     heartbeat_poll_timeout(registry, now, heartbeat_timeout_ms));
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("coordinator: poll");
            status = EXIT_FAILURE;
            break;
        }
        now = faultline_monotonic_ms();
        if (now < 0) {
            perror("coordinator: clock");
            status = EXIT_FAILURE;
            break;
        }
        for (size_t i = 0; i < MAX_CLIENTS && !stopping &&
             store->failure == FAULTLINE_STORE_FAILURE_NONE; ++i) {
            /* Earlier clients may have blocked in fsync: refresh lease time. */
            now = faultline_monotonic_ms();
            if (now < 0) { status = EXIT_FAILURE; break; }
            short events = descriptors[i + 1].revents;
            const struct faultline_worker *worker;

            if (clients[i].fd < 0) {
                continue;
            }
            worker = faultline_worker_find(registry, clients[i].worker_id);
            /* Expire before reading: late bytes cannot revive an expired identity. */
            if (faultline_worker_timed_out(worker, now, heartbeat_timeout_ms)) {
                printf("[INFO] coordinator heartbeat_timeout worker_id=%" PRIu32
                       " fd=%d timeout_ms=%d detected_at_ms=%" PRId64
                       " silence_ms=%" PRId64 "\n", clients[i].worker_id,
                       clients[i].fd, heartbeat_timeout_ms, now,
                       now - worker->last_heartbeat_ms);
                close_client(&clients[i], store, "heartbeat_timeout");
                continue;
            }
            if ((events & POLLNVAL) != 0) {
                close_client(&clients[i], store, "invalid_descriptor");
                continue;
            }
            if (clients[i].phase == READING_MESSAGE &&
                (events & (POLLIN | POLLHUP | POLLERR)) != 0) {
                read_message(&clients[i], store, now, heartbeat_timeout_ms);
            } else if (clients[i].phase == WRITING_REPLY &&
                       (events & (POLLOUT | POLLHUP | POLLERR)) != 0) {
                write_reply(&clients[i], store, now);
            }
            if (clients[i].fd >= 0 &&
                (clients[i].worker_id == FAULTLINE_WORKER_ID_UNASSIGNED ||
                 clients[i].phase == WRITING_REPLY || clients[i].received != 0) &&
                now - clients[i].last_progress_ms >= FAULTLINE_IO_TIMEOUT_MS) {
                fprintf(stderr, "[WARN] coordinator client_timeout fd=%d\n",
                        clients[i].fd);
                close_client(&clients[i], store, "io_timeout");
            }
        }
        if (store->failure != FAULTLINE_STORE_FAILURE_NONE || status != EXIT_SUCCESS) {
            status = EXIT_FAILURE;
            break;
        }
        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            fputs("[ERROR] coordinator listener unavailable\n", stderr);
            status = EXIT_FAILURE;
            break;
        }
        if ((descriptors[0].revents & POLLIN) != 0) {
            accept_clients(listener, clients, now);
        }
        if (!stopping) {
            /* Disconnect cleanup may timestamp a retry after the poll snapshot. */
            now = faultline_monotonic_ms();
            if (now < 0) {
                perror("coordinator: clock");
                status = EXIT_FAILURE;
                break;
            }
            if (schedule_jobs(clients, store, now, heartbeat_timeout_ms) != EXIT_SUCCESS) {
                status = EXIT_FAILURE;
                break;
            }
        }
    }
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].fd >= 0) {
            /* Shutdown leaves durable active attempts for startup reconciliation.
             * In particular, storage failure must cause no further WAL writes. */
            (void)close(clients[i].fd);
        }
    }
    return store->failure == FAULTLINE_STORE_FAILURE_NONE ? status : EXIT_FAILURE;
}

static void log_storage_error(const struct faultline_coordinator_store *store)
{
    fprintf(stderr, "[ERROR] coordinator persistence_failed reason=%d operation=%s errno=%d "
            "write_code=%d replay_code=%d offset=%" PRIu64 " format=%d history=%d\n",
            (int)store->failure, faultline_wal_writer_operation_name(store->wal.failed_operation),
            store->wal.system_error, (int)store->write_result, (int)store->replay_result,
            store->replay_report.error_offset, (int)store->replay_report.format_error,
            (int)store->replay_report.history_error);
}

static void usage(FILE *stream)
{
    fprintf(stream, "Usage: faultline-coordinator [--port PORT] "
            "[--heartbeat-timeout-ms MS] [--wal PATH] [--init-wal]\n"
            "Default: 127.0.0.1:9000; heartbeat timeout: %d ms; WAL: faultline.wal\n"
            "Use --init-wal once to create a NEW log; otherwise an existing log is required.\n"
            "PORT must be in 1..65535; MS in 1..INT_MAX (decimal integers).\n",
            FAULTLINE_DEFAULT_HEARTBEAT_TIMEOUT_MS);
}

int main(int argc, char **argv)
{
    uint16_t port = FAULTLINE_DEFAULT_PORT;
    int heartbeat_timeout_ms = FAULTLINE_DEFAULT_HEARTBEAT_TIMEOUT_MS;
    int port_seen = 0, timeout_seen = 0, wal_seen = 0, initialize = 0;
    const char *wal_path = "faultline.wal";
    struct sigaction action = {0};

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return EXIT_SUCCESS;
    }
    for (int i = 1; i < argc; ++i) {
        if (i + 1 < argc && strcmp(argv[i], "--port") == 0 && !port_seen &&
            faultline_parse_port(argv[i + 1], &port) == 0) {
            port_seen = 1; ++i;
        } else if (i + 1 < argc && strcmp(argv[i], "--heartbeat-timeout-ms") == 0 &&
                   !timeout_seen && faultline_parse_duration_ms(argv[i + 1], &heartbeat_timeout_ms) == 0) {
            timeout_seen = 1; ++i;
        } else if (i + 1 < argc && strcmp(argv[i], "--wal") == 0 && !wal_seen && argv[i + 1][0] != '\0') {
            wal_path = argv[++i]; wal_seen = 1;
        } else if (strcmp(argv[i], "--init-wal") == 0 && !initialize) {
            initialize = 1;
        } else {
            usage(stderr);
            return EXIT_FAILURE;
        }
    }
    (void)setvbuf(stdout, NULL, _IOLBF, 0);
    action.sa_handler = request_stop;
    if (sigemptyset(&action.sa_mask) < 0 ||
        sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0 || faultline_ignore_sigpipe() < 0) {
        perror("coordinator: configure signals");
        return EXIT_FAILURE;
    }
    struct faultline_coordinator_store *store = malloc(sizeof(*store));
    if (store == NULL) { perror("coordinator: allocate durable store"); return EXIT_FAILURE; }
    faultline_store_init(store);
    int status = EXIT_FAILURE;
    int listener = -1;
    if (faultline_store_open(store, wal_path, initialize, faultline_monotonic_ms()) != FAULTLINE_STORE_OK) {
        log_storage_error(store);
    } else if (!stopping) {
        printf("[INFO] coordinator wal_ready path=%s sequence=%" PRIu64
               " jobs=%zu pending=%zu interrupted=%zu repaired_bytes=%" PRIu64 "\n",
               wal_path, store->wal.synced_sequence, store->jobs.count, store->jobs.pending.count,
               store->interrupted_jobs, store->replay_report.tail_bytes);
        for (size_t i = 0; i < store->jobs.count; ++i) {
            log_job("job_recovered", &store->jobs.jobs[i], &store->jobs);
        }
        listener = faultline_listen(FAULTLINE_DEFAULT_HOST, port, MAX_CLIENTS);
        if (listener < 0) { perror("coordinator: listen"); }
        else {
            printf("[INFO] coordinator listening address=%s port=%u heartbeat_timeout_ms=%d\n",
                   FAULTLINE_DEFAULT_HOST, (unsigned int)port, heartbeat_timeout_ms);
            status = run_coordinator(listener, heartbeat_timeout_ms, store);
            if (store->failure != FAULTLINE_STORE_FAILURE_NONE) { log_storage_error(store); }
        }
    } else { status = EXIT_SUCCESS; }
    if (listener >= 0) { (void)close(listener); }
    enum faultline_store_failure previous_failure = store->failure;
    if (faultline_store_close(store) != FAULTLINE_STORE_OK) {
        if (previous_failure == FAULTLINE_STORE_FAILURE_NONE) { log_storage_error(store); }
        status = EXIT_FAILURE;
    }
    free(store);
    puts("[INFO] coordinator stopped");
    return status;
}
