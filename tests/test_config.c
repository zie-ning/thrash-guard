/*
 * test_config.c - 데몬 설정 파서·검증 단위 테스트
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "tg_test.h"

static int parse(const char *s, struct tg_config *c, struct tg_cfg_err *e)
{
    tg_config_defaults(c);
    return tg_config_parse(s, strlen(s), c, e);
}

static void test_defaults(void)
{
    struct tg_config c;
    struct tg_cfg_err e;

    tg_config_defaults(&c);
    /* 초기값 */
    TG_CHECK_EQ_U64(c.trigger_stall_us, 150000);
    TG_CHECK_EQ_U64(c.trigger_window_us, 1000000);
    TG_CHECK_EQ_U64(c.full_pct, 10);
    TG_CHECK_EQ_U64(c.mem_avail_pct, 10);
    TG_CHECK_EQ_U64(c.min_size_pct, 10);
    TG_CHECK_EQ_U64(c.min_growth_pct, 1);
    TG_CHECK_EQ_U64(c.sustain_sec, 10);
    TG_CHECK_EQ_U64(c.sample_interval_ms, 250);
    TG_CHECK_EQ_U64(c.startup_grace_sec, 30);
    TG_CHECK_EQ_U64(c.cooldown_sec, 60);
    TG_CHECK_EQ_U64(c.max_kills_per_hour, 3);
    TG_CHECK(c.dry_run);
    TG_CHECK(c.mlock);
    TG_CHECK(c.oom_score_adj == -1000);
    TG_CHECK(strcmp(c.target_parent, "/sys/fs/cgroup/system.slice") == 0);
    /* 기본 후보는 컨테이너 런타임의 cgroup 이름뿐이다 */
    TG_CHECK(strstr(c.target_match, "docker-*.scope") != NULL);
    TG_CHECK(strchr(c.target_match, ' ') == NULL);
    TG_CHECK_EQ_U64(tg_config_window_samples(&c), 40);
    TG_CHECK(tg_config_validate(&c, &e) == 0);
}

static void test_parse_basic(void)
{
    struct tg_config c;
    struct tg_cfg_err e;
    const char *text = "# 주석\n"
                       "\n"
                       "full_pct = 20\n"
                       "  sustain_sec=5\t\n"
                       "target_parent=/sys/fs/cgroup/test.slice\n"
                       "mlock=no\n"
                       "oom_score_adj=-500\n"
                       "log_path=/var/log/tg.log\n";

    TG_CHECK(parse(text, &c, &e) == 0);
    TG_CHECK_EQ_U64(c.full_pct, 20);
    TG_CHECK_EQ_U64(c.sustain_sec, 5);
    TG_CHECK(strcmp(c.target_parent, "/sys/fs/cgroup/test.slice") == 0);
    TG_CHECK(!c.mlock);
    TG_CHECK(c.oom_score_adj == -500);
    TG_CHECK(strcmp(c.log_path, "/var/log/tg.log") == 0);
    /* 건드리지 않은 키는 기본값 그대로 */
    TG_CHECK_EQ_U64(c.cooldown_sec, 60);
    TG_CHECK(tg_config_validate(&c, &e) == 0);
}

static void test_parse_format_quirks(void)
{
    struct tg_config c;
    struct tg_cfg_err e;

    /* 줄바꿈 없는 마지막 줄 */
    TG_CHECK(parse("full_pct=15", &c, &e) == 0);
    TG_CHECK_EQ_U64(c.full_pct, 15);

    /* CRLF */
    TG_CHECK(parse("full_pct=16\r\ncooldown_sec=7\r\n", &c, &e) == 0);
    TG_CHECK_EQ_U64(c.full_pct, 16);
    TG_CHECK_EQ_U64(c.cooldown_sec, 7);

    /* 같은 키는 나중 값이 이긴다 */
    TG_CHECK(parse("full_pct=11\nfull_pct=12\n", &c, &e) == 0);
    TG_CHECK_EQ_U64(c.full_pct, 12);

    /* 빈 입력 */
    TG_CHECK(parse("", &c, &e) == 0);
    TG_CHECK_EQ_U64(c.full_pct, 10);

    /* 빈 문자열 값 허용(log_path) */
    TG_CHECK(parse("log_path=\n", &c, &e) == 0);
    TG_CHECK(c.log_path[0] == '\0');
}

