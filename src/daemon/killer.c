/*
 * killer.c - cgroup.kill 쓰기 구현
 *
 * 종료 경로: 할당 금지. 이 파일의 함수는 힙과 stdio를 쓰지 않는다.
 */
#include "killer.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool name_ok(const char *name)
{
    size_t n;

    if (name == NULL)
        return false;
    n = strnlen(name, TG_TARGET_NAME_CAP);
    if (n == 0 || n >= TG_TARGET_NAME_CAP)
        return false;
    if (strchr(name, '/') != NULL)
        return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    return true;
}

/* dfd 아래 memory.current가 ref_fd와 같은 파일인가 */
static int same_cgroup(int dfd, int ref_fd)
{
    struct stat a;
    struct stat b;

    if (fstatat(dfd, "memory.current", &a, AT_SYMLINK_NOFOLLOW) != 0)
        return -errno;
    if (fstat(ref_fd, &b) != 0)
        return -errno;
    return (a.st_dev == b.st_dev && a.st_ino == b.st_ino) ? 0 : -ESTALE;
}

int tg_kill_cgroup(int parent_fd, const char *name)
{
    return tg_kill_cgroup_checked(parent_fd, name, -1);
}

int tg_kill_cgroup_checked(int parent_fd, const char *name, int ref_fd)
{
    int dfd;
    int fd;
    ssize_t w;
    int rc;

    if (parent_fd < 0 || !name_ok(name))
        return -EINVAL;

    /* 한 번 연 디렉터리 핸들로 확인과 쓰기를 모두 한다. 그 사이 이름이 바뀌어도 같은 cgroup이다 */
    do {
        dfd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    } while (dfd < 0 && errno == EINTR);
    if (dfd < 0)
        return -errno;

    if (ref_fd >= 0) {
        rc = same_cgroup(dfd, ref_fd);
        if (rc != 0) {
            close(dfd);
            return rc;
        }
    }

    do {
        fd = openat(dfd, "cgroup.kill", O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    } while (fd < 0 && errno == EINTR);
    close(dfd);
    if (fd < 0) {
        int err = errno;

        /* 디렉터리는 열렸으므로 cgroup.kill만 없는 것이다(커널 5.14 미만) */
        return err == ENOENT ? -ENOTSUP : -err;
    }

    do {
        w = write(fd, "1", 1);
    } while (w < 0 && errno == EINTR);
    if (w != 1) {
        int err = w < 0 ? errno : EIO;

        close(fd);
        return -err;
    }
    close(fd);
    return 0;
}
