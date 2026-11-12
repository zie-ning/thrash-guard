/*
 * test_targets.c - 후보 cgroup 표와 스캔 스레드 단위 테스트
 *
 * 임시 디렉터리에 cgroup 구조를 흉내 낸다. 후보 조건은 "하위 디렉터리에
 * memory.pressure와 memory.current 파일이 모두 열린다"이다.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "scanner.h"
#include "targets.h"
#include "tg_test.h"

static char root[64];

static void touch(const char *path)
{
    int fd = open(path, O_CREAT | O_WRONLY, 0644);

    if (fd >= 0)
        close(fd);
}

static void mk_cg(const char *name, bool pressure, bool stat_file)
{
    char p[256];

    snprintf(p, sizeof(p), "%s/%s", root, name);
    mkdir(p, 0755);
    if (pressure) {
        snprintf(p, sizeof(p), "%s/%s/memory.pressure", root, name);
        touch(p);
    }
    if (stat_file) {
        snprintf(p, sizeof(p), "%s/%s/memory.current", root, name);
        touch(p);
    }
}

static void rm_cg(const char *name)
{
    char p[256];

    snprintf(p, sizeof(p), "%s/%s/memory.pressure", root, name);
    unlink(p);
    snprintf(p, sizeof(p), "%s/%s/memory.current", root, name);
    unlink(p);
    snprintf(p, sizeof(p), "%s/%s", root, name);
    rmdir(p);
}

static void rm_file(const char *name)
{
    char p[256];

    snprintf(p, sizeof(p), "%s/%s", root, name);
    unlink(p);
}

static bool fd_open(int fd)
{
    return fd >= 0 && fcntl(fd, F_GETFD) != -1;
}

static void test_rebuild_basic(void)
{
    struct tg_targets x;
    const struct tg_target *a;

    mk_cg("a", true, true);
    mk_cg("b", true, true);
    mk_cg("c", true, false); /* memory.current 없음: 제외 */
    {
        char p[256];
        snprintf(p, sizeof(p), "%s/f", root);
        touch(p); /* 일반 파일: 제외 */
    }

    tg_targets_init(&x);
    TG_CHECK(tg_targets_rebuild(&x, NULL, root, 777, true) == 0);
    TG_CHECK_EQ_U64(x.n, 2);
    TG_CHECK_EQ_U64(x.skipped, 0);
    a = tg_targets_find(&x, "a");
    TG_CHECK(a != NULL);
    TG_CHECK(tg_targets_find(&x, "b") != NULL);
    TG_CHECK(tg_targets_find(&x, "c") == NULL);
    TG_CHECK(tg_targets_find(&x, "f") == NULL);
    if (a != NULL) {
        TG_CHECK(fd_open(a->pressure_fd));
        TG_CHECK(fd_open(a->current_fd));
        TG_CHECK(a->pressure_fd != a->current_fd);
        TG_CHECK_EQ_U64(a->first_seen_ns, 0); /* initial은 유예 면제 */
    }

    tg_targets_release(&x, NULL);
    TG_CHECK_EQ_U64(x.n, 0);
    rm_cg("a");
    rm_cg("b");
    rm_cg("c");
    rm_file("f");
}

static void test_rebuild_reuse_and_remove(void)
{
    struct tg_targets x;
    struct tg_targets y;
    int a_fd;
    int b_pfd;
    int b_sfd;
    const struct tg_target *e;

    mk_cg("a", true, true);
    mk_cg("b", true, true);
    tg_targets_init(&x);
    tg_targets_init(&y);
    TG_CHECK(tg_targets_rebuild(&x, NULL, root, 100, true) == 0);
    a_fd = tg_targets_find(&x, "a")->pressure_fd;
    b_pfd = tg_targets_find(&x, "b")->pressure_fd;
    b_sfd = tg_targets_find(&x, "b")->current_fd;

    /* b가 사라지고 d가 생긴다 */
    rm_cg("b");
    mk_cg("d", true, true);
    TG_CHECK(tg_targets_rebuild(&y, &x, root, 500, false) == 0);
    TG_CHECK_EQ_U64(y.n, 2);
    e = tg_targets_find(&y, "a");
    TG_CHECK(e != NULL && e->pressure_fd == a_fd); /* fd 공유 */
    TG_CHECK(e != NULL && e->first_seen_ns == 0);  /* 처음 발견 시각 유지 */
    e = tg_targets_find(&y, "d");
    TG_CHECK(e != NULL && e->first_seen_ns == 500); /* 새 항목은 지금 시각 */
    TG_CHECK(tg_targets_find(&y, "b") == NULL);
    /* x가 아직 b를 들고 있으므로 fd는 열려 있어야 한다 */
    TG_CHECK(fd_open(b_pfd));
    TG_CHECK(fd_open(b_sfd));

    /* x를 다시 만들 때 y에 없는 b의 fd가 닫힌다 */
    TG_CHECK(tg_targets_rebuild(&x, &y, root, 900, false) == 0);
    TG_CHECK(!fd_open(b_pfd));
    TG_CHECK(!fd_open(b_sfd));
    TG_CHECK_EQ_U64(x.n, 2);
    TG_CHECK(fd_open(tg_targets_find(&x, "a")->pressure_fd));
    TG_CHECK(tg_targets_find(&x, "a")->pressure_fd == a_fd);

    tg_targets_release(&x, &y);
    tg_targets_release(&y, NULL);
    rm_cg("a");
    rm_cg("d");
}