static void test_unknown_key(void)
{
    struct tg_config c;
    struct tg_cfg_err e;

    /* 오타가 조용히 무시되면 안 된다 */
    TG_CHECK(parse("full_pct=10\ncooldwon_sec=5\n", &c, &e) == -EINVAL);
    TG_CHECK(e.line == 2);
    TG_CHECK(strcmp(e.key, "cooldwon_sec") == 0);
    TG_CHECK(e.msg != NULL);
}

static void test_bad_values(void)
{
    struct tg_config c;
    struct tg_cfg_err e;

    TG_CHECK(parse("full_pct=abc\n", &c, &e) == -EINVAL);
    TG_CHECK(parse("full_pct=10abc\n", &c, &e) == -EINVAL);
    TG_CHECK(parse("full_pct=\n", &c, &e) == -EINVAL);
    TG_CHECK(parse("full_pct=-5\n", &c, &e) == -EINVAL);
    TG_CHECK(parse("full_pct=0\n", &c, &e) == -EINVAL);
    TG_CHECK(parse("full_pct=101\n", &c, &e) == -EINVAL);
    TG_CHECK(parse("full_pct=99999999999999999999\n", &c, &e) == -EINVAL);
    TG_CHECK(parse("mlock=maybe\n", &c, &e) == -EINVAL);
    TG_CHECK(parse("oom_score_adj=-1001\n", &c, &e) == -EINVAL);
    TG_CHECK(parse("oom_score_adj=1001\n", &c, &e) == -EINVAL);
    TG_CHECK(parse("oom_score_adj=-\n", &c, &e) == -EINVAL);
    TG_CHECK(parse("trigger_window_us=499999\n", &c, &e) == -EINVAL);
    TG_CHECK(parse("trigger_window_us=10000001\n", &c, &e) == -EINVAL);
    TG_CHECK(parse("max_kills_per_hour=0\n", &c, &e) == -EINVAL);

    /* 경계값은 통과 */
    TG_CHECK(parse("full_pct=1\n", &c, &e) == 0);
    TG_CHECK(parse("full_pct=100\n", &c, &e) == 0);
    TG_CHECK(parse("trigger_window_us=500000\n", &c, &e) == 0);
    TG_CHECK(parse("trigger_window_us=10000000\n", &c, &e) == 0);
    TG_CHECK(parse("oom_score_adj=-1000\n", &c, &e) == 0);
    TG_CHECK(parse("oom_score_adj=1000\n", &c, &e) == 0);
    TG_CHECK(parse("startup_grace_sec=0\n", &c, &e) == 0);
}

static void test_bad_syntax(void)
{
    struct tg_config c;
    struct tg_cfg_err e;
    char longkey[80];
    char longval[400];
    char text[512];

    TG_CHECK(parse("full_pct\n", &c, &e) == -EINVAL);
    TG_CHECK(e.line == 1);
    TG_CHECK(parse("=10\n", &c, &e) == -EINVAL);

    /* 너무 긴 키는 잘려서 기록되고 거부된다 */
    memset(longkey, 'k', sizeof(longkey) - 1);
    longkey[sizeof(longkey) - 1] = '\0';
    snprintf(text, sizeof(text), "%s=1\n", longkey);
    TG_CHECK(parse(text, &c, &e) == -EINVAL);
    TG_CHECK(strlen(e.key) == sizeof(e.key) - 1);

    /* 너무 긴 문자열 값 */
    memset(longval, '/', sizeof(longval) - 1);
    longval[sizeof(longval) - 1] = '\0';
    snprintf(text, sizeof(text), "target_parent=%s\n", longval);
    TG_CHECK(parse(text, &c, &e) == -EINVAL);

    /* 오류 줄 번호: 주석·빈 줄도 센다 */
    TG_CHECK(parse("# a\n\nfull_pct=5\nbogus=1\n", &c, &e) == -EINVAL);
    TG_CHECK(e.line == 4);
}

