/*
 * judge.h - 스래싱 판단 (순수 함수)
 *
 * "지금 스래싱인가, 맞다면 누구를 종료할까, 지금 종료해도 되는가"를 결정한다.
 * 파일을 읽거나 종료를 실행하지 않는다. 호출자가 숫자(누적 카운터와 시각)를 넘기고
 * 결정을 받는다. 그래서 실제 시계나 /proc 없이 단위 테스트와 리플레이가 가능하다.
 * 힙 할당을 하지 않는다. 상태는 모두 이 구조체 안의 고정 배열이다.
 *
 * 흐름 (D15, D16, D17, D20, D22):
 *   대기 -- tg_judge_trigger --> 관찰
 *   관찰: 샘플마다 구역(target_parent)과 후보별 증가분을 최근 N초 윈도에 쌓는다.
 *     - 구역 평균 full < X/2          -> 대기로 복귀 (CALM)
 *     - X/2 <= 평균 < X               -> 계속 관찰
 *     - 평균 >= X                     -> 후보 선택
 *         유예 중이거나 윈도가 덜 찬 후보는 제외하고, 평균 full >= X인 후보 중
 *         pgscan 증가량이 가장 큰 것을 고른다. 동률이면 메모리 사용량, 그래도 같으면 앞 번호.
 *         후보가 없거나 자격 미달이면 건너뜀, 쿨다운·시간당 상한에 걸리면 건너뜀,
 *         통과하면 종료 결정을 내고 대기로 복귀한다.
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

/* 최근 n칸의 증가분을 담는 고정 배열 윈도 */
struct tg_win {
    uint32_t v[TG_MAX_WINDOW_SAMPLES];
    uint64_t sum;
    uint32_t head;
    uint32_t filled;
};

struct tg_judge_slot {
    bool used;
    char name[TG_TARGET_NAME_CAP];
    bool has_prev;
    uint64_t prev_full_us;
    uint64_t prev_pgscan;
    struct tg_win full; /* 샘플당 full stall 증가분(us) */
    struct tg_win scan; /* 샘플당 pgscan 증가분(페이지) */
};

enum tg_judge_reason {
    TG_R_NONE = 0,
    TG_R_CALM,           /* 압력이 가라앉아 관찰을 끝냈다 */
    TG_R_NO_CANDIDATE,   /* 판단할 수 있는 후보가 없다 (유예 중이거나 윈도가 덜 참) */
    TG_R_NO_CULPRIT,     /* 후보는 있으나 평균 full이 X 미만이라 책임을 물을 수 없다 */
    TG_R_COOLDOWN,       /* 쿨다운 중 */
    TG_R_HOURLY_LIMIT,   /* 시간당 상한 도달 */
};

enum tg_judge_kind {
    TG_J_IDLE = 0, /* 대기 중이거나 관찰을 끝냈다 (reason 참조) */
    TG_J_WATCHING, /* 관찰 중. 아직 결정할 것이 없다 */
    TG_J_SKIP,     /* 종료 조건은 충족했으나 건너뛴다 (reason 참조). 같은 사유는 한 번만 알린다 */
    TG_J_KILL,     /* cand_index의 후보를 종료하라 */
};

/* 후보 한 개의 이번 샘플 기준 지표. 로그와 사후 비교용(D20) */
struct tg_cand_stat {
    bool ready;            /* 윈도가 가득 찼다 */
    bool in_grace;         /* 유예 중 */
    bool eligible;         /* ready && !in_grace && 평균 full >= X */
    uint32_t full_avg_x100;
    uint64_t pgscan_delta; /* 윈도 안의 pgscan 증가량 */
    uint64_t mem_bytes;
};

struct tg_decision {
    enum tg_judge_kind kind;
    enum tg_judge_reason reason;
    int32_t cand_index;        /* KILL일 때 샘플 후보 배열의 번호, 아니면 -1 */
    uint32_t zone_avg_x100;    /* 구역 평균 full. 윈도가 덜 찼으면 0 */
    uint32_t cand_avg_x100;    /* KILL일 때 선택된 후보의 평균 full */
    uint64_t cand_pgscan_delta;
    uint64_t cand_mem_bytes;
};

struct tg_judge {
    /* 설정 복사본 */
    uint32_t nsamples;
    uint32_t full_pct;
    uint32_t grace_sec;

    bool watching;
    uint64_t epoch;
    enum tg_judge_reason last_skip;

    bool zone_has_prev;
    uint64_t zone_prev_total_us;
    uint64_t zone_prev_now_ns;
    struct tg_win zone_full; /* 구역 full stall 증가분(us) */
    struct tg_win zone_dt;   /* 샘플 간 경과 시간(us) */

    struct tg_judge_slot slot[TG_MAX_TARGETS];

    struct tg_cand_stat stat[TG_MAX_TARGETS];
    uint32_t nstat;
};

/* 샘플 한 개 안의 후보 한 개 */
struct tg_cand_sample {
    const char *name;
    uint64_t full_total_us; /* 후보 memory.pressure full total */
    uint64_t pgscan;        /* 후보 memory.stat pgscan 누적 */
    uint64_t mem_bytes;     /* 후보 메모리 사용량 */
    uint64_t first_seen_ns; /* tg_target.first_seen_ns */
};

struct tg_sample {
    uint64_t now_ns;
    uint64_t zone_full_total_us; /* target_parent memory.pressure full total */
    const struct tg_cand_sample *cand;
    uint32_t ncand;
};

/* cfg에서 sustain_sec, sample_interval_ms, full_pct, startup_grace_sec를 복사한다. */
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
