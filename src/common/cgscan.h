/*
 * cgscan.h - 감시 대상 cgroup 표
 *
 * 부모 cgroup(예: /sys/fs/cgroup/workload.slice)의 직계 하위 디렉터리를 컨테이너로 보고,
 * 각각의 memory.pressure·memory.stat·memory.events를 열어 둔다.
 *
 * 스래싱 중에도 쓸 수 있도록 힙 할당을 하지 않는다(고정 크기 표, getdents64 직접 호출).
 * 컨테이너가 재시작되면 경로가 바뀌므로, 다음 둘 중 하나일 때 다시 스캔한다.
 *   - 부모의 cgroup.stat nr_descendants가 마지막 스캔 때와 달라졌다
 *   - 대상 파일 읽기가 실패했다(cgroup이 사라지면 ENODEV)
 */
#ifndef TG_CGSCAN_H
#define TG_CGSCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TG_CG_MAX 8      /* 동시에 추적하는 최대 cgroup 수. 넘는 것은 건너뛴다 */
#define TG_CG_NAME 96    /* 디렉터리 이름 최대 길이(NUL 포함) */

struct tg_cg_target {
    char name[TG_CG_NAME];
    int pressure_fd;
    int stat_fd;
    int events_fd;
};

struct tg_cg_set {
    int parent_fd;        /* 부모 디렉터리. 음수면 비활성 */
    int cgstat_fd;        /* 부모의 cgroup.stat */
    uint64_t last_desc;   /* 마지막 스캔 때의 nr_descendants */
    uint64_t last_dying;
    size_t n;
    size_t skipped;       /* 표가 가득 차서 건너뛴 수(마지막 스캔 기준) */
    bool need_rescan;     /* 읽기 실패 등으로 다시 스캔해야 한다 */
    struct tg_cg_target t[TG_CG_MAX];
    char dents[4096];     /* getdents64 버퍼 */
};

/* 비활성 상태(열린 것 없음)로 초기화한다. open 전에 부르지 않아도 되지만, 정적 영역은 0이 유효한 fd라서 필요하다. */
void tg_cg_set_init(struct tg_cg_set *s);

/* 부모를 열고 첫 스캔을 한다. 부모를 열 수 없으면 -errno. */
int tg_cg_set_open(struct tg_cg_set *s, const char *parent_path);

/* 다시 스캔해야 하면 스캔한다. 스캔했으면 true. */
bool tg_cg_set_refresh(struct tg_cg_set *s);

/* 읽기 실패를 알린다. 다음 refresh에서 다시 스캔한다. */
void tg_cg_set_mark_stale(struct tg_cg_set *s);

void tg_cg_set_close(struct tg_cg_set *s);

#endif /* TG_CGSCAN_H */
