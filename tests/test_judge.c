/*
 * test_judge.c - 스래싱 판단 함수 단위 테스트
 *
 * 실제 시계나 /proc 없이 숫자만으로 상황을 재현한다. 한 샘플은 250 ms이고,
 * "pct%"는 그 250 ms 중 pct%를 full stall로 보냈다는 뜻이다.
 * 기본 설정: N=10초(40칸), X=10%, 유예 30초, 쿨다운 60초.
 * 첫 샘플은 기준점만 잡으므로 윈도는 41번째 샘플에서 처음 찬다.
 */
#include <string.h>

#include "judge.h"
#include "tg_test.h"

#define SEC(x) ((uint64_t)(x) * 1000000000ull)
#define NC 3
#define TICK_NS 250000000ull
#define PCT_US 2500ull /* 250 ms의 1% = 2500 us */

static const char *const NAMES[NC] = {"A", "B", "C"};

struct sim {
    struct tg_config cfg;
    struct tg_policy pol;
    struct tg_judge j;
    uint64_t now;
    uint64_t zone_us;
    uint64_t full_us[NC];
    uint64_t scan[NC];
    uint64_t mem[NC];
    uint64_t first_seen[NC];
    uint32_t nc;
    uint32_t absent; /* 비트가 켜진 후보는 이번 샘플에서 빠진다 */
    struct tg_cand_sample cs[NC];
    int ticks;
};

static void sim_init(struct sim *s, uint32_t nc)
{
    memset(s, 0, sizeof(*s));
    tg_config_defaults(&s->cfg);
    tg_policy_init(&s->pol, s->cfg.cooldown_sec, s->cfg.max_kills_per_hour);
    tg_judge_init(&s->j, &s->cfg);
    s->now = SEC(1000);
    s->nc = nc;
}

static struct tg_decision sim_tick(struct sim *s, uint32_t zone_pct, const uint32_t cpct[NC],
                                   const uint32_t scan_inc[NC])
{
    struct tg_sample smp;
    uint32_t n = 0;

    s->ticks++;
    s->now += TICK_NS;
    s->zone_us += zone_pct * PCT_US;
    for (uint32_t i = 0; i < s->nc; i++) {
        s->full_us[i] += cpct[i] * PCT_US;
        s->scan[i] += scan_inc[i];
        if ((s->absent >> i) & 1u)
            continue;
        s->cs[n].name = NAMES[i];
        s->cs[n].full_total_us = s->full_us[i];
        s->cs[n].pgscan = s->scan[i];
        s->cs[n].mem_bytes = s->mem[i];
        s->cs[n].first_seen_ns = s->first_seen[i];
        n++;
    }
    smp.now_ns = s->now;
    smp.zone_full_total_us = s->zone_us;
    smp.cand = s->cs;
    smp.ncand = n;
    return tg_judge_tick(&s->j, &smp, &s->pol);
}

/* 같은 입력으로 최대 max번 돌리며 WATCHING이 아닌 첫 결정을 돌려준다. */
static struct tg_decision run_until(struct sim *s, int max, uint32_t zone_pct,
                                    const uint32_t cpct[NC], const uint32_t scan_inc[NC])
{
    struct tg_decision d;

    memset(&d, 0, sizeof(d));
    for (int i = 0; i < max; i++) {
        d = sim_tick(s, zone_pct, cpct, scan_inc);
        if (d.kind != TG_J_WATCHING)
            return d;
    }
    return d;
}

static void test_idle_without_trigger(void)
{
    struct sim s;
    const uint32_t z[NC] = {0};
    struct tg_decision d;

    sim_init(&s, 1);
    d = sim_tick(&s, 50, z, z);
    TG_CHECK(d.kind == TG_J_IDLE);
    TG_CHECK_EQ_U64(s.ticks, 1);

    /* 트리거를 두 번 불러도 관찰은 한 번만 시작한다 */
    tg_judge_trigger(&s.j);
    tg_judge_trigger(&s.j);
    d = sim_tick(&s, 50, z, z);
    TG_CHECK(d.kind == TG_J_WATCHING);
}

static void test_sustained_thrash_kills(void)
{
    struct sim s;
    const uint32_t cp[NC] = {40, 5, 0};
    const uint32_t sc[NC] = {100, 10, 0};
    struct tg_decision d;
    const struct tg_cand_stat *st;
    uint32_t n;

    sim_init(&s, 3);
    tg_judge_trigger(&s.j);
    d = run_until(&s, 100, 30, cp, sc);

    /* 최소 감지 지연: 첫 샘플은 기준점, 40칸이 차는 41번째에 결정  */
    TG_CHECK_EQ_U64(s.ticks, 41);
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK(d.cand_index == 0);
    TG_CHECK_EQ_U64(d.zone_avg_x100, 3000);
    TG_CHECK_EQ_U64(d.cand_avg_x100, 4000);
    TG_CHECK_EQ_U64(d.cand_pgscan_delta, 4000);

    /* 후보별 지표가 로그용으로 남는다  */
    st = tg_judge_stats(&s.j, &n);
    TG_CHECK_EQ_U64(n, 3);
    TG_CHECK_EQ_U64(st[1].full_avg_x100, 500);
    TG_CHECK(!st[1].eligible);
    TG_CHECK(st[0].eligible);

    /* 종료 결정 뒤에는 대기로 돌아간다 */
    d = sim_tick(&s, 30, cp, sc);
    TG_CHECK(d.kind == TG_J_IDLE);
}

