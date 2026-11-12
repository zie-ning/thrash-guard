/*
 * scanner.h - 후보 cgroup 표를 주기적으로 갱신하는 스캔 스레드
 *
 * 디렉터리를 읽는 일은 스래싱 중에 블록될 수 있다. 메인 루프가 거기서 멈추면 감지 자체를
 * 못 하므로, 스캔은 별도 스레드가 맡고 메인 루프는 완성된 표만 읽는다.
 *
 * 표는 두 벌이다. 스캔 스레드는 현재 표가 아닌 쪽에 새 표를 만든 뒤 한 번의 원자적 교체로
 * 공개한다. 메인 루프는 tg_scanner_acquire로 표를 잡고 쓴 뒤 tg_scanner_release로 놓는다.
 * 메인이 표를 잡고 있는 동안에는 스캔 스레드가 그 표를 건드리지 않는다(그 회차는 건너뛴다).
 * 어느 쪽도 락을 잡고 기다리지 않는다.
 */
#ifndef TG_SCANNER_H
#define TG_SCANNER_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "targets.h"

#define TG_SCANNER_STACK_BYTES (64 * 1024)

struct tg_scanner {
    struct tg_targets tbl[2];
    atomic_int current; /* 공개된 표의 번호 */
    atomic_int in_use;  /* 메인이 잡고 있는 표의 번호. 없으면 -1 */
    atomic_bool stop;
    atomic_uint scans;       /* 성공한 스캔 횟수 (통계) */
    atomic_uint scan_errors; /* 실패한 스캔 횟수 (통계) */
    pthread_t thread;
    bool started;
    char parent[TG_CFG_PATH_CAP];
    uint32_t interval_ms;
};

/*
 * 첫 스캔을 동기로 끝낸 뒤 스레드를 시작한다. 이때 발견한 항목은 first_seen_ns=0(유예 면제).
 * 첫 스캔이 실패하면 스레드를 만들지 않고 음수 errno를 돌려준다.
 */
int tg_scanner_start(struct tg_scanner *s, const char *parent, uint32_t interval_ms);

/* 공개된 표를 잡는다. 반드시 tg_scanner_release로 놓아야 한다. */
const struct tg_targets *tg_scanner_acquire(struct tg_scanner *s);
void tg_scanner_release(struct tg_scanner *s);

/* 스레드를 멈추고 합치고 모든 fd를 닫는다. 여러 번 불러도 안전하다. */
void tg_scanner_stop(struct tg_scanner *s);

#endif /* TG_SCANNER_H */
