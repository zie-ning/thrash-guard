/*
 * log.h - 로그 링버퍼와 기록 스레드
 *
 * 메인 루프는 고정 크기 링버퍼에 줄을 복사하기만 하고, 별도 기록 스레드가 표준 오류나
 * 파일에 쓴다. 스래싱 중 디스크 쓰기가 막혀도 판단 루프가 멈추지 않게 하려는 것이다.
 * 설계와 근거는 docs/decisions/log-ringbuffer.md.
 *
 * 규칙:
 *   - 로그를 쓰는 스레드는 정확히 하나여야 한다(보통 메인 루프). 락 없는 구조의 전제다.
 *   - tg_log_begin과 tg_log_end는 짝으로 부르고 중첩하지 않는다.
 *   - tg_log_stop은 로그를 쓰던 스레드가 쓰기를 끝낸 뒤에 부른다.
 *   - 초기화(tg_log_open/start) 뒤에는 힙 할당을 하지 않는다. 기록 스레드도 stdio를 쓰지 않는다.
 */
#ifndef TG_LOG_H
#define TG_LOG_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "fmt.h"

#define TG_LOG_SLOT_BYTES 256
#define TG_LOG_TEXT_CAP 244 /* tg_buf 규약(NUL 자리 포함)이라 본문은 최대 243바이트 */
#define TG_LOG_BATCH_BYTES 4096
#define TG_LOG_LINE_MAX 300 /* 출력 한 줄의 최대 길이(접두어, 시각, 레벨 포함) */
#define TG_LOG_STACK_BYTES (64 * 1024)
#define TG_LOG_TS_LEN 24 /* "2026-11-11T15:04:05.123Z" */

enum tg_log_level {
    TG_LOG_ERR = 0,
    TG_LOG_WARN = 1,
    TG_LOG_INFO = 2,
    TG_LOG_DEBUG = 3,
};

struct tg_log_slot {
    uint64_t ts_ns; /* 쓴 시각(CLOCK_MONOTONIC) */
    uint16_t len;
    uint8_t level;
    uint8_t reserved;
    char text[TG_LOG_TEXT_CAP];
};

_Static_assert(sizeof(struct tg_log_slot) == TG_LOG_SLOT_BYTES, "슬롯은 256바이트여야 한다");

/* 출력 대상. 성공하면 0, 실패하면 음수 errno. 부분 쓰기는 구현이 이어서 처리한다. */
typedef int (*tg_log_sink_fn)(void *ctx, const char *buf, size_t len);

struct tg_log_opts {
    uint32_t nslots;
    tg_log_sink_fn sink;
    void *sink_ctx;
    bool journald_prefix; /* 줄 앞에 "<3>" 같은 syslog 우선순위 접두어를 붙인다 */
};

struct tg_log {
    struct tg_log_slot *ring;
    uint32_t nslots;

    _Atomic uint64_t head;         /* 지금까지 쓴 줄 수. 생산자만 쓴다 */
    _Atomic uint64_t tail;         /* 지금까지 꺼낸 줄 수. 소비자만 쓴다 */
    _Atomic uint64_t dropped;      /* 가득 차서 버린 줄 수. 생산자가 올리고 소비자가 0으로 되돌린다 */
    _Atomic uint64_t write_errors; /* 출력 대상 쓰기 실패 횟수 */
    atomic_bool stop;

    int efd; /* 소비자를 깨우는 eventfd */
    tg_log_sink_fn sink;
    void *sink_ctx;
    bool journald_prefix;

    int owned_fd;    /* tg_log_start가 연 로그 파일 fd. 없으면 -1 */
    bool fell_back;  /* log_path를 열지 못해 표준 오류로 대체했다 */
    int64_t real_minus_mono_ns;

    pthread_t thread;
    bool started;
};

/* 링버퍼를 만들고 기록 스레드를 시작한다. 실패하면 음수 errno이고 아무것도 남지 않는다. */
int tg_log_open(struct tg_log *l, const struct tg_log_opts *o);

/*
 * 설정에 따라 열기: log_path가 비어 있으면 표준 오류, 아니면 그 파일(이어 쓰기).
 * 파일을 열 수 없으면 표준 오류로 대체하고 경고 줄을 남긴다(0을 돌려준다).
 */
int tg_log_start(struct tg_log *l, const struct tg_config *cfg);

/* 남은 줄을 모두 내보낸 뒤 스레드를 합치고 자원을 닫는다. 여러 번 불러도 안전하다. */
void tg_log_stop(struct tg_log *l);

/*
 * 줄 하나를 쓰기 시작한다. 가득 차 있으면 버린 개수를 올리고 false를 돌려준다(이때 b는 건드리지 않는다).
 * true이면 b가 슬롯의 본문 영역을 가리키도록 초기화되며, tg_buf_*로 조립한 뒤 tg_log_end를 부른다.
 */
bool tg_log_begin(struct tg_log *l, enum tg_log_level lv, struct tg_buf *b);

/* 조립한 줄을 공개한다: 길이·시각 기록, head 증가, 기록 스레드 깨우기. */
void tg_log_end(struct tg_log *l, const struct tg_buf *b);

/* unix 나노초를 "YYYY-MM-DDTHH:MM:SS.mmmZ" 24자로 쓴다. out은 NUL 자리를 포함해 25바이트 이상이어야 한다. */
size_t tg_log_fmt_time(char *out, uint64_t unix_ns);

#endif /* TG_LOG_H */