static void test_target_match(void)
{
    struct tg_config c;
    struct tg_cfg_err e;

    /* 사용자가 서비스 이름을 직접 적는다 */
    TG_CHECK(parse("target_match = myapp.service, worker@*.service\n", &c, &e) == 0);
    TG_CHECK(strcmp(c.target_match, "myapp.service, worker@*.service") == 0);
    TG_CHECK(parse("target_match=*\n", &c, &e) == 0);

    /* 잘못된 패턴은 파싱은 되지만 검증에서 시작을 거부한다 */
    TG_CHECK(parse("target_match=\n", &c, &e) == 0);
    TG_CHECK(tg_config_validate(&c, &e) == -EINVAL);
    TG_CHECK(strcmp(e.key, "target_match") == 0);
    TG_CHECK(parse("target_match=a,,b\n", &c, &e) == 0);
    TG_CHECK(tg_config_validate(&c, &e) == -EINVAL);
    TG_CHECK(parse("target_match=a/b\n", &c, &e) == 0);
    TG_CHECK(tg_config_validate(&c, &e) == -EINVAL);
    TG_CHECK(parse("target_match=a,\n", &c, &e) == 0);
    TG_CHECK(tg_config_validate(&c, &e) == -EINVAL);
    TG_CHECK(parse("target_match=myapp.service\n", &c, &e) == 0);
    TG_CHECK(tg_config_validate(&c, &e) == 0);
}

static void test_dry_run_policy(void)
{
    struct tg_config c;
    struct tg_cfg_err e;

    /* 설정 파일로는 dry-run을 끌 수 없다 */
    TG_CHECK(parse("dry_run=no\n", &c, &e) == -EPERM);
    TG_CHECK(e.line == 1);
    TG_CHECK(strcmp(e.key, "dry_run") == 0);
    TG_CHECK(c.dry_run);

    TG_CHECK(parse("dry_run=yes\n", &c, &e) == 0);
    TG_CHECK(c.dry_run);
    TG_CHECK(parse("dry_run=maybe\n", &c, &e) == -EINVAL);
}

