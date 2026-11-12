#!/usr/bin/env python3
"""tg-sampler가 UDP로 보낸 줄을 받아 파일에 저장한다 .

VM 밖(WSL)에서 실행한다. 받은 줄 앞에 수신 시각(유닉스 나노초)을 붙여 그대로 기록한다.
원본을 손대지 않고 남겨 두고, CSV 변환과 분석은 나중 단계(lab/analyze)에서 한다.

출력 형식: <recv_unix_ns>\t<원본 줄>

일련번호(seq)가 건너뛰면 표준 오류에 알린다. 누락 자체가 "수집기도 버티지 못한 구간"을
나타내는 지표이므로 원본 파일에서도 다시 계산할 수 있다.
"""

import argparse
import signal
import socket
import sys
import time


def parse_seq(line: str):
    for tok in line.split():
        if tok.startswith("seq="):
            try:
                return int(tok[4:])
            except ValueError:
                return None
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--bind", default="127.0.0.1", help="수신 주소 (기본 127.0.0.1)")
    ap.add_argument("--port", type=int, default=9999, help="수신 포트 (기본 9999)")
    ap.add_argument("--out", required=True, help="기록할 파일 경로")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    sock.bind((args.bind, args.port))
    sock.settimeout(1.0)

    stop = False

    def on_signal(signum, frame):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    received = 0
    gaps = 0
    last_seq = None
    print(f"수신 대기: {args.bind}:{args.port} -> {args.out}", file=sys.stderr)

    with open(args.out, "a", buffering=1, encoding="utf-8") as out:
        while not stop:
            try:
                data, _ = sock.recvfrom(65535)
            except socket.timeout:
                continue
            recv_ns = time.time_ns()
            line = data.decode("utf-8", errors="replace").rstrip("\n")
            out.write(f"{recv_ns}\t{line}\n")
            received += 1

            seq = parse_seq(line)
            if seq is not None:
                if last_seq is not None and seq != last_seq + 1:
                    if seq > last_seq + 1:
                        gaps += seq - last_seq - 1
                        print(f"누락: seq {last_seq + 1}~{seq - 1}", file=sys.stderr)
                    else:
                        # 수집기가 재시작되면 seq가 0부터 다시 시작한다.
                        print(f"seq 재시작: {last_seq} -> {seq}", file=sys.stderr)
                last_seq = seq

    print(f"종료: 수신 {received}줄, 누락 {gaps}개", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
