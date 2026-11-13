/*
 * replay_judge - 기록한 PSI(psirec의 psi.tsv)를 실제 판단 함수에 넣어 종료 결정 시각을 계산한다.
 *
 * VM 없이 판단 규칙을 비교하려는 도구다. 커널 트리거는 "1초 안에 memory full이 150 ms 이상 늘었다"로
 * 흉내 낸다. 기록은 1초 간격이라 판단 함수의 샘플 간격(설정 250 ms)과 다르지만, 판단 함수는 실제 경과
 * 시간으로 평균을 내므로 쓸 수 있다(시간 기준 윈도). 후보는 650 MiB짜리 하나로 고정한다(결정 시각과
 * 경로만 본다).
 *
 * 빌드: cc -std=c11 -D_GNU_SOURCE -Isrc/common -Isrc/daemon -o replay_judge lab/analyze/replay_judge.c \
 *           src/daemon/judge.c src/daemon/policy.c src/daemon/config.c src/daemon/match.c src/common/procfs.c \
 *           src/common/fmt.c src/common/cgscan.c
 * 사용법: replay_judge <psi.tsv> <full_pct> <io_full_pct> [io_mem_avail_pct]
 * 출력: 가용 메모리가 30 MiB 미만이 된 시각을 0으로 한 첫 종료 결정 시각(초)과 경로, 건너뜀 횟수.
 */
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "judge.h"
#include "policy.h"

#define MEM_TOTAL_MIB 961ull
#define MAX_ROWS 20000

struct row {
    uint64_t ms;
    uint64_t mem_full_us;
    uint64_t io_full_us;
    uint64_t avail_kb;
};

static struct row rows[MAX_ROWS];

int main(int argc, char **argv)
{
    FILE *f;
    int n = 0;
    struct tg_config cfg;
    struct tg_judge j;
    struct tg_policy pol;
    struct tg_cand_sample cand;
    int64_t t_low_ms = -1;
    int64_t first_kill_ms = -1;
    int path = 0;
    int skips = 0;
    int kills = 0;

    if (argc < 4) {
        fprintf(stderr, "사용법: %s <psi.tsv> <full_pct> <io_full_pct> [io_mem_avail_pct]\n", argv[0]);
        return 2;
    }
    f = fopen(argv[1], "r");
    if (f == NULL) {
        perror(argv[1]);
        return 1;
    }
    while (n < MAX_ROWS) {
        char line[512];
        unsigned long long v[10] = {0};
        int got;

        if (fgets(line, sizeof(line), f) == NULL)
            break;
        got = sscanf(line, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3],
                     &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
        if (got < 8)
            continue;
        rows[n].ms = v[0];
        rows[n].mem_full_us = v[2];
        rows[n].io_full_us = v[4];
        rows[n].avail_kb = v[7];
        n++;
    }
    fclose(f);

    tg_config_defaults(&cfg);
    cfg.full_pct = (uint32_t)atoi(argv[2]);
    cfg.io_full_pct = (uint32_t)atoi(argv[3]);
    if (argc > 4)
        cfg.io_mem_avail_pct = (uint32_t)atoi(argv[4]);
    tg_judge_init(&j, &cfg);
    tg_policy_init(&pol, cfg.cooldown_sec, cfg.max_kills_per_hour);

    cand.name = "hog";
    cand.mem_bytes = 650ull << 20;
    cand.first_seen_ns = 0; /* 데몬 시작 전부터 있던 항목: 유예 면제 */
    cand.is_protected = false;

    for (int i = 0; i < n; i++) {
        struct tg_sample s = {0};
        struct tg_decision d;

        if (t_low_ms < 0 && i > 60 && rows[i].avail_kb < 30 * 1024)
            t_low_ms = (int64_t)rows[i].ms;

        /* 흉내 낸 커널 트리거: 직전 1초 동안 memory full이 150 ms 이상 늘었다 */
        if (!j.watching && i > 0 && rows[i].mem_full_us - rows[i - 1].mem_full_us >= 150000)
            tg_judge_trigger(&j);

        s.now_ns = rows[i].ms * 1000000ull;
        s.sys_full_total_us = rows[i].mem_full_us;
        s.io_valid = true;
        s.io_full_total_us = rows[i].io_full_us;
        s.mem_available_bytes = rows[i].avail_kb * 1024ull;
        s.mem_total_bytes = MEM_TOTAL_MIB << 20;
        s.cand = &cand;
        s.ncand = 1;
        d = tg_judge_tick(&j, &s, &pol);
        if (d.kind == TG_J_SKIP)
            skips++;
        if (d.kind == TG_J_KILL) {
            kills++;
            tg_policy_record_kill(&pol, s.now_ns);
            if (first_kill_ms < 0) {
                first_kill_ms = (int64_t)rows[i].ms;
                path = d.path;
            }
        }
    }

    printf("가용<30MiB 시작 기준 첫 종료 결정: ");
    if (first_kill_ms < 0)
        printf("없음");
    else if (t_low_ms < 0)
        printf("(가용<30MiB 없음) 경로 %s", tg_judge_path_name((enum tg_judge_path)path));
    else
        printf("%+.0f초, 경로 %s", (double)(first_kill_ms - t_low_ms) / 1000.0,
               tg_judge_path_name((enum tg_judge_path)path));
    printf(" | 종료 결정 %d회, 건너뜀 %d회\n", kills, skips);
    return 0;
}
