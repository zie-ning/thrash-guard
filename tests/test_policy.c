/*
 * test_policy.c - 유예, 쿨다운, 시간당 상한 단위 테스트
 */
#include <string.h>

#include "policy.h"
#include "tg_test.h"

#define SEC(x) ((uint64_t)(x) * 1000000000ull)

static void test_grace(void)
{
    /* 100초에 발견, 유예 30초 */
    TG_CHECK(tg_policy_in_grace(SEC(100), SEC(100), 30));
    TG_CHECK(tg_policy_in_grace(SEC(100), SEC(129), 30));
    TG_CHECK(tg_policy_in_grace(SEC(100), SEC(130) - 1, 30));
    TG_CHECK(!tg_policy_in_grace(SEC(100), SEC(130), 30));
    TG_CHECK(!tg_policy_in_grace(SEC(100), SEC(5000), 30));

    /* 시계 이상: 발견 시각보다 이르면 안전하게 유예 중 */
    TG_CHECK(tg_policy_in_grace(SEC(100), SEC(50), 30));

    /* 유예 0초면 발견 즉시 대상 */
    TG_CHECK(!tg_policy_in_grace(SEC(100), SEC(100), 0));

    /* 데몬 시작 때 있던 항목(first_seen=0)은 부팅 30초 뒤부터 유예가 끝난 것으로 본다 */
    TG_CHECK(!tg_policy_in_grace(0, SEC(3600), 30));
}

static void test_cooldown(void)
{
    struct tg_policy p;

    tg_policy_init(&p, 60, 3);
    TG_CHECK(tg_policy_check(&p, SEC(1000)) == TG_POLICY_OK);

    tg_policy_record_kill(&p, SEC(1000));
    TG_CHECK(tg_policy_check(&p, SEC(1000)) == TG_POLICY_COOLDOWN);
    TG_CHECK(tg_policy_check(&p, SEC(1059)) == TG_POLICY_COOLDOWN);
    TG_CHECK(tg_policy_check(&p, SEC(1060)) == TG_POLICY_OK);

    /* 시계 이상: 마지막 종료보다 이른 시각은 쿨다운 중 */
    TG_CHECK(tg_policy_check(&p, SEC(900)) == TG_POLICY_COOLDOWN);
}

static void test_hourly_limit(void)
{
    struct tg_policy p;

    tg_policy_init(&p, 60, 3);
    tg_policy_record_kill(&p, SEC(0));
    tg_policy_record_kill(&p, SEC(100));
    tg_policy_record_kill(&p, SEC(200));

    /* 쿨다운은 지났지만 상한에 도달 */
    TG_CHECK(tg_policy_check(&p, SEC(260)) == TG_POLICY_HOURLY_LIMIT);
    TG_CHECK(tg_policy_check(&p, SEC(3599)) == TG_POLICY_HOURLY_LIMIT);

    /* 정확히 1시간이 지난 종료는 세지 않는다 */
    TG_CHECK(tg_policy_check(&p, SEC(3600)) == TG_POLICY_OK);

    /* 쿨다운이 상한보다 먼저 판정된다 */
    TG_CHECK(tg_policy_check(&p, SEC(210)) == TG_POLICY_COOLDOWN);
}

static void test_zero_cooldown(void)
{
    struct tg_policy p;

    tg_policy_init(&p, 0, 2);
    tg_policy_record_kill(&p, SEC(10));
    TG_CHECK(tg_policy_check(&p, SEC(10)) == TG_POLICY_OK);
    tg_policy_record_kill(&p, SEC(10));
    TG_CHECK(tg_policy_check(&p, SEC(10)) == TG_POLICY_HOURLY_LIMIT);
}

static void test_ring_wrap(void)
{
    struct tg_policy p;

    /* 링(100칸)을 넘겨 기록해도 최근 100건만 정확히 센다 */
    tg_policy_init(&p, 0, 100);
    for (int i = 1; i <= 150; i++)
        tg_policy_record_kill(&p, SEC(i));
    TG_CHECK_EQ_U64(p.count, TG_POLICY_RING);
    TG_CHECK(tg_policy_check(&p, SEC(151)) == TG_POLICY_HOURLY_LIMIT);
    /* 51..150이 남아 있다. 51의 1시간 뒤(3651)에는 51이 빠져 99건 */
    TG_CHECK(tg_policy_check(&p, SEC(51) + SEC(3600)) == TG_POLICY_OK);
    TG_CHECK(tg_policy_check(&p, SEC(51) + SEC(3600) - 1) == TG_POLICY_HOURLY_LIMIT);

    /* 상한이 링보다 크게 설정되어도 링 크기로 줄인다 */
    tg_policy_init(&p, 0, 500);
    TG_CHECK_EQ_U64(p.max_kills_per_hour, TG_POLICY_RING);
}

static void test_verdict_name(void)
{
    TG_CHECK(strcmp(tg_policy_verdict_name(TG_POLICY_OK), "ok") == 0);
    TG_CHECK(strcmp(tg_policy_verdict_name(TG_POLICY_COOLDOWN), "cooldown") == 0);
    TG_CHECK(strcmp(tg_policy_verdict_name(TG_POLICY_HOURLY_LIMIT), "hourly_limit") == 0);
}

int main(void)
{
    TG_RUN(test_grace);
    TG_RUN(test_cooldown);
    TG_RUN(test_hourly_limit);
    TG_RUN(test_zero_cooldown);
    TG_RUN(test_ring_wrap);
    TG_RUN(test_verdict_name);
    return tg_test_finish();
}
