/*
 * engine.h - 트리거와 틱을 처리하는 엔진 (poll과 신호는 모른다)
 *
 * 이벤트 루프(loop.c)가 "트리거가 왔다", "다음 틱 시각이 되었다"를 알려 주면, 엔진은
 *   샘플 수집 -> 판단 함수 -> 결정 처리(로그, 종료 실행, 정책 기록)
 * 를 한다. 시각을 인자로 받아서 실제 시계와 poll 없이 단위 테스트할 수 있다.
 *
 * 로그는 메인 스레드(이 엔진을 부르는 스레드)만 쓴다. 로그는 사건과 요약 단위로만 남긴다
 * (트리거, 건너뜀, 진정, 종료, 지연 경고). 샘플마다 남기지 않는다.
 *
 * 힙 할당을 하지 않는다. 구조체가 크므로(수십 KiB) 호출자가 정적으로 둔다.
 */
#ifndef TG_ENGINE_H
#define TG_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "judge.h"
#include "log.h"
#include "policy.h"
#include "sample.h"
#include "scanner.h"

struct tg_engine_stats {
    uint64_t triggers;
    uint64_t ticks;
    uint64_t skips;
    uint64_t calms;
    uint64_t kills;        /* 실제로 cgroup.kill을 쓴 횟수 */
    uint64_t dry_kills;    /* dry-run으로 가상 종료한 횟수 */
    uint64_t kill_errors;  /* 종료 실행에 실패한 횟수 */
    uint64_t sample_errors;
    uint64_t late_ticks;   /* 예정보다 1틱 이상 늦은 횟수 */
    uint64_t max_delay_ns; /* 예정 시각보다 늦은 최대 시간 */
};

struct tg_engine {
    const struct tg_config *cfg;
    struct tg_judge judge;
    struct tg_policy policy;
    struct tg_log *log;
    struct tg_scanner *scanner;
    struct tg_reader reader;
    struct tg_sample_buf sbuf;
    int parent_fd; /* target_parent 디렉터리. 종료 실행의 기준 */

    bool watching;
    bool delay_warned; /* 이번 관찰에서 지연 경고를 이미 남겼다 */
    bool error_warned; /* 이번 관찰에서 샘플 읽기 실패를 이미 알렸다 */
    bool skipped_warned; /* 이번 관찰에서 후보 표가 가득 찬 것을 이미 알렸다 */
    uint64_t interval_ns;
    uint64_t deadline_ns; /* 다음 틱 예정 시각 */

    struct tg_engine_stats st;
};

/*
 * cfg는 엔진보다 오래 살아야 한다. psi_path, io_path, meminfo_path는 보통 /proc/... 이고 테스트에서 바꾼다.
 * io_path를 열지 못하거나 cfg->io_full_pct가 0이면 io 경로 없이 시작한다(reader.io_fd < 0, 판단의 io 경로 꺼짐).
 * 실패하면 음수 errno이고 아무것도 열려 있지 않다.
 */
int tg_engine_init(struct tg_engine *e, const struct tg_config *cfg, struct tg_log *log,
                   struct tg_scanner *scanner, const char *psi_path, const char *io_path,
                   const char *meminfo_path);
void tg_engine_close(struct tg_engine *e);

/* 커널 트리거가 발화했다. 이미 관찰 중이면 무시한다. 첫 틱은 바로 처리하도록 예정한다. */
void tg_engine_trigger(struct tg_engine *e, uint64_t now_ns);

/* 틱 시각이 되었다. 관찰 중이 아니면 아무것도 하지 않는다. */
void tg_engine_tick(struct tg_engine *e, uint64_t now_ns);

static inline bool tg_engine_watching(const struct tg_engine *e)
{
    return e->watching;
}

static inline uint64_t tg_engine_deadline(const struct tg_engine *e)
{
    return e->deadline_ns;
}

/* 종료 때 통계 요약을 로그로 남긴다. */
void tg_engine_log_summary(struct tg_engine *e);

#endif /* TG_ENGINE_H */
