#include "net.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *stream)
{
    fputs("Usage: faultline ping [--coordinator IPv4:PORT]\n"
          "       faultline submit TASK [--args TEXT | --args-hex HEX]\n"
          "                        [--max-retries N] [--coordinator IPv4:PORT]\n"
          "       faultline status ID [--coordinator IPv4:PORT]\n"
          "       faultline jobs [--coordinator IPv4:PORT]\n"
          "       faultline workers [--coordinator IPv4:PORT]\n"
          "Tasks: sleep, prime_count, fibonacci, hash. Arguments: at most 1024 bytes.\n"
          "sleep: milliseconds 0..86400000; prime_count: inclusive bound 0..100000000.\n"
          "fibonacci: index 0..93; hash: raw bytes, FNV-1a 64-bit checksum.\n"
          "Numeric arguments must be decimal digits. Workers validate task inputs.\n"
          "Submission prints a job ID; status displays its state and saved result.\n"
          "Status ID: decimal 1..18446744073709551615. Exit: 0 found, 2 not found, 1 error.\n"
          "Default coordinator: 127.0.0.1:9000; max retries: 0.\n", stream);
}

static int run_ping(int argc, char **argv)
{
    char host[INET_ADDRSTRLEN] = FAULTLINE_DEFAULT_HOST;
    uint16_t port = FAULTLINE_DEFAULT_PORT;
    struct faultline_header header = {
        .magic = FAULTLINE_PROTOCOL_MAGIC,
        .version = FAULTLINE_PROTOCOL_VERSION,
        .message_type = FAULTLINE_MSG_PING,
        .payload_length = 0
    };
    uint8_t wire[FAULTLINE_HEADER_SIZE];
    enum faultline_receive_result receive_result;
    enum faultline_protocol_result protocol_result;
    int fd;
    int status = EXIT_FAILURE;

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return EXIT_SUCCESS;
    }
    if ((argc != 2 && argc != 4) || strcmp(argv[1], "ping") != 0) {
        usage(stderr);
        return EXIT_FAILURE;
    }
    if (argc == 4 && (strcmp(argv[2], "--coordinator") != 0 ||
                     faultline_parse_endpoint(argv[3], host, sizeof(host), &port) < 0)) {
        usage(stderr);
        return EXIT_FAILURE;
    }
    if (faultline_ignore_sigpipe() < 0) {
        perror("faultline: configure SIGPIPE");
        return EXIT_FAILURE;
    }
    fd = faultline_connect(host, port, FAULTLINE_IO_TIMEOUT_MS);
    if (fd < 0) {
        perror("faultline: connect");
        return EXIT_FAILURE;
    }
    protocol_result = faultline_header_encode(wire, sizeof(wire), &header);
    if (protocol_result != FAULTLINE_PROTOCOL_OK) {
        fprintf(stderr, "faultline: encode PING failed (protocol error %d)\n",
                (int)protocol_result);
        goto done;
    }
    if (faultline_send_all(fd, wire, sizeof(wire), FAULTLINE_IO_TIMEOUT_MS) < 0) {
        perror("faultline: send PING");
        goto done;
    }
    receive_result = faultline_recv_exact(fd, wire, sizeof(wire),
                                          FAULTLINE_IO_TIMEOUT_MS);
    if (receive_result != FAULTLINE_RECEIVE_OK) {
        if (receive_result == FAULTLINE_RECEIVE_ERROR) {
            perror("faultline: receive PONG");
        } else {
            fprintf(stderr, "faultline: coordinator closed %s PONG\n",
                    receive_result == FAULTLINE_RECEIVE_EOF ? "before" : "during");
        }
        goto done;
    }
    protocol_result = faultline_header_decode(wire, sizeof(wire), &header);
    if (protocol_result != FAULTLINE_PROTOCOL_OK) {
        fprintf(stderr, "faultline: invalid PONG header (protocol error %d)\n",
                (int)protocol_result);
        goto done;
    }
    if (header.message_type != FAULTLINE_MSG_PONG || header.payload_length != 0) {
        fputs("faultline: expected PONG with an empty payload\n", stderr);
        goto done;
    }
    puts("PONG");
    status = EXIT_SUCCESS;

