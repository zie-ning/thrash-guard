/*
 * cgscan.c - 감시 대상 cgroup 표 구현
 *
 * 핵심 경로: 할당 금지. 스택 버퍼와 구조체 안의 고정 버퍼만 쓴다.
 */
#include "cgscan.h"

#include "procfs.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* linux_dirent64의 배치. 정렬 문제를 피하려고 memcpy로 필드를 꺼낸다. */
#define DENT_RECLEN_OFF 16
#define DENT_TYPE_OFF 18
#define DENT_NAME_OFF 19
#define DT_DIR_VAL 4

static void target_close(struct tg_cg_target *t)
{
    if (t->pressure_fd >= 0)
        close(t->pressure_fd);
    if (t->stat_fd >= 0)
        close(t->stat_fd);
    if (t->events_fd >= 0)
        close(t->events_fd);
    t->pressure_fd = t->stat_fd = t->events_fd = -1;
    t->name[0] = '\0';
}

static void targets_close(struct tg_cg_set *s)
{
    size_t i;

    for (i = 0; i < TG_CG_MAX; i++)
        target_close(&s->t[i]);
    s->n = 0;
}

/* 줄 형식(공백 구분 key=value)을 깨지 않는 이름만 받는다. */
static bool name_ok(const char *name)
{
    size_t i;

    if (name[0] == '\0' || name[0] == '.')
        return false;
    for (i = 0; name[i] != '\0'; i++) {
        char c = name[i];

        if (i + 1 >= TG_CG_NAME)
            return false;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.' || c == '@'))
            return false;
    }
    return true;
}

static void try_add(struct tg_cg_set *s, const char *name)
{
    struct tg_cg_target *t;
    int dfd;

    if (s->n >= TG_CG_MAX) {
        s->skipped++;
        return;
    }
    dfd = openat(s->parent_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0)
        return;
    t = &s->t[s->n];
    t->pressure_fd = openat(dfd, "memory.pressure", O_RDONLY | O_CLOEXEC);
    t->stat_fd = openat(dfd, "memory.stat", O_RDONLY | O_CLOEXEC);
    t->events_fd = openat(dfd, "memory.events", O_RDONLY | O_CLOEXEC);
    close(dfd);
    if (t->pressure_fd < 0 || t->stat_fd < 0) {
        /* memory 컨트롤러가 꺼진 cgroup이다. 대상이 아니다. */
        target_close(t);
        return;
    }
    strncpy(t->name, name, TG_CG_NAME - 1);
    t->name[TG_CG_NAME - 1] = '\0';
    s->n++;
}

static int read_cgstat(const struct tg_cg_set *s, struct tg_cg_cgstat *out)
{
    char buf[512];
    size_t len = 0;
    int rc;

    if (s->cgstat_fd < 0)
        return -ENOENT;
    rc = tg_read_fd(s->cgstat_fd, buf, sizeof(buf), &len);
    if (rc != 0)
        return rc;
    return tg_cg_cgstat_parse(buf, len, out, NULL);
}

static void scan(struct tg_cg_set *s)
{
    struct tg_cg_cgstat cs;
    long nread;

    /* 스캔 도중 바뀐 것을 놓치지 않도록, 기준값은 스캔 전에 읽는다. */
    if (read_cgstat(s, &cs) == 0) {
        s->last_desc = cs.nr_descendants;
        s->last_dying = cs.nr_dying_descendants;
    }

    targets_close(s);
    s->skipped = 0;
    s->need_rescan = false;

    if (lseek(s->parent_fd, 0, SEEK_SET) < 0)
        return;
    while ((nread = syscall(SYS_getdents64, s->parent_fd, s->dents, sizeof(s->dents))) > 0) {
        long off = 0;

        while (off < nread) {
            const char *p = s->dents + off;
            unsigned short reclen;
            unsigned char type;

            memcpy(&reclen, p + DENT_RECLEN_OFF, sizeof(reclen));
            memcpy(&type, p + DENT_TYPE_OFF, sizeof(type));
            if (reclen == 0)
                return;
            if (type == DT_DIR_VAL && name_ok(p + DENT_NAME_OFF))
                try_add(s, p + DENT_NAME_OFF);
            off += reclen;
        }
    }
}

void tg_cg_set_init(struct tg_cg_set *s)
{
    size_t i;

    memset(s, 0, sizeof(*s));
    s->parent_fd = -1;
    s->cgstat_fd = -1;
    for (i = 0; i < TG_CG_MAX; i++)
        s->t[i].pressure_fd = s->t[i].stat_fd = s->t[i].events_fd = -1;
}

int tg_cg_set_open(struct tg_cg_set *s, const char *parent_path)
{
    tg_cg_set_init(s);
    s->parent_fd = open(parent_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (s->parent_fd < 0)
        return -errno;
    s->cgstat_fd = openat(s->parent_fd, "cgroup.stat", O_RDONLY | O_CLOEXEC);
    scan(s);
    return 0;
}

bool tg_cg_set_refresh(struct tg_cg_set *s)
{
    struct tg_cg_cgstat cs;

    if (s->parent_fd < 0)
        return false;
    if (!s->need_rescan) {
        if (read_cgstat(s, &cs) != 0)
            return false;
        if (cs.nr_descendants == s->last_desc && cs.nr_dying_descendants == s->last_dying)
            return false;
    }
    scan(s);
    return true;
}

void tg_cg_set_mark_stale(struct tg_cg_set *s)
{
    s->need_rescan = true;
}

void tg_cg_set_close(struct tg_cg_set *s)
{
    targets_close(s);
    if (s->cgstat_fd >= 0)
        close(s->cgstat_fd);
    if (s->parent_fd >= 0)
        close(s->parent_fd);
    s->cgstat_fd = s->parent_fd = -1;
}
