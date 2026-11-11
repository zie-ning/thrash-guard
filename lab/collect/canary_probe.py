#!/usr/bin/env python3
"""VM 밖에서 컨테이너 안 카나리(tg-load canary)의 응답성을 1초마다 잰다 (측정 정의: 카나리 프로브).

매번 새 TCP 연결로 한 줄을 보내고 응답 한 줄을 기다린다.

출력 형식(탭 구분, 헤더 포함):
  start_unix_ns  latency_ms  ok

  ok : 1이면 제한 시간 안에 "ok ..." 응답을 받음

프로브 하나가 오래 걸려도 다음 프로브는 예정 시각에 시작한다(ssh_probe.py와 같은 방식).
"""

import argparse
import signal
import socket
import sys
import threading
import time


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--port", type=int, default=9100)
    ap.add_argument("--interval", type=float, default=1.0, help="프로브 간격(초)")
    ap.add_argument("--timeout", type=float, default=1.0, help="프로브 제한 시간(초)")
    args = ap.parse_args()

    stop = threading.Event()
    lock = threading.Lock()
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    signal.signal(signal.SIGTERM, lambda *_: stop.set())

    out = open(args.out, "a", buffering=1, encoding="utf-8")
    if out.tell() == 0:
        out.write("start_unix_ns\tlatency_ms\tok\n")

    def probe():
        start_ns = time.time_ns()
        t0 = time.monotonic()
        ok = 0
        try:
            with socket.create_connection(("127.0.0.1", args.port), timeout=args.timeout) as s:
                s.settimeout(max(0.05, args.timeout - (time.monotonic() - t0)))
                s.sendall(b"ping\n")
                data = s.recv(64)
                ok = 1 if data.startswith(b"ok") else 0
        except OSError:
            ok = 0
        latency_ms = (time.monotonic() - t0) * 1000
        with lock:
            out.write(f"{start_ns}\t{latency_ms:.1f}\t{ok}\n")

    print(f"카나리 프로브 시작: 127.0.0.1:{args.port}, {args.interval}s 간격 -> {args.out}", file=sys.stderr)
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
