/*
 * protect.h - 보호된 cgroup 판정
 *
 * cgroup.kill은 그 cgroup과 하위 cgroup의 모든 프로세스를 죽인다. 그래서 후보 안(하위 포함)에
 * oom_score_adj가 -1000인 프로세스가 하나라도 있으면 그 cgroup을 종료 후보에서 뺀다.
 * 커널 OOM killer가 -1000을 절대 고르지 않는 것과 같은 기준이다.
 *
 * 확인하지 못하면 보호된 것으로 본다(안전한 쪽으로 실패). 확인하지 못하는 경우는 cgroup.procs나
 * 하위 폴더를 열 수 없을 때, 프로세스 값을 읽을 수 없을 때(프로세스가 그 사이 사라진 경우는 제외),
 * 하위 cgroup이 깊이 상한을 넘을 때다.
 *
 * 스캔 스레드에서 도는 코드다. 힙 할당과 stdio를 쓰지 않는다. 스캔 스레드의 스택이 64 KiB라
 * 단계마다 쓰는 버퍼(약 2 KiB)와 깊이 상한을 맞춰 두었다.
 */
#ifndef TG_PROTECT_H
#define TG_PROTECT_H

#include <stdint.h>

#include "targets.h"

#define TG_PROTECT_MAX_DEPTH 8 /* 후보 아래로 따라 내려가는 최대 단계 (임시) */

enum tg_protect_state {
    TG_PROTECT_NO = 0,      /* 보호 프로세스가 없다 */
    TG_PROTECT_YES = 1,     /* oom_score_adj가 -1000인 프로세스가 있다 */
    TG_PROTECT_UNKNOWN = 2, /* 끝까지 확인하지 못했다. 보호된 것으로 다룬다 */
};

/*
 * parent_fd 아래 name cgroup을 확인한다. proc_fd는 /proc 디렉터리(테스트에서는 가짜 폴더)다.
 */
int tg_protect_check(int proc_fd, int parent_fd, const char *name);

/*
 * 표의 모든 항목에 보호 상태를 다시 계산해 적고 갱신 시각(scanned_ns)을 기록한다.
 * 재사용하는 항목도 매번 다시 계산한다(보호 상태가 바뀔 수 있다). parent를 열지 못하면 음수 errno
 * 이고 표는 건드리지 않은 것으로 본다(호출자가 공개하지 않는다).
 */
int tg_targets_mark(struct tg_targets *t, int proc_fd, const char *parent, uint64_t now_ns);

#endif /* TG_PROTECT_H */
