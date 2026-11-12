/*
 * killer.c - cgroup.kill 쓰기 구현
 *
 * 종료 경로: 할당 금지. 이 파일의 함수는 힙과 stdio를 쓰지 않는다.
 */
#include "killer.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define KILL_FILE "/cgroup.kill"

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

int tg_kill_cgroup(int parent_fd, const char *name)
{
    char path[TG_TARGET_NAME_CAP + sizeof(KILL_FILE)];
    size_t n;
    int fd;
    ssize_t w;

    if (parent_fd < 0 || !name_ok(name))
        return -EINVAL;

    n = strlen(name);
    memcpy(path, name, n);
    memcpy(path + n, KILL_FILE, sizeof(KILL_FILE));

    do {
        fd = openat(parent_fd, path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        int err = errno;

        if (err == ENOENT) {
            /* cgroup 자체가 없는지, cgroup.kill만 없는지 구분한다 */
            int dfd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

            if (dfd >= 0) {
                close(dfd);
                return -ENOTSUP;
            }
        }
        return -err;
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