static void test_rebuild_failure_keeps_cur(void)
{
    struct tg_targets x;
    struct tg_targets y;
    int a_fd;

    mk_cg("a", true, true);
    tg_targets_init(&x);
    tg_targets_init(&y);
    TG_CHECK(tg_targets_rebuild(&x, NULL, root, 1, true) == 0);
    a_fd = tg_targets_find(&x, "a")->pressure_fd;

    TG_CHECK(tg_targets_rebuild(&y, &x, "/nonexistent/tg", 2, false) == -ENOENT);
    TG_CHECK_EQ_U64(y.n, 0);
    TG_CHECK(fd_open(a_fd)); /* 실패해도 현재 표는 그대로 */
    TG_CHECK_EQ_U64(x.n, 1);

    tg_targets_release(&x, NULL);
    rm_cg("a");
}

static void test_recreated_cgroup_is_new_entry(void)
{
    struct tg_targets x;
    struct tg_targets y;
    const struct tg_target *e;
    char p[256];
    char q[256];
    int old_fd;

    mk_cg("a", true, true);
    tg_targets_init(&x);
    tg_targets_init(&y);
    TG_CHECK(tg_targets_rebuild(&x, NULL, root, 1, true) == 0);
    e = tg_targets_find(&x, "a");
    TG_CHECK(e != NULL && e->first_seen_ns == 0);
    old_fd = e->current_fd;

    /* 같은 이름으로 다시 만들어진 cgroup: memory.current가 다른 파일이 되었다 */
    snprintf(p, sizeof(p), "%s/a/memory.current.new", root);
    snprintf(q, sizeof(q), "%s/a/memory.current", root);
    touch(p);
    TG_CHECK(rename(p, q) == 0);

    TG_CHECK(tg_targets_rebuild(&y, &x, root, 5, false) == 0);
    e = tg_targets_find(&y, "a");
    TG_CHECK(e != NULL);
    if (e != NULL) {
        TG_CHECK(e->current_fd != old_fd); /* 옛 fd를 재사용하지 않는다 */
        TG_CHECK_EQ_U64(e->first_seen_ns, 5); /* 새로 발견한 것으로 다뤄 유예가 다시 시작된다 */
    }

    /* 옛 표를 버리면 옛 fd가 닫힌다 */
    tg_targets_release(&x, &y);
    TG_CHECK(!fd_open(old_fd));
    tg_targets_release(&y, NULL);
    rm_cg("a");
    snprintf(p, sizeof(p), "%s/a", root);
    rmdir(p);
}

static void test_overflow(void)
{
    struct tg_targets x;
    char name[16];

    for (int i = 0; i < 34; i++) {
        snprintf(name, sizeof(name), "c%02d", i);
        mk_cg(name, true, true);
    }
    tg_targets_init(&x);
    TG_CHECK(tg_targets_rebuild(&x, NULL, root, 1, true) == 0);
    TG_CHECK_EQ_U64(x.n, TG_MAX_TARGETS);
    TG_CHECK_EQ_U64(x.skipped, 2);

    /* 새 후보가 늘어도 기존 32개는 밀려나지 않는다 */
    {
        struct tg_targets y;
        char kept[TG_MAX_TARGETS][16];

        for (uint32_t i = 0; i < x.n; i++)
            snprintf(kept[i], sizeof(kept[i]), "%s", x.t[i].name);
        for (int i = 0; i < 3; i++) {
            snprintf(name, sizeof(name), "new%d", i);
            mk_cg(name, true, true);
        }
        tg_targets_init(&y);
        TG_CHECK(tg_targets_rebuild(&y, &x, root, 2, false) == 0);
        TG_CHECK_EQ_U64(y.n, TG_MAX_TARGETS);
        TG_CHECK_EQ_U64(y.skipped, 5);
        for (uint32_t i = 0; i < TG_MAX_TARGETS; i++)
            TG_CHECK(tg_targets_find(&y, kept[i]) != NULL);
        tg_targets_release(&y, &x);
        for (int i = 0; i < 3; i++) {
            snprintf(name, sizeof(name), "new%d", i);
            rm_cg(name);
        }
    }

    tg_targets_release(&x, NULL);
    for (int i = 0; i < 34; i++) {
        snprintf(name, sizeof(name), "c%02d", i);
        rm_cg(name);
    }
}

