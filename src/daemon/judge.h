/*
 * judge.h - 스래싱 판단 (순수 함수)
 *
 * "지금 스래싱인가, 맞다면 누구를 종료할까, 지금 종료해도 되는가"를 결정한다.
 * 파일을 읽거나 종료를 실행하지 않는다. 호출자가 숫자(누적 카운터, 메모리 양, 시각)를 넘기고
 * 결정을 받는다. 그래서 실제 시계나 /proc 없이 단위 테스트가 가능하다.
 * 힙 할당을 하지 않는다. 상태는 모두 이 구조체 안의 고정 배열이다.
 * 규칙의 이유는 docs/design/overview.md와 docs/design/victim-selection.md에 있다.
 *
 * 흐름:
 *   대기 -- tg_judge_trigger --> 관찰
 *   관찰: 샘플마다 시스템 full 증가분과 후보별 사용량을 최근 N초 윈도에 쌓는다.
 *     - 시스템 평균 full < X/2         -> 대기로 복귀 (CALM)
 *     - X/2 <= 평균 < X                -> 계속 관찰
 *     - 평균 >= X                      -> MemAvailable이 전체의 Y% 이하인지 확인
 *         Y% 초과면 건너뜀(MEM_AVAILABLE). 이하면 후보를 고른다.
 *   후보 선택:
 *     제외: 보호된 후보, 유예 중인 후보, 윈도가 덜 찬 후보
 *     자격: 윈도 안 최대 사용량이 전체 메모리의 min_size_pct 이상
 *     순위: 증가량(최대 - 윈도 첫 샘플)이 전체의 min_growth_pct 이상인 후보 중 증가량이 가장 큰 것.
 *           없으면 최대 사용량이 가장 큰 것. 동률이면 샘플 후보 배열에서 앞에 있는 것.
 *     자격 있는 후보가 없으면 건너뜀(NO_CULPRIT), 쿨다운·시간당 상한에 걸리면 건너뜀,
 *     통과하면 종료 결정을 내고 대기로 복귀한다.
 *
 * 평균 full 비율은 윈도 안의 실제 경과 시간으로 나눈다(샘플 간격이 흔들려도 정확하도록).
 * 비율은 100배한 정수다. 예: 12.34% -> 1234.
 */
#ifndef TG_JUDGE_H
#define TG_JUDGE_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "policy.h"
#include "targets.h"

/* 최근 n칸의 증가분을 담는 고정 배열 윈도 (시스템 full, 샘플 간 경과 시간) */
struct tg_win {
    uint32_t v[TG_MAX_WINDOW_SAMPLES];
    uint64_t sum;
    uint32_t head;
    uint32_t filled;
};

/*
 * 최근 사용량(KiB, 64비트라 수 TiB 호스트에서도 포화하지 않는다)과 각 샘플의 시각.
 * 칸 수가 아니라 시간으로 관리한다: 가장 오래된 샘플을 빼도 윈도가 대표하는 시간(span)을 채우면 뺀다.
 * 가장 오래된 칸은 (head - filled) 위치다.
 */
struct tg_memwin {
    uint64_t kib[TG_MAX_WINDOW_SAMPLES];
    uint64_t ns[TG_MAX_WINDOW_SAMPLES];
    uint32_t head;
    uint32_t filled;
};

struct tg_judge_slot {
    bool used;
    char name[TG_TARGET_NAME_CAP];
    uint64_t first_seen_ns; /* 같은 이름으로 cgroup이 다시 만들어지면 달라진다. 달라지면 윈도를 버린다 */
    struct tg_memwin mem;
};

enum tg_judge_reason {
    TG_R_NONE = 0,
    TG_R_CALM,          /* 압력이 가라앉아 관찰을 끝냈다 */
    TG_R_MEM_AVAILABLE, /* 압력은 높지만 MemAvailable이 아직 충분하다 */
    TG_R_NO_CANDIDATE,  /* 판단할 수 있는 후보가 없다 (보호·유예·윈도 덜 참) */
    TG_R_NO_CULPRIT,    /* 후보는 있으나 모두 자격(최소 사용량) 미달이다 */
    TG_R_COOLDOWN,      /* 쿨다운 중 */
    TG_R_HOURLY_LIMIT,  /* 시간당 상한 도달 */
};

