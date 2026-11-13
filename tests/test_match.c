/*
 * test_match.c - cgroup 이름 패턴(target_match) 단위 테스트
 */
#include <string.h>

#include "match.h"
#include "tg_test.h"

static void test_glob(void)
{
    TG_CHECK(tg_match_name("docker-*.scope", "docker-0123abcd.scope"));
    TG_CHECK(!tg_match_name("docker-*.scope", "docker-0123abcd.service"));
    TG_CHECK(!tg_match_name("docker-*.scope", "xdocker-1.scope"));
    TG_CHECK(tg_match_name("docker-*.scope", "docker-.scope")); /* '*'는 0글자도 된다 */
    TG_CHECK(tg_match_name("myapp.service", "myapp.service"));
    TG_CHECK(!tg_match_name("myapp.service", "myapp.service.d"));
    TG_CHECK(!tg_match_name("myapp.service", "myapp"));
    TG_CHECK(tg_match_name("*", "anything.service"));
    TG_CHECK(tg_match_name("*.service", "a.b.service"));
    TG_CHECK(tg_match_name("a*b*c", "aXXbYYc"));
    TG_CHECK(tg_match_name("a*b*c", "abc"));
    TG_CHECK(!tg_match_name("a*b*c", "aXXbYY"));
    TG_CHECK(tg_match_name("worker@*.service", "worker@3.service"));
    TG_CHECK(tg_match_name("**", "x")); /* 연속한 '*'도 안전하다 */
    TG_CHECK(!tg_match_name("a", ""));
    TG_CHECK(!tg_match_name("", "a"));
}

static void test_list(void)
{
    const char *p = "docker-*.scope, libpod-*.scope ,\tmyapp.service";

    TG_CHECK(tg_match_name(p, "docker-1.scope"));
    TG_CHECK(tg_match_name(p, "libpod-2.scope")); /* 패턴 앞뒤 공백은 무시한다 */
    TG_CHECK(tg_match_name(p, "myapp.service"));
    TG_CHECK(!tg_match_name(p, "sshd.service"));
    TG_CHECK(!tg_match_name(p, "docker-1.service"));
}

static void test_null_matches_all(void)
{
    TG_CHECK(tg_match_name(NULL, "anything"));
}

static void test_valid(void)
{
    char longp[200];

    TG_CHECK(tg_match_valid("*"));
    TG_CHECK(tg_match_valid("docker-*.scope"));
    TG_CHECK(tg_match_valid("a, b"));
    TG_CHECK(!tg_match_valid(NULL));
    TG_CHECK(!tg_match_valid(""));
    TG_CHECK(!tg_match_valid("   "));
    TG_CHECK(!tg_match_valid("a,,b"));
    TG_CHECK(!tg_match_valid(",a"));
    TG_CHECK(!tg_match_valid("a,"));
    TG_CHECK(!tg_match_valid("a , "));
    TG_CHECK(!tg_match_valid("a/b"));
    TG_CHECK(!tg_match_valid("../x"));
    memset(longp, 'x', sizeof(longp) - 1);
    longp[sizeof(longp) - 1] = '\0';
    TG_CHECK(!tg_match_valid(longp));
    memset(longp, 'x', TG_MATCH_NAME_MAX);
    longp[TG_MATCH_NAME_MAX] = '\0';
    TG_CHECK(tg_match_valid(longp)); /* 상한 길이는 허용한다 */
}

int main(void)
{
    TG_RUN(test_glob);
    TG_RUN(test_list);
    TG_RUN(test_null_matches_all);
    TG_RUN(test_valid);
    return tg_test_finish();
}
