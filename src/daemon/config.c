/*
 * config.c - 데몬 설정 구현
 *
 * 초기화 단계 전용이다. 힙 할당과 stdio를 쓰지 않는다.
 */
#include "config.h"

#include <errno.h>
#include <string.h>

#include "procfs.h"

#define CFG_FILE_CAP 4096 /* 설정 파일 최대 크기(NUL 포함). 넘으면 -ENOBUFS */
#define CFG_KEY_CAP sizeof(((struct tg_cfg_err *)0)->key)

/* ---------- 기본값 (D13, 모두 측정 전 임시값) ---------- */

void tg_config_defaults(struct tg_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->target_parent, "/sys/fs/cgroup/workload.slice");
    cfg->trigger_stall_us = 150000;
    cfg->trigger_window_us = 1000000;
    cfg->full_pct = 10;
    cfg->sustain_sec = 10;
    cfg->sample_interval_ms = 250;
    cfg->startup_grace_sec = 30;
    cfg->cooldown_sec = 60;
    cfg->max_kills_per_hour = 3;
    cfg->dry_run = true;
    cfg->log_path[0] = '\0';
    cfg->log_ring_kb = 16;
    cfg->mlock = true;
    cfg->oom_score_adj = -1000;
}

/* ---------- 키 표 ---------- */

enum field_kind { K_U32, K_I32, K_BOOL, K_STR };

struct field {
    const char *key;
    enum field_kind kind;
    size_t off;
    int64_t min; /* 숫자 범위 */
    int64_t max; /* 숫자 범위. K_STR는 최대 길이(NUL 제외) */
};

#define F_U32(name, lo, hi) {#name, K_U32, offsetof(struct tg_config, name), (lo), (hi)}
#define F_I32(name, lo, hi) {#name, K_I32, offsetof(struct tg_config, name), (lo), (hi)}
#define F_BOOL(name) {#name, K_BOOL, offsetof(struct tg_config, name), 0, 1}
#define F_STR(name, maxlen) {#name, K_STR, offsetof(struct tg_config, name), 0, (maxlen)}

/*
 * 범위는 PSI 트리거 실측에 맞췄다(WSL 6.18, root): 윈도 상한 10초, stall이 윈도보다 크면 거부.
 * 하한은 400 ms도 통과했지만 보수적으로 500 ms로 둔다. VM 6.8 커널에서의 경계는 미확인.
 */
static const struct field fields[] = {
    F_STR(target_parent, TG_CFG_PATH_CAP - 1),
    F_U32(trigger_stall_us, 1, 10000000),
    F_U32(trigger_window_us, 500000, 10000000),
    F_U32(full_pct, 1, 100),
    F_U32(sustain_sec, 1, 300),
    F_U32(sample_interval_ms, 50, 1000),
    F_U32(startup_grace_sec, 0, 3600),
    F_U32(cooldown_sec, 0, 86400),
    F_U32(max_kills_per_hour, 1, 100),
    F_STR(log_path, TG_CFG_PATH_CAP - 1),
    F_U32(log_ring_kb, 4, 1024),
    F_BOOL(mlock),
    F_I32(oom_score_adj, -1000, 1000),
};

/* ---------- 오류 기록 ---------- */

static int fail(struct tg_cfg_err *err, int rc, const char *msg)
{
    if (err != NULL)
        err->msg = msg;
    return rc;
}

static void set_key(struct tg_cfg_err *err, const char *key, size_t len)
{
    if (err == NULL)
        return;
    if (len >= CFG_KEY_CAP)
        len = CFG_KEY_CAP - 1;
    memcpy(err->key, key, len);
    err->key[len] = '\0';
}

/* ---------- 토큰 처리 ---------- */

static int is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static const char *trim_left(const char *p, const char *end)
{
    while (p < end && is_blank(*p))
        p++;
    return p;
}

static const char *trim_right(const char *start, const char *end)
{
    while (end > start && is_blank(end[-1]))
        end--;
    return end;
}

