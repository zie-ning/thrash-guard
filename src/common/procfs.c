/*
 * procfs.c - /proc, cgroup 메모리 통계 파서 구현
 *
 * 핵심 경로: 할당 금지. 이 파일의 함수는 힙과 stdio를 쓰지 않는다.
 */
#include "procfs.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* ---------- 파일 읽기 ---------- */

int tg_read_fd(int fd, char *buf, size_t cap, size_t *len)
{
    size_t off = 0;

    if (buf == NULL || cap < 2)
        return -EINVAL;

    for (;;) {
        ssize_t n = pread(fd, buf + off, cap - 1 - off, (off_t)off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (n == 0)
            break;
        off += (size_t)n;
        if (off == cap - 1) {
            /* 버퍼가 가득 찼다. 남은 내용이 있으면 잘린 것이다. */
            char probe;
            ssize_t m;
            do {
                m = pread(fd, &probe, 1, (off_t)off);
            } while (m < 0 && errno == EINTR);
            if (m < 0)
                return -errno;
            if (m > 0)
                return -ENOBUFS;
            break;
        }
    }

    buf[off] = '\0';
    if (len != NULL)
        *len = off;
    return 0;
}

int tg_read_path(const char *path, char *buf, size_t cap, size_t *len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    int rc;

    if (fd < 0)
        return -errno;
    rc = tg_read_fd(fd, buf, cap, len);
    close(fd);
    return rc;
}

/* ---------- 공용 토큰 처리 ---------- */

static int is_blank(char c)
{
    return c == ' ' || c == '\t';
}

static const char *skip_blank(const char *p, const char *end)
{
    while (p < end && is_blank(*p))
        p++;
    return p;
}

static const char *line_end(const char *p, const char *end)
{
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    return nl != NULL ? nl : end;
}

/* 10진수를 하나 이상 읽는다. 숫자가 없으면 -EINVAL, 범위를 넘으면 -EOVERFLOW. */
static int parse_u64(const char **pp, const char *end, uint64_t *out)
{
    const char *p = *pp;
    uint64_t v = 0;

    if (p >= end || *p < '0' || *p > '9')
        return -EINVAL;
    while (p < end && *p >= '0' && *p <= '9') {
        uint64_t d = (uint64_t)(*p - '0');
        if (v > (UINT64_MAX - d) / 10)
            return -EOVERFLOW;
        v = v * 10 + d;
        p++;
    }
    *pp = p;
    *out = v;
    return 0;
}

/* ---------- PSI ---------- */

/* "12.34" 형식을 1234로 읽는다. 소수점 아래는 0~2자리를 허용한다. */
static int parse_centi(const char **pp, const char *end, uint32_t *out)
{
    const char *p = *pp;
    uint64_t ip;
    uint64_t frac = 0;
    int rc = parse_u64(&p, end, &ip);

    if (rc != 0)
        return rc;
    if (p < end && *p == '.') {
        int digits = 0;
        p++;
        while (p < end && *p >= '0' && *p <= '9') {
            if (++digits > 2)
                return -EINVAL;
            frac = frac * 10 + (uint64_t)(*p - '0');
            p++;
        }
        if (digits == 1)
            frac *= 10;
    }
    if (ip > (UINT32_MAX - frac) / 100)
        return -EOVERFLOW;
    *out = (uint32_t)(ip * 100 + frac);
    *pp = p;
    return 0;
}

static int starts_with(const char *p, const char *end, const char *lit)
{
    size_t n = strlen(lit);
    return (size_t)(end - p) >= n && memcmp(p, lit, n) == 0;
}

enum { PSI_AVG10 = 1, PSI_AVG60 = 2, PSI_AVG300 = 4, PSI_TOTAL = 8, PSI_ALL = 15 };

static int parse_psi_line(const char *p, const char *end, struct tg_psi_line *out)
{
    unsigned seen = 0;

    while (p < end) {
        int rc;
        p = skip_blank(p, end);
        if (p >= end)
            break;
        if (starts_with(p, end, "avg10=")) {
            p += 6;
            rc = parse_centi(&p, end, &out->avg10);
            seen |= PSI_AVG10;
        } else if (starts_with(p, end, "avg60=")) {
            p += 6;
            rc = parse_centi(&p, end, &out->avg60);
            seen |= PSI_AVG60;
        } else if (starts_with(p, end, "avg300=")) {
            p += 7;
            rc = parse_centi(&p, end, &out->avg300);
            seen |= PSI_AVG300;
        } else if (starts_with(p, end, "total=")) {
            p += 6;
            rc = parse_u64(&p, end, &out->total_us);
            seen |= PSI_TOTAL;
        } else {
            return -EINVAL;
        }
        if (rc != 0)
            return rc;
        if (p < end && !is_blank(*p))
            return -EINVAL;
    }
    return seen == PSI_ALL ? 0 : -EINVAL;
}

int tg_psi_parse(const char *buf, size_t len, struct tg_psi *out)
{
    const char *p = buf;
    const char *end = buf + len;
    bool has_some = false;

    memset(out, 0, sizeof(*out));
    while (p < end) {
        const char *le = line_end(p, end);
        int rc;

        if (le == p) {
            /* 빈 줄은 건너뛴다. */
        } else if (starts_with(p, le, "some ")) {
            rc = parse_psi_line(p + 5, le, &out->some);
            if (rc != 0)
                return rc;
            has_some = true;
        } else if (starts_with(p, le, "full ")) {
            rc = parse_psi_line(p + 5, le, &out->full);
            if (rc != 0)
                return rc;
            out->has_full = true;
        } else {
            return -EINVAL;
        }
        p = le < end ? le + 1 : end;
    }
    return has_some ? 0 : -EINVAL;
}

/* ---------- 키 값 형식 ---------- */

int tg_kv_parse(const char *buf, size_t len, const struct tg_kv_field *fields,
                size_t nfields, void *out, uint64_t *found)
{
    const char *p = buf;
    const char *end = buf + len;
    uint64_t mask = 0;

    if (nfields > 64)
        return -EINVAL;

    while (p < end) {
        const char *le = line_end(p, end);
        const char *k = p;
        const char *ke = p;
        size_t klen;
        size_t i;

        while (ke < le && !is_blank(*ke))
            ke++;
        klen = (size_t)(ke - k);
        if (klen > 0 && k[klen - 1] == ':')
            klen--;

        for (i = 0; i < nfields; i++) {
            if (strlen(fields[i].key) == klen && memcmp(fields[i].key, k, klen) == 0)
                break;
        }
        if (i < nfields && klen > 0) {
            const char *v = skip_blank(ke, le);
            uint64_t val;
            int rc = parse_u64(&v, le, &val);
            if (rc != 0)
                return rc;
            if (v < le && !is_blank(*v))
                return -EINVAL;
            memcpy((char *)out + fields[i].offset, &val, sizeof(val));
            mask |= (uint64_t)1 << i;
        }
        p = le < end ? le + 1 : end;
    }

    if (found != NULL)
        *found = mask;
    return 0;
}

#define FIELD(type, key, member) { key, offsetof(struct type, member) }
#define NFIELDS(arr) (sizeof(arr) / sizeof((arr)[0]))

/* ---------- /proc/vmstat ---------- */

const struct tg_kv_field tg_vmstat_fields[] = {
    FIELD(tg_vmstat, "pgscan_kswapd", pgscan_kswapd),
    FIELD(tg_vmstat, "pgscan_direct", pgscan_direct),
    FIELD(tg_vmstat, "pgsteal_kswapd", pgsteal_kswapd),
    FIELD(tg_vmstat, "pgsteal_direct", pgsteal_direct),
    FIELD(tg_vmstat, "pgscan_anon", pgscan_anon),
    FIELD(tg_vmstat, "pgscan_file", pgscan_file),
    FIELD(tg_vmstat, "pgsteal_anon", pgsteal_anon),
    FIELD(tg_vmstat, "pgsteal_file", pgsteal_file),
    FIELD(tg_vmstat, "workingset_refault_anon", workingset_refault_anon),
    FIELD(tg_vmstat, "workingset_refault_file", workingset_refault_file),
    FIELD(tg_vmstat, "pswpin", pswpin),
    FIELD(tg_vmstat, "pswpout", pswpout),
    FIELD(tg_vmstat, "pgmajfault", pgmajfault),
    FIELD(tg_vmstat, "allocstall_normal", allocstall_normal),
    FIELD(tg_vmstat, "allocstall_movable", allocstall_movable),
    FIELD(tg_vmstat, "oom_kill", oom_kill),
};
const size_t tg_vmstat_nfields = NFIELDS(tg_vmstat_fields);

int tg_vmstat_parse(const char *buf, size_t len, struct tg_vmstat *out, uint64_t *found)
{
    memset(out, 0, sizeof(*out));
    return tg_kv_parse(buf, len, tg_vmstat_fields, tg_vmstat_nfields, out, found);
}

/* ---------- /proc/meminfo ---------- */

const struct tg_kv_field tg_meminfo_fields[] = {
    FIELD(tg_meminfo, "MemTotal", mem_total_kb),
    FIELD(tg_meminfo, "MemFree", mem_free_kb),
    FIELD(tg_meminfo, "MemAvailable", mem_available_kb),
    FIELD(tg_meminfo, "Buffers", buffers_kb),
    FIELD(tg_meminfo, "Cached", cached_kb),
    FIELD(tg_meminfo, "SwapTotal", swap_total_kb),
    FIELD(tg_meminfo, "SwapFree", swap_free_kb),
    FIELD(tg_meminfo, "Active(anon)", active_anon_kb),
    FIELD(tg_meminfo, "Inactive(anon)", inactive_anon_kb),
    FIELD(tg_meminfo, "Active(file)", active_file_kb),
    FIELD(tg_meminfo, "Inactive(file)", inactive_file_kb),
    FIELD(tg_meminfo, "Dirty", dirty_kb),
    FIELD(tg_meminfo, "Writeback", writeback_kb),
};
const size_t tg_meminfo_nfields = NFIELDS(tg_meminfo_fields);

int tg_meminfo_parse(const char *buf, size_t len, struct tg_meminfo *out, uint64_t *found)
{
    memset(out, 0, sizeof(*out));
    return tg_kv_parse(buf, len, tg_meminfo_fields, tg_meminfo_nfields, out, found);
}

/* ---------- cgroup memory.stat ---------- */

const struct tg_kv_field tg_cg_memstat_fields[] = {
    FIELD(tg_cg_memstat, "anon", anon),
    FIELD(tg_cg_memstat, "file", file),
    FIELD(tg_cg_memstat, "shmem", shmem),
    FIELD(tg_cg_memstat, "workingset_refault_anon", workingset_refault_anon),
    FIELD(tg_cg_memstat, "workingset_refault_file", workingset_refault_file),
    FIELD(tg_cg_memstat, "pgscan", pgscan),
    FIELD(tg_cg_memstat, "pgsteal", pgsteal),
    FIELD(tg_cg_memstat, "pgmajfault", pgmajfault),
};
const size_t tg_cg_memstat_nfields = NFIELDS(tg_cg_memstat_fields);

int tg_cg_memstat_parse(const char *buf, size_t len, struct tg_cg_memstat *out,
                        uint64_t *found)
{
    memset(out, 0, sizeof(*out));
    return tg_kv_parse(buf, len, tg_cg_memstat_fields, tg_cg_memstat_nfields, out, found);
}

/* ---------- cgroup memory.events ---------- */

const struct tg_kv_field tg_cg_events_fields[] = {
    FIELD(tg_cg_events, "high", high),
    FIELD(tg_cg_events, "max", max),
    FIELD(tg_cg_events, "oom", oom),
    FIELD(tg_cg_events, "oom_kill", oom_kill),
};
const size_t tg_cg_events_nfields = NFIELDS(tg_cg_events_fields);

int tg_cg_events_parse(const char *buf, size_t len, struct tg_cg_events *out, uint64_t *found)
{
    memset(out, 0, sizeof(*out));
    return tg_kv_parse(buf, len, tg_cg_events_fields, tg_cg_events_nfields, out, found);
}

/* ---------- cgroup.stat ---------- */

const struct tg_kv_field tg_cg_cgstat_fields[] = {
    FIELD(tg_cg_cgstat, "nr_descendants", nr_descendants),
    FIELD(tg_cg_cgstat, "nr_dying_descendants", nr_dying_descendants),
};
const size_t tg_cg_cgstat_nfields = NFIELDS(tg_cg_cgstat_fields);

int tg_cg_cgstat_parse(const char *buf, size_t len, struct tg_cg_cgstat *out, uint64_t *found)
{
    memset(out, 0, sizeof(*out));
    return tg_kv_parse(buf, len, tg_cg_cgstat_fields, tg_cg_cgstat_nfields, out, found);
}

int tg_parse_u64(const char *buf, size_t len, uint64_t *out)
{
    size_t i = 0;
    uint64_t v = 0;
    bool any = false;

    if (buf == NULL || out == NULL)
        return -EINVAL;
    while (i < len && (buf[i] == ' ' || buf[i] == '\t'))
        i++;
    for (; i < len && buf[i] >= '0' && buf[i] <= '9'; i++) {
        uint64_t d = (uint64_t)(buf[i] - '0');

        if (v > (UINT64_MAX - d) / 10)
            return -EOVERFLOW;
        v = v * 10 + d;
        any = true;
    }
    if (!any)
        return -EINVAL;
    for (; i < len; i++) {
        if (buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\n' && buf[i] != '\r' && buf[i] != '\0')
            return -EINVAL;
    }
    *out = v;
    return 0;
}