static void test_long_name(void)
{
    struct tg_targets x;
    char name[TG_TARGET_NAME_CAP + 8];
    char ok[TG_TARGET_NAME_CAP];

    /* Docker scope 이름 길이(약 80자)는 담긴다 */
    memset(ok, 'x', 80);
    ok[80] = '\0';
    mk_cg(ok, true, true);
    /* 용량을 넘는 이름은 조용히 제외된다 */
    memset(name, 'y', TG_TARGET_NAME_CAP + 4);
    name[TG_TARGET_NAME_CAP + 4] = '\0';
    mk_cg(name, true, true);

    tg_targets_init(&x);
    TG_CHECK(tg_targets_rebuild(&x, NULL, root, 1, true) == 0);
    TG_CHECK_EQ_U64(x.n, 1);
    TG_CHECK(tg_targets_find(&x, ok) != NULL);

    tg_targets_release(&x, NULL);
    rm_cg(ok);
    rm_cg(name);
}

static bool wait_for(struct tg_scanner *s, const char *name, bool present)
{
    for (int i = 0; i < 150; i++) {
        const struct tg_targets *t = tg_scanner_acquire(s);
        bool has = tg_targets_find(t, name) != NULL;

        tg_scanner_release(s);
        if (has == present)
            return true;
        usleep(20000);
    }
    return false;
}

static void test_scanner(void)
{
    struct tg_scanner s;
    const struct tg_targets *t;

    mk_cg("a", true, true);

    /* 없는 경로면 시작하지 못한다 */
    TG_CHECK(tg_scanner_start(&s, "/nonexistent/tg", 20) == -ENOENT);
    tg_scanner_stop(&s); /* 시작 실패 후 호출해도 안전 */

    TG_CHECK(tg_scanner_start(&s, root, 20) == 0);
    t = tg_scanner_acquire(&s);
    TG_CHECK_EQ_U64(t->n, 1);
    TG_CHECK(tg_targets_find(t, "a") != NULL);
    TG_CHECK(tg_targets_find(t, "a")->first_seen_ns == 0);
    tg_scanner_release(&s);

    /* 새 항목이 나타나면 공개 표에 반영된다 */
    mk_cg("e", true, true);
    TG_CHECK(wait_for(&s, "e", true));
    t = tg_scanner_acquire(&s);
    TG_CHECK(tg_targets_find(t, "e") != NULL && tg_targets_find(t, "e")->first_seen_ns > 0);
    tg_scanner_release(&s);

    /* 사라진 항목은 표에서 빠진다 */
    rm_cg("a");
    TG_CHECK(wait_for(&s, "a", false));
    TG_CHECK(atomic_load(&s.scans) > 0);

    tg_scanner_stop(&s);
    tg_scanner_stop(&s); /* 두 번 불러도 안전 */
    rm_cg("e");
}

static void test_scanner_holds_table(void)
{
    struct tg_scanner s;
    const struct tg_targets *t;
    unsigned scans_before;

    mk_cg("a", true, true);
    TG_CHECK(tg_scanner_start(&s, root, 20) == 0);

    /* 메인이 표를 잡고 있는 동안에는 그 표가 바뀌지 않는다 */
    t = tg_scanner_acquire(&s);
    TG_CHECK_EQ_U64(t->n, 1);
    mk_cg("g", true, true);
    scans_before = atomic_load(&s.scans);
    for (int i = 0; i < 15; i++) {
        usleep(20000);
        TG_CHECK_EQ_U64(t->n, 1); /* 잡은 표는 그대로 */
    }
    tg_scanner_release(&s);
    /* 놓으면 갱신이 진행된다 */
    TG_CHECK(wait_for(&s, "g", true));
    TG_CHECK(atomic_load(&s.scans) >= scans_before);

    tg_scanner_stop(&s);
    rm_cg("a");
    rm_cg("g");
}

int main(void)
{
    snprintf(root, sizeof(root), "/tmp/tg_targets_XXXXXX");
    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 2;
    }

    TG_RUN(test_rebuild_basic);
    TG_RUN(test_rebuild_reuse_and_remove);
    TG_RUN(test_rebuild_failure_keeps_cur);
    TG_RUN(test_recreated_cgroup_is_new_entry);
    TG_RUN(test_overflow);
    TG_RUN(test_long_name);
    TG_RUN(test_scanner);
    TG_RUN(test_scanner_holds_table);

    rmdir(root);
    return tg_test_finish();
}
