# packaging

배포용 설정 예시다. 데몬은 다른 서비스의 설정을 쓰지 않으므로 사용자가 직접 설치한다.

| 파일 | 내용 |
|---|---|
| `dropins/sshd.service.d/protect.conf` | sshd 보호(`OOMScoreAdjust=-1000`, `MemoryLow=` 예시) |

보호 대상은 서비스 설정에 `OOMScoreAdjust=-1000`을 적으면 된다. 그 서비스가 든 cgroup(하위 포함)은 종료 후보에서 빠진다.
로그·관측 에이전트(journald, SSM Agent, CloudWatch Agent 등)는 환경마다 달라서 같은 방식으로 직접 작성한다.
설계는 [docs/design/protection.md](../docs/design/protection.md)에 있다.
