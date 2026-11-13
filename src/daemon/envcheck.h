/*
 * envcheck.h - 시작 시 실행 환경 점검
 *
 * 종료 단계에서야 드러나는 환경 문제(cgroup v2가 아님, memory 컨트롤러 없음,
 * cgroup.kill 없음)를 시작할 때 미리 알린다. 힙 할당과 stdio를 쓰지 않는다.
 * 문구는 정적 문자열이다.
 */
#ifndef TG_ENVCHECK_H
#define TG_ENVCHECK_H

#include <stdbool.h>

#define TG_ENV_MAX_ISSUES 4

struct tg_env_report {
    int n;                                  /* 발견한 문제 수 */
    const char *issue[TG_ENV_MAX_ISSUES];   /* 문제 설명(정적 문자열) */
};

/*
 * target_parent가 cgroup v2이고 자식에게 memory 컨트롤러가 켜져 있는지 확인한다.
 * sample_child가 NULL이 아니면 그 자식의 cgroup.kill(커널 5.14 이상)이 있는지도 확인한다.
 * NULL이면 cgroup.kill은 확인하지 않는다. 문제가 없으면 r->n == 0.
 */
void tg_env_check(const char *parent, const char *sample_child, struct tg_env_report *r);

/* tg_env_check에서 cgroup v2 파일 시스템 검사를 뺀 나머지(테스트용). */
void tg_env_check_files(const char *parent, const char *sample_child, struct tg_env_report *r);

#endif /* TG_ENVCHECK_H */
