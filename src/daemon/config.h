/*
 * config.h - 데몬 설정 (key=value 파일 + 기본값)
 *
 * 초기화 단계에서만 쓴다. 힙 할당을 하지 않고, 문자열은 고정 크기 배열에 담는다.
 *
 * 반환값 규약: 성공 0, 실패 시 음수 errno.
 *   -EINVAL : 문법 오류, 모르는 키, 범위를 벗어난 값, 검증 실패
 *   -EPERM  : 설정 파일에서 허용하지 않는 값 (dry_run=no)
 *   -ENOBUFS: 설정 파일이 읽기 버퍼보다 큼
 *   그 밖의 값 : 파일 열기·읽기 실패 errno
 * 실패 원인의 줄 번호·키·사유는 struct tg_cfg_err에 남는다.
 *
 * 파일 형식:
 *   - 한 줄에 key=value. 키와 값 앞뒤 공백은 무시한다.
 *   - '#'으로 시작하는 줄과 빈 줄은 무시한다. 줄 끝 주석은 지원하지 않는다.
 *   - 같은 키가 두 번 나오면 나중 값이 이긴다.
 *   - 모르는 키는 오류다. 오타가 조용히 무시되어 안전장치가 꺼지는 일을 막는다.
 *
 * 기본값은 모두 측정 전 임시값이다.
 */
#ifndef TG_CONFIG_H
#define TG_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TG_CFG_PATH_CAP 256        /* 경로 문자열 용량(NUL 포함) */
#define TG_MAX_WINDOW_SAMPLES 256 /* 지속 판단 고정 배열의 최대 칸 수. 후보 32개 x 2배열이 mlock되므로 작게 둔다 */

struct tg_config {
    /* 감시·종료 대상 구역. 이 cgroup의 memory.pressure를 감시하고 그 하위만 종료한다 */
    char target_parent[TG_CFG_PATH_CAP];

    /* 커널 PSI 트리거: window_us 동안 stall_us 이상 멈추면 깨운다 */
    uint32_t trigger_stall_us;
    uint32_t trigger_window_us;

    /* 지속 판단: 최근 sustain_sec 동안의 평균 full 비율이 full_pct 이상이면 스래싱 */
    uint32_t full_pct;           /* X */
    uint32_t sustain_sec;        /* N */
    uint32_t sample_interval_ms; /* 샘플 주기. sustain_sec 안에 정수 번 들어가야 한다 */
    uint32_t mem_avail_pct;      /* Y: MemAvailable이 전체의 이 비율 이하일 때만 종료한다 */
    uint32_t min_size_pct;       /* 후보 자격: 윈도 안 최대 사용량이 전체의 이 비율 이상 */
    uint32_t min_growth_pct;     /* 증가 인정: 윈도 안 증가량이 전체의 이 비율 이상 */

    /* 안전장치 */
    uint32_t startup_grace_sec;
    uint32_t cooldown_sec;
    uint32_t max_kills_per_hour;

    /* 종료 여부. 설정 파일에서는 끌 수 없고 CLI --no-dry-run으로만 끈다 */
    bool dry_run;

    /* 로그. log_path가 비어 있으면 표준 오류로 쓴다 */
    char log_path[TG_CFG_PATH_CAP];
    uint32_t log_ring_kb;

    /* 자기 보호 */
    bool mlock;
    int32_t oom_score_adj;
};

/* 실패 원인. 호출자가 메시지를 만들 때 쓴다. */
struct tg_cfg_err {
    int line;        /* 1부터 시작하는 줄 번호. 줄과 무관한 오류(검증)는 0 */
    char key[48];    /* 문제가 된 키. 없으면 빈 문자열 */
    const char *msg; /* 정적 문자열 */
};

void tg_config_defaults(struct tg_config *cfg);

/*
 * buf(NUL 종료, 길이 len)의 내용을 읽어 cfg에 덮어쓴다. 검증은 하지 않는다.
 * cfg는 미리 tg_config_defaults로 채워 두어야 한다.
 */
int tg_config_parse(const char *buf, size_t len, struct tg_config *cfg, struct tg_cfg_err *err);

/* 파일을 읽어 tg_config_parse를 호출한다. */
int tg_config_load(const char *path, struct tg_config *cfg, struct tg_cfg_err *err);

/* 키 사이의 관계까지 포함해 검증한다. 시작 전에 반드시 호출한다. */
int tg_config_validate(const struct tg_config *cfg, struct tg_cfg_err *err);

/* 지속 판단 배열의 칸 수 = sustain_sec * 1000 / sample_interval_ms */
uint32_t tg_config_window_samples(const struct tg_config *cfg);

#endif /* TG_CONFIG_H */
