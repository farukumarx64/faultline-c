#include "net.h"
#include "log.h"
#include "protocol.h"
#include "task.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static volatile sig_atomic_t stopping = 0;

static void request_stop(int signal_number)
{
    stopping = signal_number;
}

static void usage(FILE *stream)
{
    fprintf(stream, "Usage: faultline-worker [--coordinator IPv4:PORT] "
            "[--heartbeat-interval-ms MS]\n"
            "Default coordinator: 127.0.0.1:9000; heartbeat interval: %d ms\n"
            "MS must be a positive decimal integer in 1..INT_MAX.\n",
            FAULTLINE_DEFAULT_HEARTBEAT_INTERVAL_MS);
}

enum wait_result { WAIT_ERROR = -1, WAIT_STOP, WAIT_INPUT, WAIT_DEADLINE };

/* The same interruptible wait serves both ACK expiry and heartbeat scheduling. */
static int wait_for_input(int fd, int64_t deadline)
{
    struct pollfd descriptor = {.fd = fd, .events = POLLIN};

    while (!stopping) {
        int timeout = 250;
        int ready;

        if (deadline >= 0) {
            int64_t now = faultline_monotonic_ms();

            if (now < 0) {
                return -1;
            }
            if (now >= deadline) {
                return WAIT_DEADLINE;
            }
            if (deadline - now < timeout) {
                timeout = (int)(deadline - now);
            }
        }
        /* A bounded wait also handles a signal arriving just before poll(). */
        ready = poll(&descriptor, 1, timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if ((descriptor.revents & POLLNVAL) != 0) {
            errno = EBADF;
            return -1;
        }
        if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            return stopping ? 0 : 1;
        }
    }
    return 0;
}

static int receive_registration_ack(int fd, uint32_t *worker_id)
{
    uint8_t wire[FAULTLINE_HEADER_SIZE + FAULTLINE_WORKER_REGISTER_ACK_PAYLOAD_SIZE];
    size_t received = 0;
    size_t expected = FAULTLINE_HEADER_SIZE;
    size_t consumed;
    struct faultline_message message;
    int64_t start = faultline_monotonic_ms();

    if (start < 0) {
        faultline_log_error("ERROR", "worker", "clock", errno);
        return -1;
    }
    while (received < expected) {
        int ready = wait_for_input(fd, start + FAULTLINE_IO_TIMEOUT_MS);
        ssize_t count;

        if (ready == WAIT_DEADLINE) {
            errno = ETIMEDOUT;
            ready = WAIT_ERROR;
        }
        if (ready <= 0) {
            if (ready < 0) {
                faultline_log_error("ERROR", "worker", "receive registration ACK", errno);
            }
            return -1;
        }
        count = recv(fd, wire + received, expected - received, 0);
        if (count == 0) {
            (void)faultline_log(stderr, "ERROR", "worker", "runtime_error", "message=\"worker: coordinator closed %s registration ACK\"", received == 0 ? "before" : "during");
            return -1;
        }
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            faultline_log_error("ERROR", "worker", "receive registration ACK", errno);
            return -1;
        }
        received += (size_t)count;
        if (received == FAULTLINE_HEADER_SIZE) {
            struct faultline_header header;

            if (faultline_header_decode(wire, received, &header) != FAULTLINE_PROTOCOL_OK ||
                header.message_type != FAULTLINE_MSG_WORKER_REGISTER_ACK ||
                header.payload_length != FAULTLINE_WORKER_REGISTER_ACK_PAYLOAD_SIZE) {
                (void)faultline_log(stderr, "ERROR", "worker", "runtime_error", "message=\"worker: expected registration ACK with a 4-byte worker ID\"");
                return -1;
            }
            expected = sizeof(wire);
        }
    }
    if (faultline_message_decode(wire, received, &message, &consumed) != FAULTLINE_PROTOCOL_OK) {
        (void)faultline_log(stderr, "ERROR", "worker", "runtime_error", "message=\"worker: invalid registration ACK payload\"");
        return -1;
    }
    *worker_id = message.payload.worker_id;
    return 0;
}

