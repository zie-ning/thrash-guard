/*
 * tg_test.h - 최소 단위 테스트 도구
 *
 * 사용법:
 *   static void test_foo(void) { TG_CHECK(1 + 1 == 2); TG_CHECK_EQ_U64(x, 3); }
 *   int main(void) { TG_RUN(test_foo); return tg_test_finish(); }
 */
#ifndef TG_TEST_H
#define TG_TEST_H

#include <inttypes.h>
#include <stdio.h>

static int tg_test_failures;
static int tg_test_checks;

#define TG_CHECK(cond)                                                          \
    do {                                                                        \
        tg_test_checks++;                                                       \
        if (!(cond)) {                                                          \
            tg_test_failures++;                                                 \
            fprintf(stderr, "  실패 %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                       \
    } while (0)

#define TG_CHECK_EQ_U64(actual, expected)                                       \
    do {                                                                        \
        uint64_t tg_a_ = (uint64_t)(actual);                                    \
        uint64_t tg_e_ = (uint64_t)(expected);                                  \
        tg_test_checks++;                                                       \
        if (tg_a_ != tg_e_) {                                                   \
            tg_test_failures++;                                                 \
            fprintf(stderr, "  실패 %s:%d: %s == %" PRIu64 ", 기대값 %" PRIu64 "\n", \
                    __FILE__, __LINE__, #actual, tg_a_, tg_e_);                 \
        }                                                                       \
    } while (0)

#define TG_RUN(fn)                                                              \
    do {                                                                        \
        int tg_before_ = tg_test_failures;                                      \
        fn();                                                                   \
        printf("%s %s\n", tg_test_failures == tg_before_ ? "통과" : "실패", #fn); \
    } while (0)

static inline int tg_test_finish(void)
{
    printf("검사 %d개, 실패 %d개\n", tg_test_checks, tg_test_failures);
    return tg_test_failures == 0 ? 0 : 1;
}

#endif /* TG_TEST_H */
