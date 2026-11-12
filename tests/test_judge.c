/*
 * test_judge.c - 스래싱 판단 함수 단위 테스트
 *
 * 실제 시계나 /proc 없이 숫자만으로 상황을 재현한다. 한 샘플은 250 ms이고,
 * "pct%"는 그 250 ms 중 pct%를 시스템 full stall로 보냈다는 뜻이다.
 * 기본 설정: N=10초(40칸), X=40%, Y=10%, 자격 10%, 증가 인정 1%, 유예 30초, 쿨다운 60초.
 * 전체 메모리는 1 GiB로 가정한다(자격 약 102 MiB, 증가 인정 약 10 MiB).
 * 첫 샘플은 시스템 full의 기준점만 잡으므로 시스템 윈도는 41번째 샘플에서 처음 찬다.
 * 후보 사용량 윈도는 매 샘플 쌓으므로 40번째 샘플에서 찬다.
 */
#include <string.h>

#include "judge.h"
#include "tg_test.h"

#define SEC(x) ((uint64_t)(x) * 1000000000ull)
#define MIB(x) ((uint64_t)(x) << 20)
#define NC 3
#define TICK_NS 250000000ull
#define PCT_US 2500ull /* 250 ms의 1% = 2500 us */

static const char *const NAMES[NC] = {"A", "B", "C"};

struct sim {
    struct tg_config cfg;
    struct tg_policy pol;
    struct tg_judge j;
    uint64_t now;
    uint64_t sys_us;
    uint64_t total;
    uint64_t avail;
    uint64_t mem[NC];
    uint64_t first_seen[NC];
    bool prot[NC];
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
    s->total = MIB(1024);
    s->avail = MIB(50); /* 4.9%: 보호선(10%) 이하 */
    s->nc = nc;
    for (uint32_t i = 0; i < nc; i++)
        s->mem[i] = MIB(400);
}

static struct tg_decision sim_tick(struct sim *s, uint32_t sys_pct)
{
    struct tg_sample smp;
    uint32_t n = 0;

    s->ticks++;
    s->now += TICK_NS;
    s->sys_us += sys_pct * PCT_US;
    for (uint32_t i = 0; i < s->nc; i++) {
        if ((s->absent >> i) & 1u)
            continue;
        s->cs[n].name = NAMES[i];
        s->cs[n].mem_bytes = s->mem[i];
        s->cs[n].first_seen_ns = s->first_seen[i];
        s->cs[n].is_protected = s->prot[i];
        n++;
    }
    smp.now_ns = s->now;
    smp.sys_full_total_us = s->sys_us;
    smp.mem_available_bytes = s->avail;
    smp.mem_total_bytes = s->total;
    smp.cand = s->cs;
    smp.ncand = n;
    return tg_judge_tick(&s->j, &smp, &s->pol);
}

/* 같은 입력으로 최대 max번 돌리며 WATCHING이 아닌 첫 결정을 돌려준다. */
static struct tg_decision run_until(struct sim *s, int max, uint32_t sys_pct)
{
    struct tg_decision d;

    memset(&d, 0, sizeof(d));
    for (int i = 0; i < max; i++) {
        d = sim_tick(s, sys_pct);
        if (d.kind != TG_J_WATCHING)
            return d;
    }
    return d;
}

static void test_idle_without_trigger(void)
{
    struct sim s;
    struct tg_decision d;

    sim_init(&s, 1);
    d = sim_tick(&s, 50);
    TG_CHECK(d.kind == TG_J_IDLE);
    TG_CHECK_EQ_U64(s.ticks, 1);

    /* 트리거를 두 번 불러도 관찰은 한 번만 시작한다 */
    tg_judge_trigger(&s.j);
    tg_judge_trigger(&s.j);
    d = sim_tick(&s, 50);
    TG_CHECK(d.kind == TG_J_WATCHING);
}

