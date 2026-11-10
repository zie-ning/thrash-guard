/*
 * fmt.h - 할당 없는 문자열 조립기
 *
 * snprintf 대신 쓰는 최소 기능. 고정 버퍼에 문자열과 부호 없는 정수를 이어 붙인다.
 * 버퍼가 모자라면 overflow 플래그를 세우고 그 뒤 쓰기는 버린다.
 */
#ifndef TG_FMT_H
#define TG_FMT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tg_buf {
    char *mem;
    size_t len;
    size_t cap; /* 마지막 NUL 자리를 포함한 크기 */
    bool overflow;
};

void tg_buf_init(struct tg_buf *b, char *mem, size_t cap);
void tg_buf_str(struct tg_buf *b, const char *s);
void tg_buf_char(struct tg_buf *b, char c);
void tg_buf_u64(struct tg_buf *b, uint64_t v);
void tg_buf_i64(struct tg_buf *b, int64_t v);

/* 버퍼가 비어 있지 않으면 공백을 하나 넣은 뒤 "key=value"를 붙인다. */
void tg_buf_kv_u64(struct tg_buf *b, const char *key, uint64_t v);
void tg_buf_kv_i64(struct tg_buf *b, const char *key, int64_t v);

#endif /* TG_FMT_H */