done:
    (void)close(fd);
    return status;
}

static int hex_digit(char byte)
{
    if (byte >= '0' && byte <= '9') { return byte - '0'; }
    if (byte >= 'a' && byte <= 'f') { return byte - 'a' + 10; }
    if (byte >= 'A' && byte <= 'F') { return byte - 'A' + 10; }
    return -1;
}

static int parse_retries(const char *text, uint32_t *retries)
{
    uint32_t value = 0;
    if (*text == '\0') { return -1; }
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') { return -1; }
        uint32_t digit = (uint32_t)(*cursor - '0');
        if (value > (UINT32_MAX - digit) / UINT32_C(10)) { return -1; }
        value = value * UINT32_C(10) + digit;
    }
    *retries = value;
    return 0;
}

static int receive_submit_ack(int fd, uint64_t *job_id)
{
    uint8_t wire[FAULTLINE_HEADER_SIZE + FAULTLINE_JOB_SUBMIT_ACK_PAYLOAD_SIZE];
    struct faultline_header header;
    struct faultline_message message;
    size_t consumed;
    int64_t start = faultline_monotonic_ms();
    if (start < 0) { return -1; }
    if (faultline_recv_exact(fd, wire, FAULTLINE_HEADER_SIZE, FAULTLINE_IO_TIMEOUT_MS) !=
        FAULTLINE_RECEIVE_OK) {
        fputs("faultline: failed to receive submission ACK header\n", stderr);
        return -1;
    }
    if (faultline_header_decode(wire, FAULTLINE_HEADER_SIZE, &header) != FAULTLINE_PROTOCOL_OK ||
        header.message_type != FAULTLINE_MSG_JOB_SUBMIT_ACK ||
        header.payload_length != FAULTLINE_JOB_SUBMIT_ACK_PAYLOAD_SIZE) {
        fputs("faultline: expected submission ACK with an 8-byte job ID\n", stderr);
        return -1;
    }
    int64_t now = faultline_monotonic_ms();
    if (now < 0 || now - start >= FAULTLINE_IO_TIMEOUT_MS ||
        faultline_recv_exact(fd, wire + FAULTLINE_HEADER_SIZE, FAULTLINE_JOB_SUBMIT_ACK_PAYLOAD_SIZE,
                             FAULTLINE_IO_TIMEOUT_MS - (int)(now - start)) != FAULTLINE_RECEIVE_OK) {
        fputs("faultline: failed to receive submission ACK payload\n", stderr);
        return -1;
    }
    if (faultline_message_decode(wire, sizeof(wire), &message, &consumed) != FAULTLINE_PROTOCOL_OK) {
        fputs("faultline: invalid submission ACK payload\n", stderr);
        return -1;
    }
    *job_id = message.payload.job_submit_ack;
    return 0;
}

