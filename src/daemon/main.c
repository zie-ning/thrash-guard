/*
 * main.c - thrash-guard 데몬 진입점
 *
 * 시작 순서: 신호 막기(스레드 생성 전) -> 설정 -> 로그 시작 -> 자기 보호 -> 후보 스캔 시작
 *           -> 엔진 -> PSI 트리거 등록 -> 이벤트 루프.
 * 후보 탐색과 트리거 등록에 실패하면 시작을 거부한다(안전장치가 켜진 척하지 않기 위해).
 *
 * 이 파일은 초기화 단계라 stdio를 쓴다. 핵심 경로(이벤트 루프 이후)에서는 쓰지 않는다.
 */
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "engine.h"
#include "loop.h"
#include "scanner.h"
#include "selfprot.h"

enum { EXIT_USAGE = 2, EXIT_CONFIG = 3, EXIT_RUNTIME = 4 };

#define SCAN_INTERVAL_MS 1000 /* 후보 cgroup 목록을 다시 읽는 주기 (임시) */
#define STACK_PREFAULT_BYTES (64 * 1024)

/* 구조체가 커서(수십 KiB) 정적으로 둔다. mlockall 대상에도 포함된다. */
static struct tg_log g_log;
static struct tg_scanner g_scanner;
static struct tg_engine g_engine;

static void usage(FILE *out)
{
    fputs("사용법: thrash-guard [옵션]\n"
          "  --config <경로>   설정 파일 (key=value). 없으면 기본값만 쓴다\n"
          "  --dry-run         판단만 기록하고 종료는 하지 않는다 (기본)\n"
          "  --no-dry-run      실제로 cgroup을 종료한다\n"
          "  --foreground      (호환용) 현재는 항상 전면에서 실행한다\n"
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

static void log_msg(enum tg_log_level lv, const char *msg)
{
    struct tg_buf b;

    if (tg_log_begin(&g_log, lv, &b)) {
        tg_buf_str(&b, msg);
        tg_log_end(&g_log, &b);
    }
}

/* 데몬 본체. 시작 단계의 오류는 stderr로 알리고 시작을 거부한다. */
static int run_daemon(const struct tg_config *cfg)
{
    struct tg_selfprot_opts sp;
    struct tg_selfprot_result spr;
    struct tg_loop_fds fds;
    int sig_fd;
    int trigger_fd;
    int rc;

    /* 어떤 스레드보다 먼저 신호를 막는다. 새 스레드가 이 설정을 물려받는다. */
    sig_fd = tg_signals_block();
    if (sig_fd < 0) {
        fprintf(stderr, "thrash-guard: 신호 설정 실패: %s\n", strerror(-sig_fd));
        return EXIT_RUNTIME;
    }

    rc = tg_log_start(&g_log, cfg);
    if (rc != 0) {
        fprintf(stderr, "thrash-guard: 로그 시작 실패: %s\n", strerror(-rc));
        return EXIT_RUNTIME;
    }

    memset(&sp, 0, sizeof(sp));
    sp.mlock = cfg->mlock;
    sp.stack_prefault = STACK_PREFAULT_BYTES;
    sp.oom_score_adj = cfg->oom_score_adj;
    tg_selfprot_apply(&sp, &spr);
    if (spr.mlock_err > 0)
        log_msg(TG_LOG_WARN, "self protection: mlockall failed");
    if (spr.oom_adj_err > 0)
        log_msg(TG_LOG_WARN, "self protection: oom_score_adj failed");

    rc = tg_scanner_start(&g_scanner, cfg->target_parent, SCAN_INTERVAL_MS);
    if (rc != 0) {
        fprintf(stderr, "thrash-guard: 후보 탐색을 시작할 수 없다: %s: %s\n", cfg->target_parent,
                strerror(-rc));
        goto fail_log;
    }

    rc = tg_engine_init(&g_engine, cfg, &g_log, &g_scanner, "/proc/pressure/memory",
                        "/proc/meminfo");
    if (rc != 0) {
        fprintf(stderr, "thrash-guard: 엔진 초기화 실패: %s\n", strerror(-rc));
        goto fail_scanner;
    }

    trigger_fd = tg_psi_trigger_open(cfg->trigger_stall_us, cfg->trigger_window_us);
    if (trigger_fd < 0) {
        fprintf(stderr,
                "thrash-guard: PSI 트리거를 등록할 수 없다: %s\n"
                "  root 권한과 PSI 지원(커널 4.20 이상, psi=0이 아님)이 필요하다.\n",
                strerror(-trigger_fd));
        goto fail_engine;
    }

    log_msg(TG_LOG_INFO, cfg->dry_run ? "start mode=dry-run" : "start mode=kill");

    fds.sig_fd = sig_fd;
    fds.trigger_fd = trigger_fd;
    fds.trigger_events = POLLPRI;
    rc = tg_loop_run(&g_engine, &fds);
    if (rc != 0)
        log_msg(TG_LOG_ERR, "loop stopped by error");
    log_msg(TG_LOG_INFO, "stop");
    tg_engine_log_summary(&g_engine);

    close(trigger_fd);
    tg_engine_close(&g_engine);
    tg_scanner_stop(&g_scanner);
    tg_log_stop(&g_log);
    return rc == 0 ? 0 : EXIT_RUNTIME;

fail_engine:
    tg_engine_close(&g_engine);
fail_scanner:
    tg_scanner_stop(&g_scanner);
fail_log:
    tg_log_stop(&g_log);
    return EXIT_RUNTIME;
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

    /* 우선순위: 기본값 -> 설정 파일 -> CLI  */
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
    return run_daemon(&cfg);
}