static void test_short_burst_no_kill(void)
{
    struct sim s;
    const uint32_t z[NC] = {0};
    struct tg_decision d;
    bool killed = false;
    bool calm = false;

    /* 1초(4샘플) 50% 버스트 후 2%: 평균 6.8%라 X(10%) 미만, X/2(5%) 이상이라 관찰을 이어 간다 */
    sim_init(&s, 1);
    tg_judge_trigger(&s.j);
    for (int t = 1; t <= 60 && !calm && !killed; t++) {
        uint32_t pct = (t >= 2 && t <= 5) ? 50 : 2;
        uint32_t cp[NC] = {pct, 0, 0};
        uint32_t sc[NC] = {50, 0, 0};

        d = sim_tick(&s, pct, cp, sc);
        if (t == 41) {
            TG_CHECK(d.kind == TG_J_WATCHING);
            TG_CHECK(d.zone_avg_x100 >= 500 && d.zone_avg_x100 < 1000);
        }
        killed = d.kind == TG_J_KILL;
        if (d.kind == TG_J_IDLE && d.reason == TG_R_CALM) {
            calm = true;
            /* 버스트가 윈도를 빠져나가면 평균이 5% 아래로 내려와 관찰을 끝낸다 */
            TG_CHECK_EQ_U64(s.ticks, 43);
        }
    }
    (void)z;
    TG_CHECK(!killed);
    TG_CHECK(calm);
}

static void test_known_weakness_long_burst(void)
{
    struct sim s;
    struct tg_decision d;
    struct tg_decision last;

    /*
     * 특성 확인(characterization): 3초(12샘플) 60% 버스트는 10초 평균이 19.4%라 X를 넘어
     * 종료 결정이 난다. D15에 기록한 평균형의 알려진 약점이다. 비율형으로 바꾸면
     * 이 테스트가 바뀌어야 한다.
     */
    sim_init(&s, 1);
    tg_judge_trigger(&s.j);
    memset(&last, 0, sizeof(last));
    for (int t = 1; t <= 41; t++) {
        uint32_t pct = (t >= 2 && t <= 13) ? 60 : 2;
        uint32_t cp[NC] = {pct, 0, 0};
        uint32_t sc[NC] = {50, 0, 0};

        d = sim_tick(&s, pct, cp, sc);
        last = d;
    }
    TG_CHECK(last.kind == TG_J_KILL);
    TG_CHECK_EQ_U64(last.zone_avg_x100, 1940);
}

static void test_fluctuating_thrash_detected(void)
{
    struct sim s;
    struct tg_decision d;

    /* 30%와 5%가 번갈아 나오는 출렁이는 스래싱: 평균 17.5%라 감지한다 (연속형이면 놓칠 상황) */
    sim_init(&s, 1);
    tg_judge_trigger(&s.j);
    memset(&d, 0, sizeof(d));
    for (int t = 1; t <= 41; t++) {
        uint32_t pct = (t % 2 == 1) ? 30 : 5;
        uint32_t cp[NC] = {pct, 0, 0};
        uint32_t sc[NC] = {100, 0, 0};

        d = sim_tick(&s, pct, cp, sc);
    }
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK_EQ_U64(d.zone_avg_x100, 1750);
}

static void test_gradual_rise(void)
{
    struct sim s;
    struct tg_decision d;
    const uint32_t cp8[NC] = {8, 0, 0};
    const uint32_t cp30[NC] = {30, 0, 0};
    const uint32_t sc[NC] = {50, 0, 0};

    /* 8%로 시작하면 윈도가 차도 X 미만이라 계속 관찰만 한다 */
    sim_init(&s, 1);
    tg_judge_trigger(&s.j);
    d = run_until(&s, 41, 8, cp8, sc);
    TG_CHECK(d.kind == TG_J_WATCHING);
    TG_CHECK_EQ_U64(d.zone_avg_x100, 800);

    /* 30%로 오르면 윈도 평균이 X를 넘는 4번째 샘플에 바로 결정한다 (한 번만 평가하는 방식이면 놓칠 상황) */
    d = run_until(&s, 20, 30, cp30, sc);
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK_EQ_U64(s.ticks, 45);
}