static int run_submit(int argc, char **argv)
{
    char host[INET_ADDRSTRLEN] = FAULTLINE_DEFAULT_HOST;
    uint16_t port = FAULTLINE_DEFAULT_PORT;
    struct faultline_message request = {.message_type = FAULTLINE_MSG_JOB_SUBMIT};
    struct faultline_job_submit_payload *submit = &request.payload.job_submit;
    const char *tasks[] = {"sleep", "prime_count", "fibonacci", "hash"};
    int arguments_seen = 0, retries_seen = 0, endpoint_seen = 0;
    if (argc < 3) { usage(stderr); return EXIT_FAILURE; }
    for (size_t i = 0; i < sizeof(tasks) / sizeof(tasks[0]); ++i) {
        if (strcmp(argv[2], tasks[i]) == 0) { submit->task_type = (uint16_t)(i + 1); }
    }
    if (submit->task_type == 0) { usage(stderr); return EXIT_FAILURE; }
    for (int i = 3; i < argc; i += 2) {
        if (i + 1 >= argc) { usage(stderr); return EXIT_FAILURE; }
        if (strcmp(argv[i], "--coordinator") == 0 && !endpoint_seen &&
            faultline_parse_endpoint(argv[i + 1], host, sizeof(host), &port) == 0) {
            endpoint_seen = 1;
        } else if (strcmp(argv[i], "--max-retries") == 0 && !retries_seen &&
                   parse_retries(argv[i + 1], &submit->max_retries) == 0) {
            retries_seen = 1;
        } else if (strcmp(argv[i], "--args") == 0 && !arguments_seen &&
                   strlen(argv[i + 1]) <= sizeof(submit->arguments)) {
            arguments_seen = 1;
            submit->argument_size = strlen(argv[i + 1]);
            memcpy(submit->arguments, argv[i + 1], submit->argument_size);
        } else if (strcmp(argv[i], "--args-hex") == 0 && !arguments_seen &&
                   strlen(argv[i + 1]) <= 2 * sizeof(submit->arguments) &&
                   strlen(argv[i + 1]) % 2 == 0) {
            arguments_seen = 1;
            submit->argument_size = strlen(argv[i + 1]) / 2;
            for (size_t j = 0; j < submit->argument_size; ++j) {
                int high = hex_digit(argv[i + 1][2 * j]);
                int low = hex_digit(argv[i + 1][2 * j + 1]);
                if (high < 0 || low < 0) { usage(stderr); return EXIT_FAILURE; }
                submit->arguments[j] = (uint8_t)(high * 16 + low);
            }
        } else { usage(stderr); return EXIT_FAILURE; }
    }
    uint8_t wire[FAULTLINE_MESSAGE_MAX_FRAME_SIZE];
    size_t written;
    if (faultline_message_encode(wire, sizeof(wire), &request, &written) != FAULTLINE_PROTOCOL_OK) {
        fputs("faultline: could not encode submission\n", stderr);
        return EXIT_FAILURE;
    }
    if (faultline_ignore_sigpipe() < 0) { perror("faultline: configure SIGPIPE"); return EXIT_FAILURE; }
    int fd = faultline_connect(host, port, FAULTLINE_IO_TIMEOUT_MS);
    if (fd < 0) { perror("faultline: connect"); return EXIT_FAILURE; }
    uint64_t job_id = 0;
    int status = EXIT_FAILURE;
    if (faultline_send_all(fd, wire, written, FAULTLINE_IO_TIMEOUT_MS) < 0) {
        perror("faultline: send submission");
    } else if (receive_submit_ack(fd, &job_id) == 0) {
        printf("job_id=%" PRIu64 "\n", job_id);
        status = EXIT_SUCCESS;
    }
    if (status != EXIT_SUCCESS) {
        fputs("faultline: submission unconfirmed; the coordinator may already have accepted it\n", stderr);
    }
    (void)close(fd);
    return status;
}

static int parse_job_id(const char *text, uint64_t *job_id)
{
    uint64_t value = 0;
    if (*text == '\0') { return -1; }
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9') { return -1; }
        uint64_t digit = (uint64_t)(*cursor - '0');
        if (value > (UINT64_MAX - digit) / UINT64_C(10)) { return -1; }
        value = value * UINT64_C(10) + digit;
    }
    if (value == 0) { return -1; }
    *job_id = value;
    return 0;
}

/* All pieces of the response share one deadline, including its variable data. */
static int receive_query_bytes(int fd, uint8_t *wire, size_t size, int64_t start, const char *name)
{
    int64_t now = faultline_monotonic_ms();
    if (now < 0 || now - start >= FAULTLINE_IO_TIMEOUT_MS ||
        faultline_recv_exact(fd, wire, size, FAULTLINE_IO_TIMEOUT_MS - (int)(now - start)) !=
        FAULTLINE_RECEIVE_OK) {
        fprintf(stderr, "faultline: %s response incomplete (connection closed, timed out, or read failed)\n", name);
        return -1;
    }
    return 0;
}

