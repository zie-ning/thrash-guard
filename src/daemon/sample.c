/*
 * sample.c - 샘플 수집 구현
 *
 * 핵심 경로: 할당 금지. 이 파일의 함수는 힙과 stdio를 쓰지 않는다.
 */
#include "sample.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "procfs.h"

int tg_reader_open(struct tg_reader *r, const char *psi_path, const char *io_path,
                   const char *meminfo_path)
{
    r->io_fd = -1;
    r->psi_fd = open(psi_path, O_RDONLY | O_CLOEXEC);
    if (r->psi_fd < 0) {
        int err = errno;

        r->meminfo_fd = -1;
        return -err;
    }
    if (io_path != NULL)
        r->io_fd = open(io_path, O_RDONLY | O_CLOEXEC);
    r->meminfo_fd = open(meminfo_path, O_RDONLY | O_CLOEXEC);
    if (r->meminfo_fd < 0) {
        int err = errno;

        close(r->psi_fd);
        r->psi_fd = -1;
        if (r->io_fd >= 0)
            close(r->io_fd);
        r->io_fd = -1;
        return -err;
    }
    return 0;
}

void tg_reader_close(struct tg_reader *r)
{
    if (r->psi_fd >= 0)
        close(r->psi_fd);
    if (r->io_fd >= 0)
        close(r->io_fd);
    if (r->meminfo_fd >= 0)
        close(r->meminfo_fd);
    r->psi_fd = -1;
    r->io_fd = -1;
    r->meminfo_fd = -1;
}

/* 핵심 경로: 할당 금지 */
int tg_sample_collect(struct tg_reader *r, const struct tg_targets *t, uint64_t now_ns,
                      struct tg_sample_buf *out)
{
    struct tg_psi psi;
    struct tg_psi io_psi;
    struct tg_meminfo mi;
    bool io_valid = false;
    uint64_t found = 0;
    uint64_t need = 0;
    size_t len = 0;
    uint32_t n = 0;
    int rc;

    out->read_failed = 0;

    rc = tg_read_fd(r->psi_fd, r->buf, sizeof(r->buf), &len);
    if (rc != 0)
        return rc;
    rc = tg_psi_parse(r->buf, len, &psi);
    if (rc != 0)
        return rc;
    if (!psi.has_full)
        return -EINVAL;

    if (r->io_fd >= 0) {
        rc = tg_read_fd(r->io_fd, r->buf, sizeof(r->buf), &len);
        if (rc != 0)
            return rc;
        rc = tg_psi_parse(r->buf, len, &io_psi);
        if (rc != 0)
            return rc;
        if (!io_psi.has_full)
            return -EINVAL;
        io_valid = true;
    }

    rc = tg_read_fd(r->meminfo_fd, r->buf, sizeof(r->buf), &len);
    if (rc != 0)
        return rc;
    rc = tg_meminfo_parse(r->buf, len, &mi, &found);
    if (rc != 0)
        return rc;
    need = (1ull << 0) | (1ull << 2); /* MemTotal, MemAvailable: tg_meminfo_fields의 0번과 2번 */
    if ((found & need) != need)
        return -EINVAL;

    for (uint32_t i = 0; i < t->n && n < TG_MAX_TARGETS; i++) {
        uint64_t bytes;

        rc = tg_read_fd(t->t[i].current_fd, r->buf, 64, &len);
        if (rc == 0)
            rc = tg_parse_u64(r->buf, len, &bytes);
        if (rc != 0) {
            out->read_failed++;
            continue;
        }
        out->cand[n].name = t->t[i].name;
        out->cand[n].mem_bytes = bytes;
        out->cand[n].first_seen_ns = t->t[i].first_seen_ns;
        out->cand[n].is_protected = t->t[i].protect != 0; /* 확인하지 못한 것도 보호로 다룬다 */
        n++;
    }

    out->s.now_ns = now_ns;
    out->s.sys_full_total_us = psi.full.total_us;
    out->s.io_valid = io_valid;
    out->s.io_full_total_us = io_valid ? io_psi.full.total_us : 0;
    out->s.mem_available_bytes = mi.mem_available_kb * 1024;
    out->s.mem_total_bytes = mi.mem_total_kb * 1024;
    out->s.targets_stale = t->scanned_ns == 0 ||
                           (now_ns > t->scanned_ns && now_ns - t->scanned_ns > TG_TARGETS_STALE_NS);
    out->s.cand = out->cand;
    out->s.ncand = n;
    return 0;
}
