/*
 * targets.h - 종료 후보 cgroup 표
 *
 * target_parent의 직계 자식 디렉터리 중 memory.pressure와 memory.stat를 열 수 있는 것을
 * 후보로 삼아 고정 크기 표에 담는다. 항목마다 두 파일의 fd를 열어 둔 채 보관해서
 * 판단 루프가 경로 탐색 없이 pread로 읽을 수 있게 한다.
 *
 * 표를 만드는 쪽(스캔 스레드)과 읽는 쪽(메인 루프)이 이중 버퍼로 나뉜다. 두 표는 같은 항목의
 * fd를 공유하므로, 닫는 책임은 tg_targets_release 한 곳에 모아 둔다.
 *
 * 힙 할당을 하지 않는다. 디렉터리는 getdents64 시스템 콜과 스택 버퍼로 읽는다.
 */
#ifndef TG_TARGETS_H
#define TG_TARGETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TG_MAX_TARGETS 32
#define TG_TARGET_NAME_CAP 128 /* Docker scope 이름(약 80자)이 들어가도록 넉넉히 */

struct tg_target {
    char name[TG_TARGET_NAME_CAP]; /* target_parent 아래 디렉터리 이름 */
    int pressure_fd;               /* memory.pressure */
    int stat_fd;                   /* memory.stat */
    uint64_t first_seen_ns;        /* 처음 발견한 단조 시각. 데몬 시작 때 있던 항목은 0 */
};

struct tg_targets {
    struct tg_target t[TG_MAX_TARGETS];
    uint32_t n;
    uint32_t skipped; /* 표가 가득 차서 빠진 후보 수 */
};

void tg_targets_init(struct tg_targets *t);

/*
 * t의 항목 중 keep에 같은 이름·같은 fd로 살아 있지 않은 것의 fd를 닫고 t를 비운다.
 * keep이 NULL이면 전부 닫는다. 이중 버퍼에서 한쪽 표를 버릴 때 쓴다.
 */
void tg_targets_release(struct tg_targets *t, const struct tg_targets *keep);

/*
 * parent를 다시 읽어 next를 새로 만든다.
 *   - cur에 이미 있는 항목은 fd와 first_seen_ns를 그대로 가져온다(fd 공유).
 *   - 새 항목은 fd를 열고 first_seen_ns를 initial ? 0 : now_ns로 둔다.
 *   - next의 이전 내용 중 cur에 없는 항목의 fd는 먼저 닫는다.
 * next와 cur는 서로 다른 표여야 한다. cur가 NULL이면 처음 만드는 것이다.
 * 실패하면 next는 비워지고 새로 연 fd는 닫힌다. cur는 건드리지 않는다.
 */
int tg_targets_rebuild(struct tg_targets *next, const struct tg_targets *cur, const char *parent,
                       uint64_t now_ns, bool initial);

/* 이름으로 찾는다. 없으면 NULL. */
const struct tg_target *tg_targets_find(const struct tg_targets *t, const char *name);

#endif /* TG_TARGETS_H */