struct execution {
    struct faultline_job_assign_payload assignment;
    struct faultline_task_result result;
    enum faultline_task_status status;
    pthread_t thread;
    int thread_created;
    atomic_bool cancel;
    atomic_bool done;
};

static void *execute_task(void *argument)
{
    struct execution *execution = argument;
    const struct faultline_job_assign_payload *job = &execution->assignment;
    execution->status = faultline_task_execute(job->task_type, job->arguments,
        job->argument_size, &execution->cancel, &execution->result);
    /* Publish the result before the networking thread is allowed to read it. */
    atomic_store(&execution->done, true);
    return NULL;
}

static int start_execution(struct execution *execution)
{
    sigset_t blocked, previous;
    (void)sigemptyset(&blocked);
    (void)sigaddset(&blocked, SIGINT);
    (void)sigaddset(&blocked, SIGTERM);
    /* The task inherits this mask; only the main thread runs our stop handler. */
    int error = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    if (error != 0) { return error; }
    atomic_store(&execution->cancel, false);
    atomic_store(&execution->done, false);
    error = pthread_create(&execution->thread, NULL, execute_task, execution);
    execution->thread_created = error == 0;
    int restore_error = pthread_sigmask(SIG_SETMASK, &previous, NULL);
    return restore_error != 0 ? restore_error : error;
}

static int send_message(int fd, const struct faultline_message *message)
{
    uint8_t wire[FAULTLINE_MESSAGE_MAX_FRAME_SIZE];
    size_t written;
    if (faultline_message_encode(wire, sizeof(wire), message, &written) != FAULTLINE_PROTOCOL_OK) {
        (void)faultline_log(stderr, "ERROR", "worker", "runtime_error", "message=\"worker: could not encode job report\"");
        return -1;
    }
    if (faultline_send_all(fd, wire, written, FAULTLINE_IO_TIMEOUT_MS) < 0) {
        faultline_log_error("ERROR", "worker", "send job report", errno);
        return -1;
    }
    return 0;
}

static int report_execution(int fd, struct execution *execution)
{
    const struct faultline_job_identity identity = execution->assignment.identity;
    const char *status_name;
    switch (execution->status) {
    case FAULTLINE_TASK_OK: status_name = "ok"; break;
    case FAULTLINE_TASK_INVALID_ARGUMENT: status_name = "invalid_arguments"; break;
    case FAULTLINE_TASK_CANCELLED: status_name = "cancelled"; break;
    default: status_name = "system_error"; break;
    }
    struct faultline_message report = {0};
    if (execution->status == FAULTLINE_TASK_OK) {
        report.message_type = FAULTLINE_MSG_JOB_COMPLETED;
        report.payload.job_completed.identity = identity;
        report.payload.job_completed.result_size = execution->result.size;
        memcpy(report.payload.job_completed.result, execution->result.bytes, execution->result.size);
    } else {
        report.message_type = FAULTLINE_MSG_JOB_FAILED;
        report.payload.job_failed.identity = identity;
        report.payload.job_failed.failure = FAULTLINE_JOB_FAILURE_TASK;
    }
    if (send_message(fd, &report) < 0) { return -1; }
    (void)faultline_log(stdout, execution->status == FAULTLINE_TASK_OK ? "INFO" : "WARN", "worker",
           execution->status == FAULTLINE_TASK_OK ? "job_completed_sent" : "job_failed_sent",
           "job_id=%" PRIu64 " worker_id=%" PRIu32
           " attempt=%" PRIu64 " task_status=%s result_bytes=%zu acceptance=unconfirmed",
           identity.job_id, identity.worker_id, identity.attempt, status_name,
           execution->status == FAULTLINE_TASK_OK ? execution->result.size : 0);
    return 0;
}

