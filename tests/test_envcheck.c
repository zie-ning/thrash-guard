/*
 * test_envcheck.c - 시작 시 환경 점검 단위 테스트 (임시 디렉터리로 cgroup 모양을 흉내 낸다)
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "envcheck.h"
#include "tg_test.h"

static void put(const char *dir, const char *rel, const char *content)
{
    char p[512];
    FILE *f;

    snprintf(p, sizeof(p), "%s/%s", dir, rel);
    f = fopen(p, "w");
    TG_CHECK(f != NULL);
    if (f != NULL) {
        fputs(content, f);
        fclose(f);
    }
}

static void test_ok(void)
{
    char dir[] = "/tmp/tg_env_XXXXXX";
    char sub[600];
    struct tg_env_report r;

    TG_CHECK(mkdtemp(dir) != NULL);
    put(dir, "cgroup.subtree_control", "cpu memory pids\n");
    snprintf(sub, sizeof(sub), "%s/c.scope", dir);
    TG_CHECK(mkdir(sub, 0755) == 0);
    put(dir, "c.scope/cgroup.kill", "");

    r.n = 0;
    tg_env_check_files(dir, "c.scope", &r);
    TG_CHECK(r.n == 0);
}

static void test_problems(void)
{
    char dir[] = "/tmp/tg_env_XXXXXX";
    char sub[600];
    struct tg_env_report r;

    TG_CHECK(mkdtemp(dir) != NULL);
    put(dir, "cgroup.subtree_control", "cpu pids\n"); /* memory 없음 */
    snprintf(sub, sizeof(sub), "%s/c.scope", dir);
    TG_CHECK(mkdir(sub, 0755) == 0); /* cgroup.kill 없음 */

    r.n = 0;
    tg_env_check_files(dir, "c.scope", &r);
    TG_CHECK(r.n == 2);

    /* 자식을 주지 않으면 cgroup.kill은 보지 않는다 */
    r.n = 0;
    tg_env_check_files(dir, NULL, &r);
    TG_CHECK(r.n == 1);

    /* "memory"가 다른 단어의 일부면 켜진 것으로 보지 않는다 */
    put(dir, "cgroup.subtree_control", "hugetlb memory_x\n");
    r.n = 0;
    tg_env_check_files(dir, NULL, &r);
    TG_CHECK(r.n == 1);
}

static void test_not_cgroup2(void)
{
    struct tg_env_report r;

    tg_env_check("/tmp", NULL, &r);
    TG_CHECK(r.n == 1); /* tmpfs는 cgroup v2가 아니다 */
    tg_env_check("/nonexistent/tg", NULL, &r);
    TG_CHECK(r.n == 1);
}

int main(void)
{
    TG_RUN(test_ok);
    TG_RUN(test_problems);
    TG_RUN(test_not_cgroup2);
    return tg_test_finish();
}
