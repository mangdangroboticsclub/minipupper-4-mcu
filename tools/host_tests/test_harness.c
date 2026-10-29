#include "test_harness.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MPX_MAX_TESTS 512

typedef struct {
    const char *name;
    mpx_test_fn fn;
} mpx_test_entry;

static mpx_test_entry s_tests[MPX_MAX_TESTS];
static size_t s_test_count;
static int s_current_failed;

void mpx_test_register(const char *name, mpx_test_fn fn)
{
    if (s_test_count >= MPX_MAX_TESTS) {
        fprintf(stderr, "test harness: more than %d tests, raise MPX_MAX_TESTS\n",
                MPX_MAX_TESTS);
        return;
    }
    s_tests[s_test_count].name = name;
    s_tests[s_test_count].fn = fn;
    s_test_count++;
}

void mpx_test_fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    s_current_failed = 1;
    fprintf(stderr, "  %s:%d: ", file, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

int mpx_test_strcmp(const char *a, const char *b)
{
    return strcmp(a, b);
}

int mpx_test_mem_eq(const void *a, const void *b, size_t len)
{
    return memcmp(a, b, len) == 0;
}

void mpx_test_fail_mem(const char *file, int line, const char *a_name,
                       const char *e_name, const unsigned char *a,
                       const unsigned char *e, size_t len)
{
    size_t i;
    s_current_failed = 1;
    fprintf(stderr, "  %s:%d: %s != %s (%zu bytes)\n", file, line, a_name, e_name, len);
    fprintf(stderr, "    got      ");
    for (i = 0; i < len; i++) {
        fprintf(stderr, "%02x", a[i]);
    }
    fprintf(stderr, "\n    expected ");
    for (i = 0; i < len; i++) {
        fprintf(stderr, "%02x", e[i]);
    }
    fputc('\n', stderr);
}

int mpx_test_run_all(void)
{
    size_t i;
    int failed_tests = 0;

    if (s_test_count == 0) {
        fprintf(stderr, "test harness: no tests registered\n");
        return 1;
    }

    for (i = 0; i < s_test_count; i++) {
        s_current_failed = 0;
        s_tests[i].fn();
        if (s_current_failed) {
            failed_tests++;
            printf("FAIL  %s\n", s_tests[i].name);
        } else {
            printf("ok    %s\n", s_tests[i].name);
        }
    }

    printf("\n%zu tests, %d failed\n", s_test_count, failed_tests);
    return failed_tests == 0 ? 0 : 1;
}
