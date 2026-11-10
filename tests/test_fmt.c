/*
 * test_fmt.c - 할당 없는 문자열 조립기 테스트
 */
#include "fmt.h"
#include "tg_test.h"

#include <string.h>

static void test_basic(void)
{
    char mem[64];
    struct tg_buf b;

    tg_buf_init(&b, mem, sizeof(mem));
    tg_buf_str(&b, "tg1");
    tg_buf_kv_u64(&b, "seq", 0);
    tg_buf_kv_u64(&b, "max", UINT64_MAX);
    tg_buf_kv_i64(&b, "neg", -42);
    TG_CHECK(!b.overflow);
    TG_CHECK(strcmp(mem, "tg1 seq=0 max=18446744073709551615 neg=-42") == 0);
    TG_CHECK_EQ_U64(b.len, strlen(mem));
}

static void test_int64_min(void)
{
    char mem[32];
    struct tg_buf b;

    tg_buf_init(&b, mem, sizeof(mem));
    tg_buf_i64(&b, INT64_MIN);
    TG_CHECK(strcmp(mem, "-9223372036854775808") == 0);
}

static void test_overflow(void)
{
    char mem[8];
    struct tg_buf b;

    tg_buf_init(&b, mem, sizeof(mem));
    tg_buf_str(&b, "1234567890");
    TG_CHECK(b.overflow);
    TG_CHECK_EQ_U64(b.len, 7); /* NUL 자리를 남긴다 */
    TG_CHECK(mem[7] == '\0');

    /* 넘친 뒤의 쓰기는 버린다. */
    tg_buf_char(&b, 'x');
    TG_CHECK_EQ_U64(b.len, 7);
}

int main(void)
{
    TG_RUN(test_basic);
    TG_RUN(test_int64_min);
    TG_RUN(test_overflow);
    return tg_test_finish();
}
