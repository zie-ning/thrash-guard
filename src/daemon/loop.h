/*
 * loop.h - 이벤트 루프 (메인 스레드)
 *
 * poll()로 세 가지를 기다린다.
 *   1. 종료 신호(signalfd)       SIGTERM, SIGINT
 *   2. 커널 PSI 트리거           POLLPRI
 *   3. 관찰 중에만 다음 틱 시각   poll의 대기 시간(timeout)으로 처리한다(별도 타이머 fd 없음)
 * 관찰 중이 아닐 때는 대기 시간 없이(무한) 잠든다.
 *
 * 신호는 스레드를 만들기 전에 tg_signals_block으로 막아야 한다. 새 스레드는 막힌 상태를
 * 물려받으므로 어느 스레드도 신호 때문에 끊기지 않고, 신호는 signalfd로만 들어온다.
 */
#ifndef TG_LOOP_H
#define TG_LOOP_H

#include <poll.h>
#include <stdint.h>

#include "engine.h"

/*
 * SIGTERM, SIGINT, SIGHUP를 막고 SIGPIPE는 무시한 뒤 signalfd를 돌려준다.
 * 반드시 첫 스레드를 만들기 전에 부른다. 실패하면 음수 errno.
 */
int tg_signals_block(void);

/*
 * 커널 PSI 트리거를 등록하고 poll할 fd를 돌려준다: "full <stall_us> <window_us>".
 * 실패하면 음수 errno(비root, PSI 꺼짐, 범위 밖 값 등).
 */
int tg_psi_trigger_open(uint32_t stall_us, uint32_t window_us);

struct tg_loop_fds {
    int sig_fd;           /* tg_signals_block이 돌려준 signalfd */
    int trigger_fd;       /* tg_psi_trigger_open이 돌려준 fd */
    short trigger_events; /* 보통 POLLPRI. 테스트에서는 eventfd라 POLLIN */
};

/*
 * 종료 신호를 받을 때까지 돈다. 정상 종료는 0.
 * 트리거 fd가 오류 상태가 되면 -EIO를 돌려준다(커널 트리거가 사라진 경우).
 */
int tg_loop_run(struct tg_engine *e, const struct tg_loop_fds *fds);

#endif /* TG_LOOP_H */