/* 10진 정수를 읽는다. allow_neg이면 앞의 '-'를 허용한다. 숫자 외 문자가 섞이면 실패. */
static int parse_int(const char *p, const char *end, bool allow_neg, int64_t *out)
{
    bool neg = false;
    int64_t v = 0;

    if (allow_neg && p < end && *p == '-') {
        neg = true;
        p++;
    }
    if (p >= end)
        return -EINVAL;
    for (; p < end; p++) {
        if (*p < '0' || *p > '9')
            return -EINVAL;
        if (v > (INT64_MAX - (*p - '0')) / 10)
            return -ERANGE;
        v = v * 10 + (*p - '0');
    }
    *out = neg ? -v : v;
    return 0;
}

static bool token_eq(const char *p, const char *end, const char *lit)
{
    size_t n = strlen(lit);
    return (size_t)(end - p) == n && memcmp(p, lit, n) == 0;
}

/* ---------- 필드 쓰기 ---------- */

static int set_field(struct tg_config *cfg, const struct field *f, const char *v,
                     const char *vend, struct tg_cfg_err *err)
{
    char *dst = (char *)cfg + f->off;

    switch (f->kind) {
    case K_U32:
    case K_I32: {
        int64_t n;
        int rc = parse_int(v, vend, f->kind == K_I32, &n);
        if (rc != 0)
            return fail(err, -EINVAL, "값이 정수가 아니다");
        if (n < f->min || n > f->max)
            return fail(err, -EINVAL, "값이 허용 범위를 벗어났다");
        if (f->kind == K_U32) {
            uint32_t u = (uint32_t)n;
            memcpy(dst, &u, sizeof(u));
        } else {
            int32_t s = (int32_t)n;
            memcpy(dst, &s, sizeof(s));
        }
        return 0;
    }
    case K_BOOL: {
        bool b;
        if (token_eq(v, vend, "yes"))
            b = true;
        else if (token_eq(v, vend, "no"))
            b = false;
        else
            return fail(err, -EINVAL, "값은 yes 또는 no여야 한다");
        memcpy(dst, &b, sizeof(b));
        return 0;
    }
    case K_STR: {
        size_t n = (size_t)(vend - v);
        if ((int64_t)n > f->max)
            return fail(err, -EINVAL, "문자열이 너무 길다");
        memcpy(dst, v, n);
        dst[n] = '\0';
        return 0;
    }
    }
    return fail(err, -EINVAL, "내부 오류: 알 수 없는 필드 종류");
}

/* ---------- 한 줄 처리 ---------- */

static int parse_line(const char *p, const char *le, struct tg_config *cfg,
                      struct tg_cfg_err *err)
{
    const char *eq;
    const char *kend;
    const char *v;
    const char *vend;
    size_t klen;

    p = trim_left(p, le);
    if (p == le || *p == '#')
        return 0;

    eq = memchr(p, '=', (size_t)(le - p));
    if (eq == NULL)
        return fail(err, -EINVAL, "'='가 없다");

    kend = trim_right(p, eq);
    klen = (size_t)(kend - p);
    if (klen == 0)
        return fail(err, -EINVAL, "키가 비어 있다");
    set_key(err, p, klen);
    if (klen >= CFG_KEY_CAP)
        return fail(err, -EINVAL, "키가 너무 길다");

    v = trim_left(eq + 1, le);
    vend = trim_right(v, le);

    /* dry_run은 설정 파일에서 켤 수만 있다. 끄는 것은 CLI --no-dry-run만 가능하다(D14). */
    if (klen == strlen("dry_run") && memcmp(p, "dry_run", klen) == 0) {
        if (token_eq(v, vend, "yes")) {
            cfg->dry_run = true;
            return 0;
        }
        if (token_eq(v, vend, "no"))
            return fail(err, -EPERM,
                        "설정 파일에서는 dry_run을 끌 수 없다 (--no-dry-run 사용)");
        return fail(err, -EINVAL, "값은 yes 또는 no여야 한다");
    }

    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (klen == strlen(fields[i].key) && memcmp(p, fields[i].key, klen) == 0)
            return set_field(cfg, &fields[i], v, vend, err);
    }
    return fail(err, -EINVAL, "알 수 없는 키");
}