static int receive_query(int fd, uint16_t expected_type, const char *name, struct faultline_message *reply)
{
    uint8_t wire[FAULTLINE_MESSAGE_MAX_FRAME_SIZE];
    struct faultline_header header;
    size_t consumed, prefix, maximum;
    int64_t start = faultline_monotonic_ms();
    if (start < 0) { perror("faultline: clock"); return -1; }
    if (receive_query_bytes(fd, wire, FAULTLINE_HEADER_SIZE, start, name) < 0) { return -1; }
    if (faultline_header_decode(wire, FAULTLINE_HEADER_SIZE, &header) != FAULTLINE_PROTOCOL_OK) {
        fprintf(stderr, "faultline: invalid %s response header\n", name);
        return -1;
    }
    if (header.message_type != expected_type &&
        !(expected_type == FAULTLINE_MSG_JOB_STATUS_RESPONSE &&
          header.message_type == FAULTLINE_MSG_JOB_STATUS_NOT_FOUND)) {
        fprintf(stderr, "faultline: unexpected %s response type\n", name);
        return -1;
    }
    if (header.message_type == FAULTLINE_MSG_JOB_STATUS_NOT_FOUND) {
        prefix = FAULTLINE_JOB_STATUS_NOT_FOUND_PAYLOAD_SIZE;
        maximum = prefix;
    } else if (header.message_type == FAULTLINE_MSG_JOB_STATUS_RESPONSE) {
        prefix = FAULTLINE_JOB_STATUS_RESPONSE_PREFIX_SIZE;
        maximum = prefix + FAULTLINE_JOB_MAX_RESULT_SIZE;
    } else if (header.message_type == FAULTLINE_MSG_JOBS_RESPONSE) {
        prefix = FAULTLINE_JOBS_PREFIX_SIZE;
        maximum = prefix + FAULTLINE_JOBS_MAX_ENTRIES * FAULTLINE_JOB_SUMMARY_SIZE;
    } else {
        prefix = FAULTLINE_WORKERS_PREFIX_SIZE;
        maximum = prefix + FAULTLINE_WORKERS_MAX_ENTRIES * FAULTLINE_WORKER_SUMMARY_SIZE;
    }
    if (header.payload_length < prefix || header.payload_length > maximum) {
        fprintf(stderr, "faultline: invalid %s response length\n", name);
        return -1;
    }
    if (receive_query_bytes(fd, wire + FAULTLINE_HEADER_SIZE, prefix, start, name) < 0) { return -1; }
    size_t available = FAULTLINE_HEADER_SIZE + prefix;
    enum faultline_protocol_result result = faultline_message_decode(wire, available, reply, &consumed);
    if (result == FAULTLINE_PROTOCOL_BUFFER_TOO_SMALL) {
        /* Prefix fields (including bounded list counts) and lengths are valid. */
        size_t remaining = (size_t)header.payload_length - prefix;
        if (receive_query_bytes(fd, wire + available, remaining, start, name) < 0) { return -1; }
        available += remaining;
        result = faultline_message_decode(wire, available, reply, &consumed);
    }
    if (result != FAULTLINE_PROTOCOL_OK || consumed != available) {
        fprintf(stderr, "faultline: invalid %s response payload\n", name);
        return -1;
    }
    return 0;
}

static const char *status_state_name(uint16_t state)
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

static const char *status_failure_name(uint16_t failure)
{
    switch (failure) {
    case FAULTLINE_JOB_FAILURE_NONE: return "NONE";
    case FAULTLINE_JOB_FAILURE_TASK: return "TASK";
    case FAULTLINE_JOB_FAILURE_WORKER_LOST: return "WORKER_LOST";
    default: return "INVALID";
    }
}

static int print_status(const struct faultline_job_status_payload *snapshot)
{
    printf("job_id=%" PRIu64 "\nstate=%s\n", snapshot->job_id, status_state_name(snapshot->state));
    if (snapshot->worker_id == 0) { puts("worker_id=none"); }
    else { printf("worker_id=%" PRIu32 "\n", snapshot->worker_id); }
    printf("attempt=%" PRIu64 "\nretries=%" PRIu32 "/%" PRIu32 "\nfailure=%s\nresult_bytes=%zu\n",
           snapshot->attempt, snapshot->retry_count, snapshot->max_retries,
           status_failure_name(snapshot->failure), snapshot->result_size);
    if (snapshot->state == FAULTLINE_JOB_DONE) {
        fputs("result=\"", stdout);
        for (size_t i = 0; i < snapshot->result_size; ++i) {
            unsigned int byte = snapshot->result[i];
            if (byte >= 0x20 && byte <= 0x7e && byte != '"' && byte != '\\') {
                (void)putchar((int)byte);
            } else { printf("\\x%02x", byte); }
        }
        puts("\"");
    }
    if (fflush(stdout) == EOF || ferror(stdout)) { perror("faultline: write status"); return EXIT_FAILURE; }
    return EXIT_SUCCESS;
}

