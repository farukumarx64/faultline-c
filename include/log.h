#ifndef FAULTLINE_LOG_H
#define FAULTLINE_LOG_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Trusted, constant level/component/event/format strings; escape external bytes
 * before inserting them into quoted fields. Each call emits one flushed line:
 * UTC [LEVEL] component event fields pid=N monotonic_ms=N.
 * Logging is best effort, preserves errno, and never supplies WAL durability.
 * Returns 0 on success, -1 on invalid arguments or output failure.
 * Do not call from signal handlers. NULL format means no additional fields. */
int faultline_log(FILE *stream, const char *level, const char *component,
                  const char *event, const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 5, 6)))
#endif
    ;

/* Printable ASCII except quotes/backslashes is copied; other bytes use \xHH.
 * Adds a NUL terminator, but no surrounding quotes. NULL bytes is allowed for
 * size zero. Buffers must not overlap. On failure, output is unchanged. */
int faultline_log_escape(char *output, size_t capacity, const uint8_t *bytes, size_t size);

/* system_error on stderr, with an explicit errno or pthread error code.
 * level/component/operation are trusted constants. Preserves the caller's errno. */
void faultline_log_error(const char *level, const char *component, const char *operation, int error);

#endif