static void test_sustained_thrash_kills(void)
{
    struct sim s;
    struct tg_decision d;
    const struct tg_cand_stat *st;
    uint32_t n;

    sim_init(&s, 3);
    s.mem[1] = MIB(50); /* 자격 미달 */
    s.mem[2] = MIB(300);
    tg_judge_trigger(&s.j);
    d = run_until(&s, 100, 50);

    /* 최소 감지 지연: 첫 샘플은 기준점, 40칸이 차는 41번째에 결정 */
    TG_CHECK_EQ_U64(s.ticks, 41);
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK(d.cand_index == 0); /* 증가 중인 후보가 없으니 가장 큰 A(400 MiB) */
    TG_CHECK_EQ_U64(d.sys_avg_x100, 5000);
    TG_CHECK_EQ_U64(d.cand_max_bytes, MIB(400));
    TG_CHECK_EQ_U64(d.cand_growth_bytes, 0);
    TG_CHECK(!d.cand_growing);

    /* 후보별 지표가 로그용으로 남는다 */
    st = tg_judge_stats(&s.j, &n);
    TG_CHECK_EQ_U64(n, 3);
    TG_CHECK(st[0].eligible);
    TG_CHECK(!st[1].eligible);
    TG_CHECK_EQ_U64(st[1].max_bytes, MIB(50));
    TG_CHECK(st[2].eligible);

    /* 종료 결정 뒤에는 대기로 돌아간다 */
    d = sim_tick(&s, 50);
    TG_CHECK(d.kind == TG_J_IDLE);
}

static void test_short_burst_calm(void)
{
    struct sim s;
    struct tg_decision d;
    bool killed = false;
    bool calm = false;

    /* 1초(4샘플) 80% 버스트 후 10%: 평균 17%라 X/2(20%) 미만이라 관찰을 끝낸다 */
    sim_init(&s, 1);
    tg_judge_trigger(&s.j);
    for (int t = 1; t <= 60 && !calm && !killed; t++) {
        d = sim_tick(&s, (t >= 2 && t <= 5) ? 80 : 10);
        killed = d.kind == TG_J_KILL;
        if (d.kind == TG_J_IDLE && d.reason == TG_R_CALM) {
            calm = true;
            TG_CHECK_EQ_U64(s.ticks, 41);
        }
    }
    TG_CHECK(!killed);
    TG_CHECK(calm);
}

static void test_known_weakness_long_burst(void)
{
    struct sim s;
    struct tg_decision d;
    struct tg_decision last;

    /*
     * 특성 확인(characterization): 5초(20샘플) 80% 버스트 후 5%는 10초 평균이 42.5%라 X를 넘어
     * 종료 결정이 난다. 평균형의 알려진 약점이며, 비율형 등으로 바꾸면 이 테스트가 바뀌어야 한다.
     */
    sim_init(&s, 1);
    tg_judge_trigger(&s.j);
    memset(&last, 0, sizeof(last));
    for (int t = 1; t <= 41; t++) {
        d = sim_tick(&s, (t >= 2 && t <= 21) ? 80 : 5);
        last = d;
    }
    TG_CHECK(last.kind == TG_J_KILL);
    TG_CHECK_EQ_U64(last.sys_avg_x100, 4250);
}

static void test_fluctuating_thrash_detected(void)
{
    struct sim s;
    struct tg_decision d;

    /* 70%와 20%가 번갈아 나오는 출렁이는 스래싱: 평균 45%라 감지한다 (연속형이면 놓칠 상황) */
    sim_init(&s, 1);
    tg_judge_trigger(&s.j);
    memset(&d, 0, sizeof(d));
    for (int t = 1; t <= 41; t++)
        d = sim_tick(&s, (t % 2 == 1) ? 70 : 20);
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK_EQ_U64(d.sys_avg_x100, 4500);
}

static void test_gradual_rise(void)
{
    struct sim s;
    struct tg_decision d;

    /* 30%는 X/2 이상 X 미만이라 윈도가 차도 계속 관찰만 한다 */
    sim_init(&s, 1);
    tg_judge_trigger(&s.j);
    d = run_until(&s, 41, 30);
    TG_CHECK(d.kind == TG_J_WATCHING);
    TG_CHECK_EQ_U64(d.sys_avg_x100, 3000);

    /* 60%로 오르면 윈도 평균이 40%를 넘는 14번째 샘플에 결정한다 (30 + 0.75k >= 40) */
    d = run_until(&s, 30, 60);
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK_EQ_U64(s.ticks, 55);
}

