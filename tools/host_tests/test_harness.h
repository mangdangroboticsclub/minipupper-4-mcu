/* Minimal zero-dependency test harness for the MPX host tests.
 *
 * Why not Unity: these tests have to run on a laptop with no network and no
 * ESP-IDF, in the same CI job as the Python vector checks. A vendored
 * framework is one more thing to pin and audit for no benefit at this size.
 *
 * A test is a static function registered at load time, so adding a file is
 * the whole of adding a test -- no list to keep in sync.
 *
 *     TEST_CASE(id_accepts_valid) {
 *         CHECK(is_valid("MPX-AJZ7J8Y0YTDS"));
 *     }
 *
 * Assertions abort the current test (return) and count as a failure; the rest
 * of the suite still runs, so one break reports all the damage at once.
 */
#ifndef MPX_TEST_HARNESS_H
#define MPX_TEST_HARNESS_H

#include <stddef.h>

typedef void (*mpx_test_fn)(void);

void mpx_test_register(const char *name, mpx_test_fn fn);

/* Records a failure against the current test. Called by the macros below. */
void mpx_test_fail(const char *file, int line, const char *fmt, ...);

/* Runs every registered test. Returns 0 when all passed. */
int mpx_test_run_all(void);

#define TEST_CASE(fn_name)                                                     \
    static void fn_name(void);                                                 \
    __attribute__((constructor)) static void fn_name##_register(void)          \
    {                                                                          \
        mpx_test_register(#fn_name, fn_name);                                  \
    }                                                                          \
    static void fn_name(void)

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            mpx_test_fail(__FILE__, __LINE__, "CHECK(%s)", #cond);             \
            return;                                                            \
        }                                                                      \
    } while (0)

#define CHECK_MSG(cond, ...)                                                   \
    do {                                                                       \
        if (!(cond)) {                                                         \
            mpx_test_fail(__FILE__, __LINE__, __VA_ARGS__);                    \
            return;                                                            \
        }                                                                      \
    } while (0)

#define CHECK_EQ_INT(actual, expected)                                         \
    do {                                                                       \
        long long a_ = (long long)(actual), e_ = (long long)(expected);        \
        if (a_ != e_) {                                                        \
            mpx_test_fail(__FILE__, __LINE__,                                  \
                          "%s: got %lld, expected %lld", #actual, a_, e_);     \
            return;                                                            \
        }                                                                      \
    } while (0)

#define CHECK_EQ_STR(actual, expected)                                         \
    do {                                                                       \
        const char *a_ = (actual), *e_ = (expected);                           \
        if (a_ == NULL || e_ == NULL || mpx_test_strcmp(a_, e_) != 0) {        \
            mpx_test_fail(__FILE__, __LINE__,                                  \
                          "%s: got \"%s\", expected \"%s\"",                   \
                          #actual, a_ ? a_ : "(null)", e_ ? e_ : "(null)");    \
            return;                                                            \
        }                                                                      \
    } while (0)

/* Byte-wise comparison with a hex dump of both sides on failure. */
#define CHECK_EQ_MEM(actual, expected, len)                                    \
    do {                                                                       \
        if (!mpx_test_mem_eq((const void *)(actual), (const void *)(expected),  \
                             (size_t)(len))) {                                  \
            mpx_test_fail_mem(__FILE__, __LINE__, #actual, #expected,           \
                              (const unsigned char *)(actual),                  \
                              (const unsigned char *)(expected), (size_t)(len));\
            return;                                                            \
        }                                                                      \
    } while (0)

/* Helpers used by the macros above; declared here so tests do not need
 * another include. */
int mpx_test_strcmp(const char *a, const char *b);
int mpx_test_mem_eq(const void *a, const void *b, size_t len);
void mpx_test_fail_mem(const char *file, int line, const char *a_name,
                       const char *e_name, const unsigned char *a,
                       const unsigned char *e, size_t len);

#endif /* MPX_TEST_HARNESS_H */