static int run_status(int argc, char **argv)
{
    char host[INET_ADDRSTRLEN] = FAULTLINE_DEFAULT_HOST;
    uint16_t port = FAULTLINE_DEFAULT_PORT;
    struct faultline_message request = {.message_type = FAULTLINE_MSG_JOB_STATUS_REQUEST}, reply;
    if ((argc != 3 && argc != 5) || parse_job_id(argv[2], &request.payload.job_status_request) < 0 ||
        (argc == 5 && (strcmp(argv[3], "--coordinator") != 0 ||
                      faultline_parse_endpoint(argv[4], host, sizeof(host), &port) < 0))) {
        usage(stderr);
        return EXIT_FAILURE;
    }
    uint8_t wire[FAULTLINE_HEADER_SIZE + FAULTLINE_JOB_STATUS_REQUEST_PAYLOAD_SIZE];
    size_t written;
    if (faultline_message_encode(wire, sizeof(wire), &request, &written) != FAULTLINE_PROTOCOL_OK) {
        fputs("faultline: could not encode status request\n", stderr);
        return EXIT_FAILURE;
    }
    if (faultline_ignore_sigpipe() < 0) { perror("faultline: configure SIGPIPE"); return EXIT_FAILURE; }
    int fd = faultline_connect(host, port, FAULTLINE_IO_TIMEOUT_MS);
    if (fd < 0) { perror("faultline: connect"); return EXIT_FAILURE; }
    int status = EXIT_FAILURE;
    if (faultline_send_all(fd, wire, written, FAULTLINE_IO_TIMEOUT_MS) < 0) {
        perror("faultline: send status request");
    } else if (receive_query(fd, FAULTLINE_MSG_JOB_STATUS_RESPONSE, "status", &reply) == 0) {
        uint64_t echoed = reply.message_type == FAULTLINE_MSG_JOB_STATUS_NOT_FOUND ?
                          reply.payload.job_status_not_found : reply.payload.job_status_response.job_id;
        if (echoed != request.payload.job_status_request) {
            fputs("faultline: status response job ID does not match request\n", stderr);
        } else if (reply.message_type == FAULTLINE_MSG_JOB_STATUS_NOT_FOUND) {
            fprintf(stderr, "faultline: job %" PRIu64 " not found\n", reply.payload.job_status_not_found);
            status = 2;
        } else { status = print_status(&reply.payload.job_status_response); }
    }
    (void)close(fd);
    return status;
}

static const char *task_name(uint16_t type)
{
    switch (type) {
    case FAULTLINE_TASK_SLEEP: return "sleep";
    case FAULTLINE_TASK_PRIME_COUNT: return "prime_count";
    case FAULTLINE_TASK_FIBONACCI: return "fibonacci";
    case FAULTLINE_TASK_HASH: return "hash";
    default: return "INVALID";
    }
}

