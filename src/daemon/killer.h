/*
 * killer.h - 종료 실행 (cgroup.kill 쓰기)
 *
 * target_parent를 연 디렉터리 핸들과 cgroup 이름을 받아 "<이름>/cgroup.kill"에 "1"을 쓴다.
 * 커널이 그 cgroup의 모든 프로세스에 SIGKILL을 전달한다(리눅스 5.14 이상).
 * 성공은 "신호를 전달했다"까지이며 프로세스가 사라졌다는 뜻이 아니다. 해소 여부는 PSI로 본다.
 *
 * dry-run은 이 함수가 모른다. 호출자가 dry-run이면 부르지 않는다.
 * 힙 할당과 stdio를 쓰지 않는다. 반환값은 성공 0, 실패 시 음수 errno:
 *   -EINVAL  잘못된 이름(빈 이름, '/' 포함, ".", "..", 용량 초과)
 *   -ENOENT  cgroup이 이미 없다
 *   -ENOTSUP cgroup은 있으나 cgroup.kill이 없다(커널 5.14 미만)
 *   -EACCES, -EPERM 등 그 밖의 값은 open·write의 errno
 */
#ifndef TG_KILLER_H
#define TG_KILLER_H

#include "targets.h"

int tg_kill_cgroup(int parent_fd, const char *name);

#endif /* TG_KILLER_H */