static void test_mem_available_guard(void)
{
    struct sim s;
    struct tg_decision d;

    sim_init(&s, 1);
    s.avail = MIB(300); /* 29%: 여유가 충분하다 */
    tg_judge_trigger(&s.j);

    /* 압력은 높지만 여유 메모리가 있으면 종료하지 않는다. 같은 사유는 한 번만 알린다 */
    d = run_until(&s, 100, 60);
    TG_CHECK_EQ_U64(s.ticks, 41);
    TG_CHECK(d.kind == TG_J_SKIP);
    TG_CHECK(d.reason == TG_R_MEM_AVAILABLE);
    TG_CHECK(d.avail_x100 > 2800 && d.avail_x100 < 3000);
    d = run_until(&s, 20, 60);
    TG_CHECK(d.kind == TG_J_WATCHING);

    /* 여유가 보호선 아래로 내려가면 바로 종료 결정 */
    s.avail = MIB(100); /* 9.8% */
    d = sim_tick(&s, 60);
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK(d.avail_x100 <= 1000);
}

static void test_mem_total_zero(void)
{
    struct sim s;
    struct tg_decision d;

    /* 전체 메모리를 모르면(0) 종료하지 않는다 */
    sim_init(&s, 1);
    s.total = 0;
    tg_judge_trigger(&s.j);
    d = run_until(&s, 100, 60);
    TG_CHECK(d.kind == TG_J_SKIP);
    TG_CHECK(d.reason == TG_R_MEM_AVAILABLE);
}

static void test_growing_beats_big(void)
{
    struct sim s;
    struct tg_decision d;

    /* A: 윈도 이전에 600 MiB를 잡고 가만히 있음, B: 정상 앱처럼 서서히 증가(약 +39 MiB) */
    sim_init(&s, 2);
    tg_judge_trigger(&s.j);
    memset(&d, 0, sizeof(d));
    for (int t = 1; t <= 41; t++) {
        s.mem[0] = MIB(600);
        s.mem[1] = MIB(150) + MIB(1) * (uint64_t)t;
        d = sim_tick(&s, 60);
    }
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK(d.cand_index == 1);
    TG_CHECK(d.cand_growing);
    TG_CHECK_EQ_U64(d.cand_growth_bytes, MIB(39));
}

static void test_larger_growth_wins(void)
{
    struct sim s;
    struct tg_decision d;

    /* 둘 다 늘어나면 증가량이 큰 쪽(크기가 아니라) */
    sim_init(&s, 2);
    tg_judge_trigger(&s.j);
    memset(&d, 0, sizeof(d));
    for (int t = 1; t <= 41; t++) {
        s.mem[0] = MIB(500) + MIB(1) * (uint64_t)t;
        s.mem[1] = MIB(100) + MIB(3) * (uint64_t)t;
        d = sim_tick(&s, 60);
    }
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK(d.cand_index == 1);
    TG_CHECK_EQ_U64(d.cand_growth_bytes, MIB(117));
}

static void test_growth_threshold(void)
{
    struct sim s;
    struct tg_decision d;

    /* 증가량이 전체의 1%(약 10.2 MiB) 미만이면 증가 중으로 보지 않고 크기로 고른다 */
    sim_init(&s, 2);
    tg_judge_trigger(&s.j);
    memset(&d, 0, sizeof(d));
    for (int t = 1; t <= 41; t++) {
        s.mem[0] = MIB(600);
        s.mem[1] = MIB(150) + (uint64_t)t * 131072ull; /* 0.125 MiB씩, 윈도 증가 약 4.9 MiB */
        d = sim_tick(&s, 60);
    }
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK(d.cand_index == 0);
    TG_CHECK(!d.cand_growing);
}

static void test_size_tiebreak_and_order(void)
{
    struct sim s;
    struct tg_decision d;

    /* 증가 중인 후보가 없으면 가장 큰 것 */
    sim_init(&s, 3);
    s.mem[0] = MIB(200);
    s.mem[1] = MIB(500);
    s.mem[2] = MIB(300);
    tg_judge_trigger(&s.j);
    d = run_until(&s, 100, 60);
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK(d.cand_index == 1);

    /* 크기도 같으면 앞 번호 */
    sim_init(&s, 3);
    tg_judge_trigger(&s.j);
    d = run_until(&s, 100, 60);
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK(d.cand_index == 0);
}

static void test_burst_uses_window_max(void)
{
    struct sim s;
    struct tg_decision d;

    /* A는 윈도 안에서 300 MiB를 잡았다 놓기를 반복한다. 마지막 샘플이 해제 구간이어도 최대로 자격을 판단한다 */
    sim_init(&s, 1);
    tg_judge_trigger(&s.j);
    memset(&d, 0, sizeof(d));
    for (int t = 1; t <= 41; t++) {
        s.mem[0] = (t % 8 < 4) ? MIB(300) : MIB(20);
        d = sim_tick(&s, 60);
    }
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK_EQ_U64(d.cand_max_bytes, MIB(300));
}