static void test_grace_excludes_candidate(void)
{
    struct sim s;
    const uint32_t cp[NC] = {40, 0, 0};
    const uint32_t sc[NC] = {100, 0, 0};
    struct tg_decision d;

    sim_init(&s, 1);
    s.first_seen[0] = s.now; /* 방금 생긴 컨테이너 */
    tg_judge_trigger(&s.j);

    d = run_until(&s, 100, 30, cp, sc);
    TG_CHECK_EQ_U64(s.ticks, 41);
    TG_CHECK(d.kind == TG_J_SKIP);
    TG_CHECK(d.reason == TG_R_NO_CANDIDATE);

    /* 같은 사유는 다시 알리지 않고, 유예(30초 = 120샘플)가 끝나는 순간 종료 결정이 난다 */
    d = run_until(&s, 200, 30, cp, sc);
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK_EQ_U64(s.ticks, 120);
}

static void test_no_culprit_then_found(void)
{
    struct sim s;
    const uint32_t low[NC] = {5, 5, 0};
    const uint32_t high[NC] = {40, 5, 0};
    const uint32_t sc[NC] = {100, 10, 0};
    struct tg_decision d;

    sim_init(&s, 3);
    tg_judge_trigger(&s.j);

    /* 구역은 힘든데 어느 후보의 평균도 X에 못 미친다 */
    d = run_until(&s, 100, 30, low, sc);
    TG_CHECK_EQ_U64(s.ticks, 41);
    TG_CHECK(d.kind == TG_J_SKIP);
    TG_CHECK(d.reason == TG_R_NO_CULPRIT);

    /* A가 오르면 윈도 평균이 X를 넘는 6번째 샘플에 종료 결정 */
    d = run_until(&s, 100, 30, high, sc);
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK(d.cand_index == 0);
    TG_CHECK_EQ_U64(s.ticks, 47);
}

static void test_selection_by_pgscan(void)
{
    struct sim s;
    const uint32_t cp[NC] = {20, 15, 0};
    struct tg_decision d;

    /* 압력은 A가 더 크지만 회수 부담(pgscan)은 B가 크다: B를 고른다  */
    {
        const uint32_t sc[NC] = {10, 500, 0};

        sim_init(&s, 3);
        tg_judge_trigger(&s.j);
        d = run_until(&s, 100, 30, cp, sc);
        TG_CHECK(d.kind == TG_J_KILL);
        TG_CHECK(d.cand_index == 1);
    }
    /* pgscan이 같으면 메모리 사용량이 큰 쪽 */
    {
        const uint32_t sc[NC] = {100, 100, 0};

        sim_init(&s, 3);
        s.mem[0] = 1000;
        s.mem[1] = 2000;
        tg_judge_trigger(&s.j);
        d = run_until(&s, 100, 30, cp, sc);
        TG_CHECK(d.kind == TG_J_KILL);
        TG_CHECK(d.cand_index == 1);
    }
    /* 그것도 같으면 앞 번호 */
    {
        const uint32_t sc[NC] = {100, 100, 0};

        sim_init(&s, 3);
        s.mem[0] = 1000;
        s.mem[1] = 1000;
        tg_judge_trigger(&s.j);
        d = run_until(&s, 100, 30, cp, sc);
        TG_CHECK(d.kind == TG_J_KILL);
        TG_CHECK(d.cand_index == 0);
    }
}

static void test_policy_blocks(void)
{
    struct sim s;
    const uint32_t cp[NC] = {40, 0, 0};
    const uint32_t sc[NC] = {100, 0, 0};
    struct tg_decision d;

    /* 쿨다운: 관찰 시작 시점에 방금 종료한 기록이 있다 */
    sim_init(&s, 1);
    tg_policy_record_kill(&s.pol, s.now);
    tg_judge_trigger(&s.j);
    d = run_until(&s, 100, 30, cp, sc);
    TG_CHECK_EQ_U64(s.ticks, 41);
    TG_CHECK(d.kind == TG_J_SKIP);
    TG_CHECK(d.reason == TG_R_COOLDOWN);
    /* 같은 사유는 반복하지 않고, 쿨다운 60초(240샘플)가 끝나는 순간 종료 결정 */
    d = run_until(&s, 400, 30, cp, sc);
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK_EQ_U64(s.ticks, 240);

    /* 시간당 상한: 쿨다운은 지났지만 1시간 안에 이미 상한만큼 종료했다 */
    sim_init(&s, 1);
    tg_policy_init(&s.pol, 60, 1);
    tg_policy_record_kill(&s.pol, s.now - SEC(70));
    tg_judge_trigger(&s.j);
    d = run_until(&s, 100, 30, cp, sc);
    TG_CHECK(d.kind == TG_J_SKIP);
    TG_CHECK(d.reason == TG_R_HOURLY_LIMIT);
    d = run_until(&s, 400, 30, cp, sc);
    TG_CHECK(d.kind == TG_J_WATCHING); /* 100초가 더 지나도 종료하지 않는다 */
}