static void test_validate(void)
{
    struct tg_config c;
    struct tg_cfg_err e;

    tg_config_defaults(&c);

    /* target_parent는 깨끗한 절대 경로여야 한다 */
    strcpy(c.target_parent, "workload.slice");
    TG_CHECK(tg_config_validate(&c, &e) == -EINVAL);
    TG_CHECK(strcmp(e.key, "target_parent") == 0);
    strcpy(c.target_parent, "/sys/fs/cgroup/../etc");
    TG_CHECK(tg_config_validate(&c, &e) == -EINVAL);
    strcpy(c.target_parent, "/sys/fs/cgroup/workload.slice/");
    TG_CHECK(tg_config_validate(&c, &e) == -EINVAL);
    strcpy(c.target_parent, "/sys//fs");
    TG_CHECK(tg_config_validate(&c, &e) == -EINVAL);
    strcpy(c.target_parent, "/");
    TG_CHECK(tg_config_validate(&c, &e) == -EINVAL);
    strcpy(c.target_parent, "/sys/fs/cgroup/./x");
    TG_CHECK(tg_config_validate(&c, &e) == -EINVAL);
    strcpy(c.target_parent, "/sys/fs/cgroup/a..b");
    TG_CHECK(tg_config_validate(&c, &e) == 0);

    tg_config_defaults(&c);
    strcpy(c.log_path, "relative.log");
    TG_CHECK(tg_config_validate(&c, &e) == -EINVAL);
    TG_CHECK(strcmp(e.key, "log_path") == 0);

    /* stall은 window를 넘을 수 없다 (커널도 거부함, WSL 실측) */
    tg_config_defaults(&c);
    c.trigger_stall_us = 1000001;
    TG_CHECK(tg_config_validate(&c, &e) == -EINVAL);
    TG_CHECK(strcmp(e.key, "trigger_stall_us") == 0);
    c.trigger_stall_us = 1000000;
    TG_CHECK(tg_config_validate(&c, &e) == 0);

    /* 샘플 주기는 sustain_sec을 나누어떨어뜨려야 한다 */
    tg_config_defaults(&c);
    c.sample_interval_ms = 300;
    TG_CHECK(tg_config_validate(&c, &e) == -EINVAL);
    TG_CHECK(strcmp(e.key, "sample_interval_ms") == 0);

    /* 고정 배열 칸 수 상한 */
    tg_config_defaults(&c);
    c.sustain_sec = 300;
    c.sample_interval_ms = 50; /* 6000칸 */
    TG_CHECK(tg_config_validate(&c, &e) == -EINVAL);
    c.sample_interval_ms = 250; /* 1200칸 */
    TG_CHECK(tg_config_validate(&c, &e) == -EINVAL);
    c.sample_interval_ms = 1000; /* 300칸: 상한 256 초과 */
    TG_CHECK(tg_config_validate(&c, &e) == -EINVAL);
    c.sustain_sec = 100;
    c.sample_interval_ms = 500; /* 200칸 */
    TG_CHECK(tg_config_validate(&c, &e) == 0);
    TG_CHECK_EQ_U64(tg_config_window_samples(&c), 200);
    c.sustain_sec = 64;
    c.sample_interval_ms = 250; /* 256칸: 경계 */
    TG_CHECK(tg_config_validate(&c, &e) == 0);
}

static void test_validate_zero(void)
{
    struct tg_config c;
    struct tg_cfg_err e;

    /* 구조체를 직접 채워도 0으로 나누지 않고 거부한다 */
    tg_config_defaults(&c);
    c.sample_interval_ms = 0;
    TG_CHECK(tg_config_validate(&c, &e) == -EINVAL);
    TG_CHECK(strcmp(e.key, "sample_interval_ms") == 0);

    tg_config_defaults(&c);
    c.sustain_sec = 0;
    TG_CHECK(tg_config_validate(&c, &e) == -EINVAL);
    TG_CHECK(strcmp(e.key, "sustain_sec") == 0);
}

static void test_load(void)
{
    struct tg_config c;
    struct tg_cfg_err e;
    char path[256];

    tg_config_defaults(&c);
    TG_CHECK(tg_config_load("/nonexistent/tg.conf", &c, &e) == -ENOENT);
    TG_CHECK(e.msg != NULL);

    snprintf(path, sizeof(path), "%s/config/valid.conf", TG_FIXTURE_DIR);
    tg_config_defaults(&c);
    TG_CHECK(tg_config_load(path, &c, &e) == 0);
    TG_CHECK_EQ_U64(c.full_pct, 12);
    TG_CHECK_EQ_U64(c.sustain_sec, 20);
    TG_CHECK(tg_config_validate(&c, &e) == 0);

    snprintf(path, sizeof(path), "%s/config/unknown_key.conf", TG_FIXTURE_DIR);
    tg_config_defaults(&c);
    TG_CHECK(tg_config_load(path, &c, &e) == -EINVAL);
    TG_CHECK(e.line == 3);
    TG_CHECK(strcmp(e.key, "cooldwon_sec") == 0);
}

int main(void)
{
    TG_RUN(test_defaults);
    TG_RUN(test_parse_basic);
    TG_RUN(test_parse_format_quirks);
    TG_RUN(test_unknown_key);
    TG_RUN(test_bad_values);
    TG_RUN(test_bad_syntax);
    TG_RUN(test_target_match);
    TG_RUN(test_dry_run_policy);
    TG_RUN(test_validate);
    TG_RUN(test_validate_zero);
    TG_RUN(test_load);
    return tg_test_finish();
}