static void test_no_culprit_then_found(void)
{
    struct sim s;
    struct tg_decision d;

    /* 시스템은 힘든데 모든 후보가 자격(전체의 10%)에 못 미친다 */
    sim_init(&s, 2);
    s.mem[0] = MIB(50);
    s.mem[1] = MIB(60);
    tg_judge_trigger(&s.j);
    d = run_until(&s, 100, 60);
    TG_CHECK_EQ_U64(s.ticks, 41);
    TG_CHECK(d.kind == TG_J_SKIP);
    TG_CHECK(d.reason == TG_R_NO_CULPRIT);

    /* A가 커지면 윈도 최대가 바로 올라 다음 샘플에 종료 결정 */
    s.mem[0] = MIB(400);
    d = run_until(&s, 100, 60);
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK(d.cand_index == 0);
    TG_CHECK_EQ_U64(s.ticks, 42);
}

static void test_protected_excluded(void)
{
    struct sim s;
    struct tg_decision d;

    /* 보호된 A가 더 크지만 B를 고른다 */
    sim_init(&s, 2);
    s.mem[0] = MIB(600);
    s.mem[1] = MIB(300);
    s.prot[0] = true;
    tg_judge_trigger(&s.j);
    d = run_until(&s, 100, 60);
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK(d.cand_index == 1);

    /* 후보가 모두 보호되면 아무도 종료하지 않는다 */
    sim_init(&s, 2);
    s.prot[0] = true;
    s.prot[1] = true;
    tg_judge_trigger(&s.j);
    d = run_until(&s, 100, 60);
    TG_CHECK(d.kind == TG_J_SKIP);
    TG_CHECK(d.reason == TG_R_NO_CANDIDATE);
    d = run_until(&s, 100, 60);
    TG_CHECK(d.kind == TG_J_WATCHING);
}

static void test_grace_excludes_candidate(void)
{
    struct sim s;
    struct tg_decision d;

    sim_init(&s, 1);
    s.first_seen[0] = s.now; /* 방금 생긴 컨테이너 */
    tg_judge_trigger(&s.j);

    d = run_until(&s, 100, 60);
    TG_CHECK_EQ_U64(s.ticks, 41);
    TG_CHECK(d.kind == TG_J_SKIP);
    TG_CHECK(d.reason == TG_R_NO_CANDIDATE);

    /* 같은 사유는 다시 알리지 않고, 유예(30초 = 120샘플)가 끝나는 순간 종료 결정이 난다 */
    d = run_until(&s, 200, 60);
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK_EQ_U64(s.ticks, 120);
}

static void test_policy_blocks(void)
{
    struct sim s;
    struct tg_decision d;

    /* 쿨다운: 관찰 시작 시점에 방금 종료한 기록이 있다 */
    sim_init(&s, 1);
    tg_policy_record_kill(&s.pol, s.now);
    tg_judge_trigger(&s.j);
    d = run_until(&s, 100, 60);
    TG_CHECK_EQ_U64(s.ticks, 41);
    TG_CHECK(d.kind == TG_J_SKIP);
    TG_CHECK(d.reason == TG_R_COOLDOWN);
    /* 같은 사유는 반복하지 않고, 쿨다운 60초(240샘플)가 끝나는 순간 종료 결정 */
    d = run_until(&s, 400, 60);
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK_EQ_U64(s.ticks, 240);

    /* 시간당 상한: 쿨다운은 지났지만 1시간 안에 이미 상한만큼 종료했다 */
    sim_init(&s, 1);
    tg_policy_init(&s.pol, 60, 1);
    tg_policy_record_kill(&s.pol, s.now - SEC(70));
    tg_judge_trigger(&s.j);
    d = run_until(&s, 100, 60);
    TG_CHECK(d.kind == TG_J_SKIP);
    TG_CHECK(d.reason == TG_R_HOURLY_LIMIT);
    d = run_until(&s, 400, 60);
    TG_CHECK(d.kind == TG_J_WATCHING); /* 100초가 더 지나도 종료하지 않는다 */
}