static void test_counter_reset(void)
{
    struct sim s;
    const uint32_t cp[NC] = {30, 0, 0};
    const uint32_t sc[NC] = {100, 0, 0};
    struct tg_decision d;

    /* 21번째 샘플에서 누적 카운터가 0으로 돌아간다(cgroup 재생성). 증가분 0으로 본다. */
    sim_init(&s, 1);
    tg_judge_trigger(&s.j);
    memset(&d, 0, sizeof(d));
    for (int t = 1; t <= 41; t++) {
        if (t == 21) {
            s.zone_us = 0;
            s.full_us[0] = 0;
            s.scan[0] = 0;
        }
        d = sim_tick(&s, 30, cp, sc);
    }
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK_EQ_U64(d.zone_avg_x100, 2925); /* 39칸 x 30% / 40칸 */
    TG_CHECK(d.zone_avg_x100 <= 10000);
}

static void test_candidate_disappears(void)
{
    struct sim s;
    const uint32_t cp[NC] = {40, 0, 0};
    const uint32_t sc[NC] = {100, 0, 0};
    struct tg_decision d;

    /* 30번째 샘플에서 후보가 한 번 사라졌다 돌아오면 그 후보의 윈도는 처음부터 다시 쌓는다 */
    sim_init(&s, 1);
    tg_judge_trigger(&s.j);
    d = run_until(&s, 29, 30, cp, sc);
    TG_CHECK(d.kind == TG_J_WATCHING);
    s.absent = 1;
    d = sim_tick(&s, 30, cp, sc);
    TG_CHECK(d.kind == TG_J_WATCHING);
    s.absent = 0;

    d = run_until(&s, 100, 30, cp, sc);
    TG_CHECK_EQ_U64(s.ticks, 41);
    TG_CHECK(d.kind == TG_J_SKIP);
    TG_CHECK(d.reason == TG_R_NO_CANDIDATE);

    d = run_until(&s, 100, 30, cp, sc);
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK_EQ_U64(s.ticks, 71); /* 복귀 다음 샘플(32번)부터 40칸이 모이는 71번째 */
}

static void test_irregular_interval(void)
{
    struct sim s;
    struct tg_sample smp;
    struct tg_cand_sample c;
    struct tg_decision d;
    uint64_t now = SEC(1000);
    uint64_t zone = 0;
    int ticks = 0;

    /*
     * 데몬이 밀려 샘플 간격이 500 ms로 늘어도 평균은 실제 경과 시간으로 나눈다.
     * 500 ms 동안 150 ms stall이면 30%다.
     */
    sim_init(&s, 1);
    tg_judge_trigger(&s.j);
    c.name = "A";
    c.mem_bytes = 0;
    c.first_seen_ns = 0;
    c.pgscan = 0;
    c.full_total_us = 0;
    smp.cand = &c;
    smp.ncand = 1;
    memset(&d, 0, sizeof(d));
    while (d.kind == TG_J_WATCHING || ticks == 0) {
        now += 2 * TICK_NS;
        zone += 150000;
        c.full_total_us += 150000;
        c.pgscan += 100;
        smp.now_ns = now;
        smp.zone_full_total_us = zone;
        d = tg_judge_tick(&s.j, &smp, &s.pol);
        ticks++;
        if (ticks > 100)
            break;
    }
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK_EQ_U64(d.zone_avg_x100, 3000);
}

static void test_names(void)
{
    TG_CHECK(strcmp(tg_judge_reason_name(TG_R_NO_CULPRIT), "no_culprit") == 0);
    TG_CHECK(strcmp(tg_judge_reason_name(TG_R_CALM), "calm") == 0);
    TG_CHECK(strcmp(tg_judge_kind_name(TG_J_KILL), "kill") == 0);
    TG_CHECK(strcmp(tg_judge_kind_name(TG_J_SKIP), "skip") == 0);
}

int main(void)
{
    TG_RUN(test_idle_without_trigger);
    TG_RUN(test_sustained_thrash_kills);
    TG_RUN(test_short_burst_no_kill);
    TG_RUN(test_known_weakness_long_burst);
    TG_RUN(test_fluctuating_thrash_detected);
    TG_RUN(test_gradual_rise);
    TG_RUN(test_grace_excludes_candidate);
    TG_RUN(test_no_culprit_then_found);
    TG_RUN(test_selection_by_pgscan);
    TG_RUN(test_policy_blocks);
    TG_RUN(test_counter_reset);
    TG_RUN(test_candidate_disappears);
    TG_RUN(test_irregular_interval);
    TG_RUN(test_names);
    return tg_test_finish();
}