static int worker_loop(int fd, uint32_t worker_id, int interval_ms, struct execution *execution)
{
    const struct faultline_message heartbeat = {
        .message_type = FAULTLINE_MSG_HEARTBEAT, .payload.worker_id = worker_id
    };
    uint8_t wire[FAULTLINE_HEADER_SIZE + FAULTLINE_HEARTBEAT_PAYLOAD_SIZE];
    size_t written;
    uint8_t input[FAULTLINE_MESSAGE_MAX_FRAME_SIZE];
    size_t received = 0, expected = FAULTLINE_HEADER_SIZE;
    int64_t frame_started = -1;
    int64_t now = faultline_monotonic_ms();
    int64_t next_heartbeat;

    if (now < 0) {
        faultline_log_error("ERROR", "worker", "clock", errno);
        return EXIT_FAILURE;
    }
    if (faultline_message_encode(wire, sizeof(wire), &heartbeat, &written) !=
        FAULTLINE_PROTOCOL_OK) {
        (void)faultline_log(stderr, "ERROR", "worker", "runtime_error", "message=\"worker: could not encode heartbeat\"");
        return EXIT_FAILURE;
    }
    next_heartbeat = now + interval_ms;
    while (!stopping) {
        if (execution->thread_created && atomic_load(&execution->done)) {
            int error = pthread_join(execution->thread, NULL);
            if (error != 0) {
                faultline_log_error("ERROR", "worker", "join task", error);
                return EXIT_FAILURE;
            }
            execution->thread_created = 0;
            if (stopping) { break; }
            if (report_execution(fd, execution) < 0) { return EXIT_FAILURE; }
            /* Ownership is released only after the complete terminal frame was sent. */
            execution->assignment.identity.job_id = 0;
        }
        now = faultline_monotonic_ms();
        if (now < 0) { faultline_log_error("ERROR", "worker", "clock", errno); return EXIT_FAILURE; }
        int64_t deadline = next_heartbeat;
        /* Poll completion at most 50 ms later, without a busy loop or a second socket writer. */
        if (execution->thread_created && now + 50 < deadline) { deadline = now + 50; }
        if (frame_started >= 0 && frame_started + FAULTLINE_IO_TIMEOUT_MS < deadline) {
            deadline = frame_started + FAULTLINE_IO_TIMEOUT_MS;
        }
        int ready = wait_for_input(fd, deadline);
        if (ready == WAIT_STOP) { break; }
        if (ready == WAIT_ERROR) {
            faultline_log_error("ERROR", "worker", "wait for coordinator", errno);
            return EXIT_FAILURE;
        }
        now = faultline_monotonic_ms();
        if (now < 0) { faultline_log_error("ERROR", "worker", "clock", errno); return EXIT_FAILURE; }
        if (frame_started >= 0 && now - frame_started >= FAULTLINE_IO_TIMEOUT_MS) {
            (void)faultline_log(stderr, "ERROR", "worker", "runtime_error", "message=\"worker: assignment receive timeout\"");
            return EXIT_FAILURE;
        }
        /* Input traffic and partial assignments never postpone heartbeats. */
        if (now >= next_heartbeat) {
            if (faultline_send_all(fd, wire, written, FAULTLINE_IO_TIMEOUT_MS) < 0) {
                faultline_log_error("ERROR", "worker", "send heartbeat", errno);
                return EXIT_FAILURE;
            }
            now = faultline_monotonic_ms();
            if (now < 0) { faultline_log_error("ERROR", "worker", "clock", errno); return EXIT_FAILURE; }
            (void)faultline_log(stdout, "INFO", "worker", "heartbeat_sent", "worker_id=%" PRIu32, worker_id);
            next_heartbeat = now + interval_ms;
            continue;
        }
        if (ready != WAIT_INPUT) { continue; }
        ssize_t count = recv(fd, input + received, expected - received, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) { continue; }
        if (count <= 0) {
            if (count < 0) { faultline_log_error("ERROR", "worker", "coordinator connection", errno); }
            else { (void)faultline_log(stderr, "ERROR", "worker", "runtime_error", "message=\"worker: coordinator disconnected\""); }
            return EXIT_FAILURE;
        }
        if (received == 0) { frame_started = now; }
        received += (size_t)count;
        const uint8_t prefix[] = {0x46, 0x4c, 0x49, 0x4e, 0, 1};
        size_t prefix_size = received < sizeof(prefix) ? received : sizeof(prefix);
        if (memcmp(input, prefix, prefix_size) != 0) {
            (void)faultline_log(stderr, "ERROR", "worker", "runtime_error", "message=\"worker: unexpected data after registration\"");
            return EXIT_FAILURE;
        }
        if (received < expected) { continue; }
        if (received == FAULTLINE_HEADER_SIZE) {
            struct faultline_header header;
            if (faultline_header_decode(input, received, &header) != FAULTLINE_PROTOCOL_OK ||
                header.message_type != FAULTLINE_MSG_JOB_ASSIGN ||
                header.payload_length < FAULTLINE_JOB_ASSIGN_PREFIX_SIZE ||
                header.payload_length > sizeof(input) - FAULTLINE_HEADER_SIZE) {
                (void)faultline_log(stderr, "ERROR", "worker", "runtime_error", "message=\"worker: unexpected data after registration; expected job assignment\"");
                return EXIT_FAILURE;
            }
            expected = FAULTLINE_HEADER_SIZE + (size_t)header.payload_length;
            continue;
        }
        struct faultline_message message;
        size_t consumed;
        if (faultline_message_decode(input, received, &message, &consumed) != FAULTLINE_PROTOCOL_OK ||
            message.payload.job_assign.identity.worker_id != worker_id ||
            execution->assignment.identity.job_id != 0) {
            (void)faultline_log(stderr, "ERROR", "worker", "runtime_error", "message=\"worker: invalid assignment or worker already busy\"");
            return EXIT_FAILURE;
        }
        execution->assignment = message.payload.job_assign;
        const struct faultline_job_assign_payload *active = &execution->assignment;
        (void)faultline_log(stdout, "INFO", "worker", "job_assigned", "job_id=%" PRIu64 " worker_id=%" PRIu32
               " attempt=%" PRIu64 " task_type=%u argument_bytes=%zu", active->identity.job_id, worker_id, active->identity.attempt, (unsigned int)active->task_type, active->argument_size);
        const struct faultline_message started = {
            .message_type = FAULTLINE_MSG_JOB_STARTED, .payload.job_started = active->identity
        };
        if (stopping) { break; }
        if (send_message(fd, &started) < 0) { return EXIT_FAILURE; }
        (void)faultline_log(stdout, "INFO", "worker", "job_started_sent",
            "job_id=%" PRIu64 " worker_id=%" PRIu32 " attempt=%" PRIu64 " acceptance=unconfirmed",
            active->identity.job_id, worker_id, active->identity.attempt);
        int error = start_execution(execution);
        if (error != 0) {
            faultline_log_error("ERROR", "worker", "start task thread", error);
            /* A mask-restore failure after creation needs cancellation before exiting. */
            if (execution->thread_created) { return EXIT_FAILURE; }
            execution->status = FAULTLINE_TASK_SYSTEM_ERROR;
            if (report_execution(fd, execution) < 0) { return EXIT_FAILURE; }
            execution->assignment.identity.job_id = 0;
        } else {
            (void)faultline_log(stdout, "INFO", "worker", "task_thread_started",
                "job_id=%" PRIu64 " worker_id=%" PRIu32 " attempt=%" PRIu64,
                active->identity.job_id, worker_id, active->identity.attempt);
        }
        received = 0;
        expected = FAULTLINE_HEADER_SIZE;
        frame_started = -1;
    }
    return EXIT_SUCCESS;
}

