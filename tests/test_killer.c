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

int main(void)
{
    snprintf(root, sizeof(root), "/tmp/tg_killer_%d", (int)getpid());
    TG_CHECK(mkdir(root, 0700) == 0);

    TG_RUN(test_writes_one);
    TG_RUN(test_missing_cgroup);
    TG_RUN(test_no_kill_file);
    TG_RUN(test_bad_names);
    TG_RUN(test_symlink_kill_file_refused);

    {
        char cmd[200];

        snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
        if (system(cmd) != 0)
            fprintf(stderr, "임시 디렉터리 정리 실패: %s\n", root);
    }
    return tg_test_finish();
}