/* ---------- 공개 함수 ---------- */

int tg_config_parse(const char *buf, size_t len, struct tg_config *cfg, struct tg_cfg_err *err)
{
    const char *p = buf;
    const char *end = buf + len;
    int line = 0;

    if (err != NULL) {
        err->line = 0;
        err->key[0] = '\0';
        err->msg = NULL;
    }

    while (p < end) {
        const char *le = memchr(p, '\n', (size_t)(end - p));
        int rc;

        if (le == NULL)
            le = end;
        line++;
        rc = parse_line(p, le, cfg, err);
        if (rc != 0) {
            if (err != NULL)
                err->line = line;
            return rc;
        }
        p = le < end ? le + 1 : end;
    }
    return 0;
}

int tg_config_load(const char *path, struct tg_config *cfg, struct tg_cfg_err *err)
{
    char buf[CFG_FILE_CAP];
    size_t len = 0;
    int rc = tg_read_path(path, buf, sizeof(buf), &len);

    if (err != NULL) {
        err->line = 0;
        err->key[0] = '\0';
        err->msg = NULL;
    }
    if (rc != 0)
        return fail(err, rc, rc == -ENOBUFS ? "설정 파일이 너무 크다" : "설정 파일을 읽지 못했다");
    return tg_config_parse(buf, len, cfg, err);
}

uint32_t tg_config_window_samples(const struct tg_config *cfg)
{
    return cfg->sustain_sec * 1000u / cfg->sample_interval_ms;
}

/* 절대 경로이고, 빈 요소(//, 끝의 /)와 "."·".." 요소가 없어야 한다. */
static bool path_is_clean_absolute(const char *s)
{
    const char *p = s;

    if (*p != '/')
        return false;
    while (*p == '/') {
        const char *comp = p + 1;
        const char *q = comp;

        while (*q != '\0' && *q != '/')
            q++;
        if (q == comp)
            return false;
        if ((q - comp == 1 && comp[0] == '.') || (q - comp == 2 && comp[0] == '.' && comp[1] == '.'))
            return false;
        p = q;
    }
    return *p == '\0';
}

int tg_config_validate(const struct tg_config *cfg, struct tg_cfg_err *err)
{
    if (err != NULL) {
        err->line = 0;
        err->key[0] = '\0';
        err->msg = NULL;
    }

    if (!path_is_clean_absolute(cfg->target_parent)) {
        set_key(err, "target_parent", strlen("target_parent"));
        return fail(err, -EINVAL, "절대 경로여야 하고 빈 요소, '.', '..'를 포함할 수 없다");
    }
    if (cfg->log_path[0] != '\0' && !path_is_clean_absolute(cfg->log_path)) {
        set_key(err, "log_path", strlen("log_path"));
        return fail(err, -EINVAL, "비어 있거나 깨끗한 절대 경로여야 한다");
    }
    if (cfg->trigger_stall_us > cfg->trigger_window_us) {
        set_key(err, "trigger_stall_us", strlen("trigger_stall_us"));
        return fail(err, -EINVAL, "trigger_window_us보다 클 수 없다");
    }
    if ((cfg->sustain_sec * 1000u) % cfg->sample_interval_ms != 0) {
        set_key(err, "sample_interval_ms", strlen("sample_interval_ms"));
        return fail(err, -EINVAL, "sustain_sec 안에 정수 번 들어가야 한다");
    }
    if (tg_config_window_samples(cfg) > TG_MAX_WINDOW_SAMPLES) {
        set_key(err, "sample_interval_ms", strlen("sample_interval_ms"));
        return fail(err, -EINVAL, "sustain_sec / sample_interval_ms가 최대 칸 수를 넘는다");
    }
    return 0;
}
