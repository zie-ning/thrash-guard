/*
 * fmt.c - 할당 없는 문자열 조립기 구현
 *
 * 핵심 경로: 할당 금지.
 */
#include "fmt.h"

void tg_buf_init(struct tg_buf *b, char *mem, size_t cap)
{
    b->mem = mem;
    b->len = 0;
    b->cap = cap;
    b->overflow = cap == 0;
    if (cap > 0)
        mem[0] = '\0';
}

void tg_buf_char(struct tg_buf *b, char c)
{
    if (b->overflow)
        return;
    if (b->len + 1 >= b->cap) {
        b->overflow = true;
        return;
    }
    b->mem[b->len++] = c;
    b->mem[b->len] = '\0';
}

void tg_buf_str(struct tg_buf *b, const char *s)
{
    while (*s != '\0' && !b->overflow)
        tg_buf_char(b, *s++);
}

void tg_buf_u64(struct tg_buf *b, uint64_t v)
{
    char tmp[20]; /* UINT64_MAX는 20자리 */
    int n = 0;

    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    while (n > 0)
        tg_buf_char(b, tmp[--n]);
}

void tg_buf_i64(struct tg_buf *b, int64_t v)
{
    if (v < 0) {
        tg_buf_char(b, '-');
        /* INT64_MIN도 안전하게 변환한다. */
        tg_buf_u64(b, (uint64_t)(-(v + 1)) + 1);
    } else {
        tg_buf_u64(b, (uint64_t)v);
    }
}

static void kv_prefix(struct tg_buf *b, const char *key)
{
    if (b->len > 0)
        tg_buf_char(b, ' ');
    tg_buf_str(b, key);
    tg_buf_char(b, '=');
}

void tg_buf_kv_u64(struct tg_buf *b, const char *key, uint64_t v)
{
    kv_prefix(b, key);
    tg_buf_u64(b, v);
}

void tg_buf_kv_i64(struct tg_buf *b, const char *key, int64_t v)
{
    kv_prefix(b, key);
    tg_buf_i64(b, v);
}