static int run_worker(int fd, uint32_t worker_id, int interval_ms)
{
    struct execution execution = {0};
    atomic_init(&execution.cancel, false);
    atomic_init(&execution.done, false);
    int status = worker_loop(fd, worker_id, interval_ms, &execution);
    /* Every exit path (signal, EOF, bad frame, failed send) stops and joins work. */
    if (execution.thread_created) {
        atomic_store(&execution.cancel, true);
        int error = pthread_join(execution.thread, NULL);
        if (error != 0) {
            faultline_log_error("ERROR", "worker", "join cancelled task", error);
            return EXIT_FAILURE;
        }
        (void)faultline_log(stdout, "INFO", "worker", "task_joined",
            "job_id=%" PRIu64 " worker_id=%" PRIu32 " attempt=%" PRIu64
            " cancel_requested=1 terminal_report_sent=0",
            execution.assignment.identity.job_id, worker_id, execution.assignment.identity.attempt);
    }
    return status;
}

int main(int argc, char **argv)
{
    char host[INET_ADDRSTRLEN] = FAULTLINE_DEFAULT_HOST;
    uint16_t port = FAULTLINE_DEFAULT_PORT;
    int interval_ms = FAULTLINE_DEFAULT_HEARTBEAT_INTERVAL_MS;
    int endpoint_seen = 0;
    int interval_seen = 0;
    uint32_t worker_id = FAULTLINE_WORKER_ID_UNASSIGNED;
    const struct faultline_message registration = {
        .message_type = FAULTLINE_MSG_WORKER_REGISTER, .payload.worker_id = 0
    };
    uint8_t wire[FAULTLINE_HEADER_SIZE];
    size_t written;
    struct sigaction action = {0};
    int fd;
    int status = EXIT_FAILURE;

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return EXIT_SUCCESS;
    }
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 < argc && strcmp(argv[i], "--coordinator") == 0 && !endpoint_seen &&
            faultline_parse_endpoint(argv[i + 1], host, sizeof(host), &port) == 0) {
            endpoint_seen = 1;
        } else if (i + 1 < argc && strcmp(argv[i], "--heartbeat-interval-ms") == 0 &&
                   !interval_seen && faultline_parse_duration_ms(argv[i + 1], &interval_ms) == 0) {
            interval_seen = 1;
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
        faultline_log_error("ERROR", "worker", "configure signals", errno);
        return EXIT_FAILURE;
    }
    fd = faultline_connect(host, port, FAULTLINE_IO_TIMEOUT_MS);
    if (fd < 0) {
        if (stopping) {
            return EXIT_SUCCESS;
        }
        faultline_log_error("ERROR", "worker", "connect", errno);
        return EXIT_FAILURE;
    }
    if (stopping) {
        goto done;
    }
    if (faultline_message_encode(wire, sizeof(wire), &registration, &written) !=
        FAULTLINE_PROTOCOL_OK) {
        (void)faultline_log(stderr, "ERROR", "worker", "runtime_error", "message=\"worker: could not encode registration\"");
        goto done;
    }
    if (faultline_send_all(fd, wire, written, FAULTLINE_IO_TIMEOUT_MS) < 0) {
        faultline_log_error("ERROR", "worker", "send registration", errno);
        goto done;
    }
    if (receive_registration_ack(fd, &worker_id) < 0 || stopping) {
        goto done;
    }
    (void)faultline_log(stdout, "INFO", "worker", "registered", "worker_id=%" PRIu32
           " coordinator=%s:%u heartbeat_interval_ms=%d", worker_id, host, (unsigned int)port, interval_ms);
    status = run_worker(fd, worker_id, interval_ms);

done:
    (void)close(fd);
    if (worker_id != FAULTLINE_WORKER_ID_UNASSIGNED) {
        (void)faultline_log(stdout, stopping || status == EXIT_SUCCESS ? "INFO" : "ERROR", "worker", "stopped",
            "worker_id=%" PRIu32 " exit_code=%d reason=%s signal=%d", worker_id,
            stopping ? EXIT_SUCCESS : status, stopping ? "signal" : "error", (int)stopping);
    }
    return stopping ? EXIT_SUCCESS : status;
}