static int print_listing(const struct faultline_message *reply)
{
    if (reply->message_type == FAULTLINE_MSG_JOBS_RESPONSE) {
        const struct faultline_jobs_payload *list = &reply->payload.jobs;
        printf("jobs=%zu\n", list->count);
        if (list->count == 0) { puts("No retained jobs."); }
        else { printf("%-20s %-11s %-8s %-10s %-10s %-21s %-11s %s\n",
                      "JOB_ID", "TASK", "STATE", "WORKER_ID", "ATTEMPT", "RETRIES", "FAILURE", "RESULT_BYTES"); }
        for (size_t i = 0; i < list->count; ++i) {
            const struct faultline_job_summary *entry = &list->entries[i];
            char worker[11] = "none", retries[22];
            if (entry->worker_id != 0) { (void)snprintf(worker, sizeof(worker), "%" PRIu32, entry->worker_id); }
            (void)snprintf(retries, sizeof(retries), "%" PRIu32 "/%" PRIu32, entry->retry_count, entry->max_retries);
            printf("%-20" PRIu64 " %-11s %-8s %-10s %-10" PRIu64 " %-21s %-11s %" PRIu32 "\n",
                   entry->job_id, task_name(entry->task_type), status_state_name(entry->state),
                   worker, entry->attempt, retries, status_failure_name(entry->failure), entry->result_size);
        }
    } else {
        const struct faultline_workers_payload *list = &reply->payload.workers;
        printf("workers=%zu heartbeat_timeout_ms=%" PRIu32 "\n", list->count, list->heartbeat_timeout_ms);
        if (list->count == 0) { puts("No retained worker registrations."); }
        else { printf("%-10s %-8s %-20s %-20s %s\n", "WORKER_ID", "LIVENESS", "HEARTBEAT_AGE_MS", "JOB_ID", "ATTEMPT"); }
        for (size_t i = 0; i < list->count; ++i) {
            const struct faultline_worker_summary *entry = &list->entries[i];
            const char *liveness = entry->state == FAULTLINE_WORKER_VIEW_DEAD ? "DEAD" :
                                  entry->heartbeat_age_ms >= list->heartbeat_timeout_ms ? "EXPIRED" : "ALIVE";
            char job[21] = "none";
            if (entry->job_id != 0) { (void)snprintf(job, sizeof(job), "%" PRIu64, entry->job_id); }
            printf("%-10" PRIu32 " %-8s %-20" PRIu64 " %-20s %" PRIu64 "\n",
                   entry->worker_id, liveness, entry->heartbeat_age_ms, job, entry->attempt);
        }
    }
    if (fflush(stdout) == EOF || ferror(stdout)) { perror("faultline: write listing"); return EXIT_FAILURE; }
    return EXIT_SUCCESS;
}

static int run_listing(int argc, char **argv)
{
    char host[INET_ADDRSTRLEN] = FAULTLINE_DEFAULT_HOST;
    uint16_t port = FAULTLINE_DEFAULT_PORT;
    int jobs = strcmp(argv[1], "jobs") == 0;
    if ((argc != 2 && argc != 4) ||
        (argc == 4 && (strcmp(argv[2], "--coordinator") != 0 ||
                      faultline_parse_endpoint(argv[3], host, sizeof(host), &port) < 0))) {
        usage(stderr);
        return EXIT_FAILURE;
    }
    struct faultline_message request = {.message_type = jobs ? FAULTLINE_MSG_JOBS_REQUEST : FAULTLINE_MSG_WORKERS_REQUEST};
    struct faultline_message reply;
    uint8_t wire[FAULTLINE_HEADER_SIZE];
    size_t written;
    if (faultline_message_encode(wire, sizeof(wire), &request, &written) != FAULTLINE_PROTOCOL_OK) {
        fputs("faultline: could not encode listing request\n", stderr);
        return EXIT_FAILURE;
    }
    if (faultline_ignore_sigpipe() < 0) { perror("faultline: configure SIGPIPE"); return EXIT_FAILURE; }
    int fd = faultline_connect(host, port, FAULTLINE_IO_TIMEOUT_MS);
    if (fd < 0) { perror("faultline: connect"); return EXIT_FAILURE; }
    int status = EXIT_FAILURE;
    if (faultline_send_all(fd, wire, written, FAULTLINE_IO_TIMEOUT_MS) < 0) {
        perror("faultline: send listing request");
    } else if (receive_query(fd, jobs ? FAULTLINE_MSG_JOBS_RESPONSE : FAULTLINE_MSG_WORKERS_RESPONSE,
                             argv[1], &reply) == 0) {
        status = print_listing(&reply);
    }
    (void)close(fd);
    return status;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "submit") == 0) { return run_submit(argc, argv); }
    if (argc >= 2 && strcmp(argv[1], "status") == 0) { return run_status(argc, argv); }
    if (argc >= 2 && (strcmp(argv[1], "jobs") == 0 || strcmp(argv[1], "workers") == 0)) {
        return run_listing(argc, argv);
    }
    return run_ping(argc, argv);
}