static void test_counter_reset(void)
{
    struct sim s;
    struct tg_decision d;

    /* 21번째 샘플에서 누적 카운터가 0으로 돌아간다. 증가분 0으로 본다. */
    sim_init(&s, 1);
    tg_judge_trigger(&s.j);
    memset(&d, 0, sizeof(d));
    for (int t = 1; t <= 41; t++) {
        if (t == 21)
            s.sys_us = 0;
        d = sim_tick(&s, 50);
    }
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK_EQ_U64(d.sys_avg_x100, 4875); /* 39칸 x 50% / 40칸 */
}

static void test_candidate_disappears(void)
{
    struct sim s;
    struct tg_decision d;

    /* 30번째 샘플에서 후보가 한 번 사라졌다 돌아오면 그 후보의 윈도는 처음부터 다시 쌓는다 */
    sim_init(&s, 1);
    tg_judge_trigger(&s.j);
    d = run_until(&s, 29, 60);
    TG_CHECK(d.kind == TG_J_WATCHING);
    s.absent = 1;
    d = sim_tick(&s, 60);
    TG_CHECK(d.kind == TG_J_WATCHING);
    s.absent = 0;

    d = run_until(&s, 100, 60);
    TG_CHECK_EQ_U64(s.ticks, 41);
    TG_CHECK(d.kind == TG_J_SKIP);
    TG_CHECK(d.reason == TG_R_NO_CANDIDATE);

    d = run_until(&s, 100, 60);
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK_EQ_U64(s.ticks, 70); /* 복귀한 31번째 샘플부터 40칸이 모이는 70번째 */
}

static void test_irregular_interval(void)
{
    struct sim s;
    struct tg_sample smp;
    struct tg_cand_sample c;
    struct tg_decision d;
    uint64_t now = SEC(1000);
    uint64_t sys = 0;
    int ticks = 0;

    /*
     * 데몬이 밀려 샘플 간격이 500 ms로 늘어도 평균은 실제 경과 시간으로 나눈다.
     * 500 ms 동안 200 ms stall이면 40%다.
     */
    sim_init(&s, 1);
    tg_judge_trigger(&s.j);
    c.name = "A";
    c.mem_bytes = MIB(400);
    c.first_seen_ns = 0;
    c.is_protected = false;
    smp.cand = &c;
    smp.ncand = 1;
    smp.mem_available_bytes = MIB(50);
    smp.mem_total_bytes = MIB(1024);
    memset(&d, 0, sizeof(d));
    while (d.kind == TG_J_WATCHING || ticks == 0) {
        now += 2 * TICK_NS;
        sys += 200000;
        smp.now_ns = now;
        smp.sys_full_total_us = sys;
        d = tg_judge_tick(&s.j, &smp, &s.pol);
        ticks++;
        if (ticks > 100)
            break;
    }
    TG_CHECK(d.kind == TG_J_KILL);
    TG_CHECK_EQ_U64(d.sys_avg_x100, 4000);
}

static void test_names(void)
{
    TG_CHECK(strcmp(tg_judge_reason_name(TG_R_NO_CULPRIT), "no_culprit") == 0);
    TG_CHECK(strcmp(tg_judge_reason_name(TG_R_CALM), "calm") == 0);
    TG_CHECK(strcmp(tg_judge_reason_name(TG_R_MEM_AVAILABLE), "mem_available") == 0);
    TG_CHECK(strcmp(tg_judge_kind_name(TG_J_KILL), "kill") == 0);
    TG_CHECK(strcmp(tg_judge_kind_name(TG_J_SKIP), "skip") == 0);
}

int main(void)
{
    TG_RUN(test_idle_without_trigger);
    TG_RUN(test_sustained_thrash_kills);
    TG_RUN(test_short_burst_calm);
    TG_RUN(test_known_weakness_long_burst);
    TG_RUN(test_fluctuating_thrash_detected);
    TG_RUN(test_gradual_rise);
    TG_RUN(test_mem_available_guard);
    TG_RUN(test_mem_total_zero);
    TG_RUN(test_growing_beats_big);
    TG_RUN(test_larger_growth_wins);
    TG_RUN(test_growth_threshold);
    TG_RUN(test_size_tiebreak_and_order);
    TG_RUN(test_burst_uses_window_max);
    TG_RUN(test_no_culprit_then_found);
    TG_RUN(test_protected_excluded);
    TG_RUN(test_grace_excludes_candidate);
    TG_RUN(test_policy_blocks);
    TG_RUN(test_counter_reset);
    TG_RUN(test_candidate_disappears);
    TG_RUN(test_irregular_interval);
    TG_RUN(test_names);
    return tg_test_finish();
}
