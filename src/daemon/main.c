/*
 * main.c - thrash-guard 데몬 진입점
 *
 * 현재 범위(M3-1): CLI와 설정을 읽어 검증하고 유효 설정을 출력한다.
 * 이벤트 루프(M3-2) 이후 단계는 아직 없다.
 *
 * 이 파일은 초기화 단계라 stdio를 쓴다. 핵심 경로(이벤트 루프 이후)에서는 쓰지 않는다.
 */
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

enum { EXIT_USAGE = 2, EXIT_CONFIG = 3 };

static void usage(FILE *out)
{
    fputs("사용법: thrash-guard [옵션]\n"
          "  --config <경로>   설정 파일 (key=value). 없으면 기본값만 쓴다\n"
          "  --dry-run         판단만 기록하고 종료는 하지 않는다 (기본)\n"
          "  --no-dry-run      실제로 cgroup을 종료한다\n"
          "  --foreground      백그라운드로 전환하지 않는다\n"
          "  --help            이 도움말\n",
          out);
}

static void print_config(const struct tg_config *c, bool foreground)
{
    fprintf(stderr,
            "thrash-guard 유효 설정:\n"
            "  모드: %s%s\n"
            "  target_parent=%s\n"
            "  trigger: full %u/%u us\n"
            "  판단: 최근 %u초 평균 full >= %u%% (샘플 %u ms, %u칸)\n"
            "  안전장치: grace %u초, cooldown %u초, 시간당 최대 %u회\n"
            "  로그: %s (링버퍼 %u KiB)\n"
            "  자기 보호: mlock=%s, oom_score_adj=%d\n",
            c->dry_run ? "dry-run (종료하지 않음)" : "실제 종료",
            foreground ? ", foreground" : "", c->target_parent,
            (unsigned)c->trigger_stall_us, (unsigned)c->trigger_window_us,
            (unsigned)c->sustain_sec, (unsigned)c->full_pct, (unsigned)c->sample_interval_ms,
            (unsigned)tg_config_window_samples(c), (unsigned)c->startup_grace_sec,
            (unsigned)c->cooldown_sec, (unsigned)c->max_kills_per_hour,
            c->log_path[0] != '\0' ? c->log_path : "표준 오류", (unsigned)c->log_ring_kb,
            c->mlock ? "yes" : "no", (int)c->oom_score_adj);
}

static void report_cfg_error(const char *path, int rc, const struct tg_cfg_err *e)
{
    fprintf(stderr, "thrash-guard: 설정 오류:");
    if (path != NULL)
        fprintf(stderr, " %s", path);
    if (e->line > 0)
        fprintf(stderr, ":%d", e->line);
    if (e->key[0] != '\0')
        fprintf(stderr, ": %s", e->key);
    fprintf(stderr, ": %s", e->msg != NULL ? e->msg : "원인 불명");
    if (rc != -EINVAL && rc != -EPERM)
        fprintf(stderr, " (%s)", strerror(-rc));
    fputc('\n', stderr);
}

int main(int argc, char **argv)
{
    static const struct option longopts[] = {
        {"config", required_argument, NULL, 'c'},
        {"dry-run", no_argument, NULL, 'n'},
        {"no-dry-run", no_argument, NULL, 'k'},
        {"foreground", no_argument, NULL, 'f'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    const char *config_path = NULL;
    int dry_run_override = -1; /* -1이면 설정 파일 값을 따른다 */
    bool foreground = false;
    struct tg_config cfg;
    struct tg_cfg_err err;
    int opt;
    int rc;

    while ((opt = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
        switch (opt) {
        case 'c':
            config_path = optarg;
            break;
        case 'n':
            dry_run_override = 1;
            break;
        case 'k':
            dry_run_override = 0;
            break;
        case 'f':
            foreground = true;
            break;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return EXIT_USAGE;
        }
    }
    if (optind != argc) {
        fprintf(stderr, "thrash-guard: 알 수 없는 인자: %s\n", argv[optind]);
        usage(stderr);
        return EXIT_USAGE;
    }

    /* 우선순위: 기본값 -> 설정 파일 -> CLI (D14) */
    tg_config_defaults(&cfg);
    if (config_path != NULL) {
        rc = tg_config_load(config_path, &cfg, &err);
        if (rc != 0) {
            report_cfg_error(config_path, rc, &err);
            return EXIT_CONFIG;
        }
    }
    if (dry_run_override >= 0)
        cfg.dry_run = dry_run_override == 1;

    rc = tg_config_validate(&cfg, &err);
    if (rc != 0) {
        report_cfg_error(config_path, rc, &err);
        return EXIT_CONFIG;
    }

    print_config(&cfg, foreground);
    fputs("thrash-guard: 이벤트 루프는 아직 구현되지 않았다 (M3-2). 종료한다.\n", stderr);
    return 0;
}
