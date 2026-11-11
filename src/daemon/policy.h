/*
 * policy.h - 종료 안전장치 (유예, 쿨다운, 시간당 상한)
 *
 * 시각을 인자로 받는 순수 함수라서 실제 시계 없이 단위 테스트할 수 있다.
 * 시각은 모두 단조 시계 나노초다. 힙 할당을 하지 않는다.
 *
 * dry-run에서도 "종료했다면" 기록(tg_policy_record_kill)을 남겨 쿨다운과 상한을
 * 소모한다(D21). 그래야 dry-run 로그가 실제 모드의 동작을 그대로 보여 준다.
 */
#ifndef TG_POLICY_H
#define TG_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define TG_POLICY_RING 100 /* config의 max_kills_per_hour 최댓값과 같다 */

struct tg_policy {
    uint64_t kill_ns[TG_POLICY_RING]; /* 최근 종료 시각을 돌려 가며 보관 */
    uint32_t head;                    /* 다음에 쓸 칸 */
    uint32_t count;                   /* 기록된 개수 (최대 TG_POLICY_RING) */
    uint32_t cooldown_sec;
    uint32_t max_kills_per_hour;
};

enum tg_policy_verdict {
    TG_POLICY_OK = 0,
    TG_POLICY_COOLDOWN,     /* 마지막 종료 후 쿨다운 중 */
    TG_POLICY_HOURLY_LIMIT, /* 최근 1시간 종료 횟수가 상한에 도달 */
};

/* max_kills_per_hour가 TG_POLICY_RING보다 크면 TG_POLICY_RING으로 줄인다. */
void tg_policy_init(struct tg_policy *p, uint32_t cooldown_sec, uint32_t max_kills_per_hour);

/* 지금 종료해도 되는지. 쿨다운이 상한보다 먼저 판정된다. */
enum tg_policy_verdict tg_policy_check(const struct tg_policy *p, uint64_t now_ns);

/* 종료(또는 dry-run의 가상 종료)를 기록한다. */
void tg_policy_record_kill(struct tg_policy *p, uint64_t now_ns);

/*
 * 컨테이너가 처음 발견된 지 grace_sec이 지나지 않았으면 true.
 * now가 first_seen보다 이르면(시계 이상) 안전하게 true로 본다.
 * 데몬 시작 때 이미 있던 항목은 first_seen_ns=0이라 부팅 직후가 아니면 유예가 끝난 것으로 본다.
 */
bool tg_policy_in_grace(uint64_t first_seen_ns, uint64_t now_ns, uint32_t grace_sec);

const char *tg_policy_verdict_name(enum tg_policy_verdict v);

#endif /* TG_POLICY_H */
