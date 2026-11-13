/*
 * match.h - cgroup 이름 패턴 (target_match)
 *
 * 쉼표로 구분한 이름 패턴 목록이다. 각 패턴은 앞뒤 공백을 무시하고 '*'(아무 글자 0개 이상)만 특별하다.
 * 예: "docker-*.scope, myapp.service". 힙 할당과 stdio를 쓰지 않는다.
 */
#ifndef TG_MATCH_H
#define TG_MATCH_H

#include <stdbool.h>
#include <stddef.h>

#define TG_MATCH_NAME_MAX 127 /* 패턴 한 개의 최대 길이. cgroup 이름 용량(128)에 맞춘다 */

/* name이 patterns 중 하나에 맞으면 true. patterns가 NULL이면 모든 이름에 맞는다. */
bool tg_match_name(const char *patterns, const char *name);

/*
 * 패턴 목록이 올바른지 검사한다: 비어 있지 않고, 빈 패턴이 없고, 패턴이 너무 길지 않고,
 * '/'를 포함하지 않는다(cgroup 이름에는 '/'가 없다). 올바르면 true.
 */
bool tg_match_valid(const char *patterns);

#endif /* TG_MATCH_H */
