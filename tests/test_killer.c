/*
 * test_killer.c - cgroup.kill 쓰기 단위 테스트
 *
 * 임시 디렉터리에 가짜 cgroup 폴더와 cgroup.kill 파일을 만든다. 실제로 프로세스가
 * 죽는지는 VM 실험에서 확인한다.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "killer.h"
#include "tg_test.h"

static char root[128];

static void mk_cg(const char *name, bool with_kill_file)
{
    char path[256];

    snprintf(path, sizeof(path), "%s/%s", root, name);
    TG_CHECK(mkdir(path, 0700) == 0);
    if (with_kill_file) {
        int fd;

        snprintf(path, sizeof(path), "%s/%s/cgroup.kill", root, name);
        fd = open(path, O_WRONLY | O_CREAT, 0600);
        TG_CHECK(fd >= 0);
        close(fd);
    }
}

static void read_kill_file(const char *name, char *out, size_t cap)
{
    char path[256];
    int fd;
    ssize_t n;

    snprintf(path, sizeof(path), "%s/%s/cgroup.kill", root, name);
    fd = open(path, O_RDONLY);
    TG_CHECK(fd >= 0);
    n = read(fd, out, cap - 1);
    out[n < 0 ? 0 : n] = '\0';
    close(fd);
}

static void test_writes_one(void)
{
    int dfd = open(root, O_RDONLY | O_DIRECTORY);
    char buf[8];

    TG_CHECK(dfd >= 0);
    mk_cg("a.scope", true);
    TG_CHECK(tg_kill_cgroup(dfd, "a.scope") == 0);
    read_kill_file("a.scope", buf, sizeof(buf));
    TG_CHECK(strcmp(buf, "1") == 0);
    close(dfd);
}

static void test_missing_cgroup(void)
{
    int dfd = open(root, O_RDONLY | O_DIRECTORY);

    TG_CHECK(tg_kill_cgroup(dfd, "gone.scope") == -ENOENT);
    close(dfd);
}

static void test_no_kill_file(void)
{
    int dfd = open(root, O_RDONLY | O_DIRECTORY);

    mk_cg("old.scope", false);
    TG_CHECK(tg_kill_cgroup(dfd, "old.scope") == -ENOTSUP);
    close(dfd);
}

static void test_bad_names(void)
{
    int dfd = open(root, O_RDONLY | O_DIRECTORY);
    char longname[TG_TARGET_NAME_CAP + 8];

    TG_CHECK(tg_kill_cgroup(dfd, NULL) == -EINVAL);
    TG_CHECK(tg_kill_cgroup(dfd, "") == -EINVAL);
    TG_CHECK(tg_kill_cgroup(dfd, ".") == -EINVAL);
    TG_CHECK(tg_kill_cgroup(dfd, "..") == -EINVAL);
    TG_CHECK(tg_kill_cgroup(dfd, "a.scope/x") == -EINVAL);
    TG_CHECK(tg_kill_cgroup(dfd, "../a.scope") == -EINVAL);
    TG_CHECK(tg_kill_cgroup(dfd, "/etc") == -EINVAL);
    memset(longname, 'x', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    TG_CHECK(tg_kill_cgroup(dfd, longname) == -EINVAL);
    TG_CHECK(tg_kill_cgroup(-1, "a.scope") == -EINVAL);
    close(dfd);
}

static void test_symlink_kill_file_refused(void)
{
    int dfd = open(root, O_RDONLY | O_DIRECTORY);
    char path[256];
    char target[256];
    char buf[8];
    int fd;

    /* cgroup.kill이 다른 파일로 향하는 심볼릭 링크면 따라가지 않는다 */
    mk_cg("link.scope", false);
    snprintf(target, sizeof(target), "%s/victim.txt", root);
    fd = open(target, O_WRONLY | O_CREAT, 0600);
    TG_CHECK(fd >= 0);
    close(fd);
    snprintf(path, sizeof(path), "%s/link.scope/cgroup.kill", root);
    TG_CHECK(symlink(target, path) == 0);

    TG_CHECK(tg_kill_cgroup(dfd, "link.scope") != 0);
    fd = open(target, O_RDONLY);
    TG_CHECK(fd >= 0);
    TG_CHECK(read(fd, buf, sizeof(buf)) == 0); /* victim.txt는 그대로 비어 있다 */
    close(fd);
    close(dfd);
}

