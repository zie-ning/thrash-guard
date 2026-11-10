#!/usr/bin/env python3
"""QEMU QMP 명령을 보낸다.

멈춘 VM을 강제로 리셋하거나 끌 때 쓴다. VM 안의 상태와 관계없이 QEMU가 직접 처리하므로
스래싱으로 SSH가 응답하지 않아도 동작한다.

사용 예:
  qmp.py status          # 실행 상태
  qmp.py reset           # 전원 리셋 (system_reset). WHPX에서는 재부팅처럼 실패할 수 있어
                         # 실험 스크립트는 quit 후 새로 켜는 방식(reset-run.sh)을 쓴다
  qmp.py powerdown       # ACPI 종료 요청 (정상 종료)
  qmp.py quit            # QEMU 프로세스 즉시 종료
"""

import json
import socket
import sys

COMMANDS = {
    "status": "query-status",
    "reset": "system_reset",
    "powerdown": "system_powerdown",
    "quit": "quit",
}


def recv_json(f):
    while True:
        line = f.readline()
        if not line:
            raise ConnectionError("QMP 연결이 끊겼다")
        msg = json.loads(line)
        # 비동기 이벤트는 건너뛰고 응답만 돌려준다.
        if "event" not in msg:
            return msg


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        print(__doc__, file=sys.stderr)
        return 2
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 4444

    try:
        sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    except OSError as e:
        print(f"QMP 접속 실패 (127.0.0.1:{port}): {e}", file=sys.stderr)
        return 1
    f = sock.makefile("rw", encoding="utf-8")
    recv_json(f)  # 인사말
    f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
    f.flush()
    recv_json(f)

    f.write(json.dumps({"execute": COMMANDS[sys.argv[1]]}) + "\n")
    f.flush()
    try:
        resp = recv_json(f)
    except ConnectionError:
        # quit은 응답 직후 연결이 끊길 수 있다.
        resp = {"return": {}}
    if "error" in resp:
        print(f"QMP 오류: {resp['error']}", file=sys.stderr)
        return 1
    print(json.dumps(resp.get("return", {}), ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
