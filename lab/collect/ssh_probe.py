#!/usr/bin/env python3
"""VM 밖에서 SSH 응답성을 1초마다 잰다 (측정 정의: 외부 SSH 프로브).

매번 **새 연결**로 `ssh vm true`를 실행한다. 연결 재사용(ControlMaster)은 sshd의
fork·로그인 경로를 건너뛰므로 쓰지 않는다. 길벗에서 SSH가 먼저 멈춘 경로를 그대로 재기 위함이다.

출력 형식(탭 구분, 헤더 포함):
  start_unix_ns  latency_ms  ok  rc

  ok  : 1이면 제한 시간 안에 성공
  rc  : ssh 종료 코드. 제한 시간을 넘기면 -1

프로브 하나가 오래 걸려도 다음 프로브는 예정 시각에 시작한다(동시에 여러 개가 돌 수 있다).
그래야 응답 불능 구간에도 1초 간격의 기록이 남는다.
"""

import argparse
import os
import signal
import subprocess
import sys
import threading
import time


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--port", type=int, default=2222)
    ap.add_argument("--user", default="tg")
    ap.add_argument("--key", default=os.path.expanduser("~/.ssh/tg_vm_ed25519"))
    ap.add_argument("--interval", type=float, default=1.0, help="프로브 간격(초)")
    ap.add_argument("--timeout", type=float, default=5.0, help="프로브 제한 시간(초)")
    args = ap.parse_args()

    cmd = [
        "ssh", "-i", args.key, "-p", str(args.port),
        "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
        "-o", "LogLevel=ERROR", "-o", "BatchMode=yes",
        "-o", f"ConnectTimeout={int(args.timeout)}",
        "-o", "ControlMaster=no", "-o", "ControlPath=none",
        f"{args.user}@127.0.0.1", "true",
    ]

    stop = threading.Event()
    lock = threading.Lock()
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    signal.signal(signal.SIGTERM, lambda *_: stop.set())

    out = open(args.out, "a", buffering=1, encoding="utf-8")
    if out.tell() == 0:
        out.write("start_unix_ns\tlatency_ms\tok\trc\n")

    def probe():
        start_ns = time.time_ns()
        t0 = time.monotonic()
        try:
            rc = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, timeout=args.timeout).returncode
        except subprocess.TimeoutExpired:
            rc = -1
        latency_ms = (time.monotonic() - t0) * 1000
        ok = 1 if rc == 0 else 0
        with lock:
            out.write(f"{start_ns}\t{latency_ms:.1f}\t{ok}\t{rc}\n")

    print(f"SSH 프로브 시작: 127.0.0.1:{args.port}, {args.interval}s 간격 -> {args.out}",
          file=sys.stderr)
    next_t = time.monotonic()
    threads = []
    while not stop.is_set():
        t = threading.Thread(target=probe, daemon=True)
        t.start()
        threads.append(t)
        threads = [x for x in threads if x.is_alive()]
        next_t += args.interval
        stop.wait(max(0.0, next_t - time.monotonic()))

    for t in threads:
        t.join(timeout=args.timeout + 1)
    out.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