static void test_checked_identity(void)
{
    int dfd = open(root, O_RDONLY | O_DIRECTORY);
    char path[256];
    char buf[8];
    int ref;

    mk_cg("c.scope", true);
    mk_cg("d.scope", true);
    snprintf(path, sizeof(path), "%s/c.scope/memory.current", root);
    {
        int fd = open(path, O_WRONLY | O_CREAT, 0600);

        TG_CHECK(fd >= 0);
        close(fd);
    }
    snprintf(path, sizeof(path), "%s/d.scope/memory.current", root);
    {
        int fd = open(path, O_WRONLY | O_CREAT, 0600);

        TG_CHECK(fd >= 0);
        close(fd);
    }

    /* 샘플링할 때 연 fd와 같은 cgroup이면 종료한다 */
    snprintf(path, sizeof(path), "%s/c.scope/memory.current", root);
    ref = open(path, O_RDONLY);
    TG_CHECK(ref >= 0);
    TG_CHECK(tg_kill_cgroup_checked(dfd, "c.scope", ref) == 0);
    read_kill_file("c.scope", buf, sizeof(buf));
    TG_CHECK(strcmp(buf, "1") == 0);

    /* 이름은 같아도 다른 cgroup을 가리키는 fd라면(다시 만들어진 경우) 쓰지 않는다 */
    TG_CHECK(tg_kill_cgroup_checked(dfd, "d.scope", ref) == -ESTALE);
    read_kill_file("d.scope", buf, sizeof(buf));
    TG_CHECK(strcmp(buf, "") == 0);

    close(ref);
    close(dfd);
}

static void touch_in(const char *dir, const char *file)
{
    char path[256];
    int fd;

    snprintf(path, sizeof(path), "%s/%s/%s", root, dir, file);
    fd = open(path, O_WRONLY | O_CREAT, 0600);
    TG_CHECK(fd >= 0);
    if (fd >= 0)
        close(fd);
}

static void test_same_name_recreated(void)
{
    int dfd = open(root, O_RDONLY | O_DIRECTORY);
    char from[256];
    char to[256];
    char path[256];
    char buf[8];
    int ref;

    /* 샘플링할 때 e.scope의 memory.current를 열어 둔다 */
    mk_cg("e.scope", true);
    touch_in("e.scope", "memory.current");
    snprintf(path, sizeof(path), "%s/e.scope/memory.current", root);
    ref = open(path, O_RDONLY);
    TG_CHECK(ref >= 0);

    /*
     * 같은 이름으로 cgroup이 다시 만들어진 상황: 새 cgroup을 먼저 만든 뒤(inode가 겹치지 않는다)
     * 옛 것을 치우고 같은 이름을 쓴다.
     */
    mk_cg("e.new", true);
    touch_in("e.new", "memory.current");
    snprintf(from, sizeof(from), "%s/e.scope", root);
    snprintf(to, sizeof(to), "%s/e.old", root);
    TG_CHECK(rename(from, to) == 0);
    snprintf(from, sizeof(from), "%s/e.new", root);
    snprintf(to, sizeof(to), "%s/e.scope", root);
    TG_CHECK(rename(from, to) == 0);

    /* 옛 fd로는 같은 이름의 새 cgroup을 종료하지 않는다 */
    TG_CHECK(tg_kill_cgroup_checked(dfd, "e.scope", ref) == -ESTALE);
    read_kill_file("e.scope", buf, sizeof(buf));
    TG_CHECK(strcmp(buf, "") == 0);

    /* 새 cgroup에서 다시 연 fd로는 종료한다 */
    close(ref);
    snprintf(path, sizeof(path), "%s/e.scope/memory.current", root);
    ref = open(path, O_RDONLY);
    TG_CHECK(ref >= 0);
    TG_CHECK(tg_kill_cgroup_checked(dfd, "e.scope", ref) == 0);
    read_kill_file("e.scope", buf, sizeof(buf));
    TG_CHECK(strcmp(buf, "1") == 0);

    close(ref);
    close(dfd);
}

int main(void)
{
    snprintf(root, sizeof(root), "/tmp/tg_killer_XXXXXX");
    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    TG_RUN(test_writes_one);
    TG_RUN(test_missing_cgroup);
    TG_RUN(test_no_kill_file);
    TG_RUN(test_bad_names);
    TG_RUN(test_symlink_kill_file_refused);
    TG_RUN(test_checked_identity);
    TG_RUN(test_same_name_recreated);

    {
        char cmd[200];

        snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
        if (system(cmd) != 0)
            fprintf(stderr, "임시 디렉터리 정리 실패: %s\n", root);
    }
    return tg_test_finish();
}
