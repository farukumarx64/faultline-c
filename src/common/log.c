#include "log.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int printable(uint8_t byte)
{
    return byte >= 0x20 && byte <= 0x7e && byte != '"' && byte != '\\';
}

int faultline_log_escape(char *output, size_t capacity, const uint8_t *bytes, size_t size)
{
    if (output == NULL || (bytes == NULL && size != 0) || size > (SIZE_MAX - 1) / 4) { return -1; }
    size_t needed = 1;
    for (size_t i = 0; i < size; ++i) { needed += printable(bytes[i]) ? 1u : 4u; }
    if (needed > capacity) { return -1; }
    const char hex[] = "0123456789abcdef";
    size_t position = 0;
    for (size_t i = 0; i < size; ++i) {
        uint8_t byte = bytes[i];
        if (printable(byte)) { output[position++] = (char)byte; }
        else {
            output[position++] = '\\'; output[position++] = 'x';
            output[position++] = hex[byte >> 4]; output[position++] = hex[byte & 15u];
        }
    }
    output[position] = '\0';
    return 0;
}

int faultline_log(FILE *stream, const char *level, const char *component,
                  const char *event, const char *format, ...)
{
    int saved_errno = errno;
    if (stream == NULL || level == NULL || component == NULL || event == NULL) { return -1; }
    char timestamp[48] = "time=unavailable", date[32];
    struct timespec wall, monotonic;
    struct tm utc;
    if (clock_gettime(CLOCK_REALTIME, &wall) == 0 && gmtime_r(&wall.tv_sec, &utc) != NULL &&
        strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &utc) != 0) {
        (void)snprintf(timestamp, sizeof(timestamp), "%s.%03ldZ", date, wall.tv_nsec / 1000000L);
    }
    int64_t monotonic_ms = -1;
    if (clock_gettime(CLOCK_MONOTONIC, &monotonic) == 0 && monotonic.tv_sec >= 0 &&
        monotonic.tv_sec <= (INT64_MAX - 999) / 1000) {
        monotonic_ms = (int64_t)monotonic.tv_sec * 1000 + monotonic.tv_nsec / 1000000;
    }
    /* Protect the complete record if another thread uses the same FILE. */
    flockfile(stream);
    int failed = fprintf(stream, "%s [%s] %s %s", timestamp, level, component, event) < 0;
    if (format != NULL && format[0] != '\0') {
        if (fputc(' ', stream) == EOF) { failed = 1; }
        va_list arguments;
        va_start(arguments, format);
        if (vfprintf(stream, format, arguments) < 0) { failed = 1; }
        va_end(arguments);
    }
    if (fprintf(stream, " pid=%ld monotonic_ms=%" PRId64 "\n", (long)getpid(), monotonic_ms) < 0 ||
        fflush(stream) == EOF || ferror(stream)) { failed = 1; }
    funlockfile(stream);
    errno = saved_errno;
    return failed ? -1 : 0;
}

void faultline_log_error(const char *level, const char *component, const char *operation, int error)
{
    int saved_errno = errno;
    char message[256], escaped[1025];
    if (strerror_r(error, message, sizeof(message)) != 0) {
        (void)snprintf(message, sizeof(message), "error description unavailable");
    }
    if (faultline_log_escape(escaped, sizeof(escaped), (const uint8_t *)message, strlen(message)) < 0) {
        (void)snprintf(escaped, sizeof(escaped), "error description unavailable");
    }
    (void)faultline_log(stderr, level, component, "system_error",
                        "operation=\"%s\" errno=%d message=\"%s: %s: %s\"",
                        operation, error, component, operation, escaped);
    errno = saved_errno;
}