enum tg_judge_kind {
    TG_J_IDLE = 0, /* 대기 중이거나 관찰을 끝냈다 (reason 참조) */
    TG_J_WATCHING, /* 관찰 중. 아직 결정할 것이 없다 */
    TG_J_SKIP,     /* 종료 조건은 충족했으나 건너뛴다 (reason 참조). 같은 사유는 한 번만 알린다 */
    TG_J_KILL,     /* cand_index의 후보를 종료하라 */
};

/* 후보 한 개의 이번 샘플 기준 지표. 로그와 사후 점검용 */
struct tg_cand_stat {
    bool ready;      /* 윈도가 가득 찼다 */
    bool in_grace;   /* 유예 중 */
    bool is_protected;
    bool eligible;   /* ready && !유예 && !보호 && 최대 사용량 >= 최소 */
    bool growing;    /* 증가량 >= 최소 증가 */
    uint64_t max_bytes;    /* 윈도 안 최대 사용량. ready가 아니면 0 */
    uint64_t growth_bytes; /* 최대 - 윈도 첫 샘플. ready가 아니면 0 */
};

struct tg_decision {
    enum tg_judge_kind kind;
    enum tg_judge_reason reason;
    int32_t cand_index;      /* KILL일 때 샘플 후보 배열의 번호, 아니면 -1 */
    uint32_t sys_avg_x100;   /* 시스템 평균 full. 윈도가 덜 찼으면 0 */
    uint32_t avail_x100;     /* MemAvailable / 전체 메모리 (100배한 %). 확인 전이면 0 */
    uint64_t cand_max_bytes; /* KILL일 때 선택된 후보의 값 */
    uint64_t cand_growth_bytes;
    bool cand_growing;
};

struct tg_judge {
    /* 설정 복사본 */
    uint32_t nsamples;
    uint64_t interval_ns;
    uint64_t span_us; /* 윈도가 대표하는 시간(칸 수 x 샘플 간격). 샘플 사이가 이보다 벌어지면 윈도를 버린다 */
    uint32_t full_pct;
    uint32_t mem_avail_pct;
    uint32_t min_size_pct;
    uint32_t min_growth_pct;
    uint32_t grace_sec;

    bool watching;
    enum tg_judge_reason last_skip;

    bool sys_has_prev;
    uint64_t sys_prev_total_us;
    uint64_t sys_prev_now_ns;
    struct tg_win sys_full; /* 시스템 full stall 증가분(us) */
    struct tg_win sys_dt;   /* 샘플 간 경과 시간(us) */

    struct tg_judge_slot slot[TG_MAX_TARGETS];

    struct tg_cand_stat stat[TG_MAX_TARGETS];
    uint32_t nstat;
};

/* 샘플 한 개 안의 후보 한 개 */
struct tg_cand_sample {
    const char *name;
    uint64_t mem_bytes;     /* 후보 메모리 사용량(anon + file) */
    uint64_t first_seen_ns; /* tg_target.first_seen_ns */
    bool is_protected;      /* 보호된 cgroup이면 후보에서 뺀다 */
};

struct tg_sample {
    uint64_t now_ns;
    uint64_t sys_full_total_us;  /* /proc/pressure/memory full total */
    uint64_t mem_available_bytes;
    uint64_t mem_total_bytes;
    const struct tg_cand_sample *cand;
    uint32_t ncand;
};

/* cfg에서 판단에 쓰는 값(윈도 칸 수, 임계, 자격, 유예)을 복사한다. */
void tg_judge_init(struct tg_judge *j, const struct tg_config *cfg);

/* 커널 트리거가 발화했다. 대기 중이면 관찰을 시작한다. 관찰 중이면 무시한다. */
void tg_judge_trigger(struct tg_judge *j);

/*
 * 샘플을 처리해 결정을 돌려준다. pol은 읽기만 한다. 종료 결정을 받은 호출자는
 * (dry-run이든 아니든) tg_policy_record_kill을 불러야 한다.
 * 관찰 중이 아니면 TG_J_IDLE을 돌려준다.
 */
struct tg_decision tg_judge_tick(struct tg_judge *j, const struct tg_sample *s,
                                 const struct tg_policy *pol);

/* 직전 tg_judge_tick의 후보별 지표. 샘플 후보 배열과 같은 순서다. */
const struct tg_cand_stat *tg_judge_stats(const struct tg_judge *j, uint32_t *n);

const char *tg_judge_reason_name(enum tg_judge_reason r);
const char *tg_judge_kind_name(enum tg_judge_kind k);

#endif /* TG_JUDGE_H */
