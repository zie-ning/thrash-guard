/*
 * loop.c - 이벤트 루프 구현
 *
 * 핵심 경로: 할당 금지. 초기화 함수(tg_signals_block, tg_psi_trigger_open)만 stdio 없이 시스템 콜을 쓴다.
 */
#include "loop.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "mono.h"

int tg_signals_block(void)
{
    sigset_t set;
    int fd;

    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGHUP);
    if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0)
        return -EINVAL;
    /* 닫힌 파이프에 쓸 때 프로세스가 죽지 않게 한다. 쓰기 실패는 오류 코드로 받는다. */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        return -errno;
    fd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
    if (fd < 0)
        return -errno;
    return fd;
}

int tg_psi_trigger_open(uint32_t stall_us, uint32_t window_us)
{
    char spec[64];
    int n;
    int fd;
    ssize_t w;

    n = snprintf(spec, sizeof(spec), "full %u %u", (unsigned)stall_us, (unsigned)window_us);
    if (n <= 0 || (size_t)n >= sizeof(spec))
        return -EINVAL;

    fd = open("/proc/pressure/memory", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    do {
        w = write(fd, spec, (size_t)n + 1); /* 커널 문서 예시처럼 끝의 NUL까지 쓴다 */
    } while (w < 0 && errno == EINTR);
    if (w < 0) {
        int err = errno;

        close(fd);
        return -err;
    }
    return fd;
}

/* 밀린 신호를 모두 읽고 종료 신호가 있었는지 돌려준다. */
static bool read_stop_signal(int sig_fd)
{
    bool stop = false;

    for (;;) {
        struct signalfd_siginfo si;
        ssize_t n = read(sig_fd, &si, sizeof(si));

        if (n != (ssize_t)sizeof(si))
            break; /* EAGAIN(더 없음) 또는 오류 */
        if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT)
            stop = true;
        /* SIGHUP은 아직 다시 읽기 기능이 없어 무시한다. */
    }
    return stop;
}

/* 다음 틱까지 남은 시간(ms, 올림). 이미 지났으면 0. */
static int timeout_ms(uint64_t now_ns, uint64_t deadline_ns)
{
    uint64_t ms;

    if (deadline_ns <= now_ns)
        return 0;
    ms = (deadline_ns - now_ns + 999999ull) / 1000000ull;
    return ms > (uint64_t)INT_MAX ? INT_MAX : (int)ms;
}

int tg_loop_run(struct tg_engine *e, const struct tg_loop_fds *fds)
{
    for (;;) {
        struct pollfd p[2];
        int timeout = -1;
        int n;

        if (tg_engine_watching(e))
            timeout = timeout_ms(tg_now_ns(), tg_engine_deadline(e));

        p[0].fd = fds->sig_fd;
        p[0].events = POLLIN;
        p[0].revents = 0;
        p[1].fd = fds->trigger_fd;
        p[1].events = fds->trigger_events;
        p[1].revents = 0;

        n = poll(p, 2, timeout);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }

        if ((p[0].revents & POLLIN) != 0 && read_stop_signal(fds->sig_fd))
            return 0;
        if ((p[1].revents & (POLLERR | POLLNVAL)) != 0)
            return -EIO;
        if ((p[1].revents & fds->trigger_events) != 0) {
            uint64_t buf;

            if ((fds->trigger_events & POLLIN) != 0) {
                /* eventfd 같은 대체 트리거는 읽어서 비운다. 실제 PSI 트리거는 POLLPRI만 쓴다. */
                if (read(fds->trigger_fd, &buf, sizeof(buf)) < 0 && errno != EAGAIN)
                    return -errno;
            }
            tg_engine_trigger(e, tg_now_ns());
        }

        if (tg_engine_watching(e)) {
            uint64_t now = tg_now_ns();

            if (now >= tg_engine_deadline(e))
                tg_engine_tick(e, now);
        }
    }
}
