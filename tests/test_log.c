#include "log.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return EXIT_FAILURE; } } while (0)

static int test_escaping(void)
{
    const uint8_t bytes[] = {'A', 0, '\n', '\r', 0x1b, '"', '\\', 0xff, ' '};
    const char expected[] = "A\\x00\\x0a\\x0d\\x1b\\x22\\x5c\\xff ";
    char output[sizeof(expected) + 1], before[sizeof(output)];
    memset(output, '!', sizeof(output)); memcpy(before, output, sizeof(output));
    CHECK(faultline_log_escape(output, sizeof(expected) - 1, bytes, sizeof(bytes)) == -1);
    CHECK(memcmp(output, before, sizeof(output)) == 0);
    CHECK(faultline_log_escape(output, sizeof(expected), bytes, sizeof(bytes)) == 0);
    CHECK(strcmp(output, expected) == 0 && output[sizeof(expected)] == '!');
    CHECK(faultline_log_escape(output, 1, NULL, 0) == 0 && output[0] == 0);
    CHECK(faultline_log_escape(NULL, 1, NULL, 0) == -1);
    CHECK(faultline_log_escape(output, sizeof(output), NULL, 1) == -1);
    CHECK(faultline_log_escape(output, sizeof(output), bytes, SIZE_MAX) == -1);
    uint8_t all[256]; char encoded[1025];
    for (size_t i = 0; i < sizeof(all); ++i) { all[i] = (uint8_t)i; }
    CHECK(faultline_log_escape(encoded, sizeof(encoded), all, sizeof(all)) == 0);
    CHECK(strchr(encoded, '\n') == NULL && strchr(encoded, '\r') == NULL && strchr(encoded, '"') == NULL);
    CHECK(strstr(encoded, "\\x1b") != NULL && strstr(encoded, "\\xff") != NULL);
    return 0;
}

static int test_format_and_errno(void)
{
    FILE *stream = tmpfile();
    CHECK(stream != NULL);
    time_t before = time(NULL);
    errno = EDOM;
    CHECK(faultline_log(stream, "WARN", "test", "example", "job_id=%d value=\"%s\"", 7, "hello world") == 0);
    CHECK(errno == EDOM);
    time_t after = time(NULL);
    CHECK(faultline_log(stream, "INFO", "test", "empty", NULL) == 0);
    rewind(stream);
    char line[512], date[32], expected_before[32], expected_after[32];
    CHECK(fgets(line, sizeof(line), stream) != NULL);
    CHECK(strlen(line) > 24 && line[23] == 'Z' && line[24] == ' ');
    CHECK(sscanf(line, "%19s", date) == 1);
    struct tm utc;
    CHECK(gmtime_r(&before, &utc) != NULL);
    CHECK(strftime(expected_before, sizeof(expected_before), "%Y-%m-%dT%H:%M:%S", &utc) != 0);
    CHECK(gmtime_r(&after, &utc) != NULL);
    CHECK(strftime(expected_after, sizeof(expected_after), "%Y-%m-%dT%H:%M:%S", &utc) != 0);
    CHECK(strcmp(date, expected_before) == 0 || strcmp(date, expected_after) == 0);
    CHECK(strstr(line, " [WARN] test example job_id=7 value=\"hello world\" pid=") != NULL);
    long pid; int64_t monotonic;
    char *metadata = strstr(line, " pid=");
    CHECK(metadata != NULL && sscanf(metadata, " pid=%ld monotonic_ms=%" SCNd64, &pid, &monotonic) == 2);
    CHECK(pid == (long)getpid() && monotonic >= 0);
    CHECK(fgets(line, sizeof(line), stream) != NULL && strstr(line, " [INFO] test empty pid=") != NULL);
    CHECK(fgets(line, sizeof(line), stream) == NULL);
    CHECK(fclose(stream) == 0);
    return 0;
}

static int test_output_failure(void)
{
    FILE *read_only = fopen("/dev/null", "r");
    CHECK(read_only != NULL);
    errno = ERANGE;
    CHECK(faultline_log(read_only, "ERROR", "test", "write_failure", NULL) == -1);
    CHECK(errno == ERANGE);
    CHECK(fclose(read_only) == 0);
    CHECK(faultline_log(NULL, "INFO", "test", "invalid", NULL) == -1);
    return 0;
}

int main(void)
{
    int (*tests[])(void) = {test_escaping, test_format_and_errno, test_output_failure};
    const char *names[] = {"log binary escaping", "log UTC metadata and errno", "log output failure"};
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (tests[i]() != 0) { return EXIT_FAILURE; }
        printf("PASS: %s\n", names[i]);
    }
    return EXIT_SUCCESS;
}
