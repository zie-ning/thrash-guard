# packaging

배포용 설정 예시다. 데몬은 다른 서비스의 설정을 쓰지 않으므로 사용자가 직접 설치한다.

| 파일 | 내용 |
|---|---|
| `thrash-guard.service` | 데몬 서비스 파일(dry-run 시작, 재시작, 자기 보호, `CPUWeight`) |
| `thrash-guard.conf` | 설정 예시(모두 기본값, 주석 처리) |
| `dropins/sshd.service.d/protect.conf` | sshd 종료 보호(`OOMScoreAdjust=-1000`). `MemoryLow=`는 효과가 입증되지 않아 주석 처리한 선택 항목 |

보호 대상은 서비스 설정에 `OOMScoreAdjust=-1000`을 적으면 된다. 그 서비스가 든 cgroup(하위 포함)은 종료 후보에서 빠진다. 이 한 줄이 필수 설정이다.
스래싱 중 메모리가 쫓겨나지 않게 하는 `MemoryLow=`/`MemoryMin=`은 선택이다. VM 실험에서는 효과가 없었고 크기를 직접 정해야 하며 부모 slice까지 설정해야 해서 기본으로 권하지 않는다(설계: [docs/design/protection.md](../docs/design/protection.md) 2.2절).
로그·관측 에이전트(journald, SSM Agent, CloudWatch Agent 등)는 환경마다 달라서 같은 방식으로 직접 작성한다.
설계는 [docs/design/protection.md](../docs/design/protection.md)에 있다.

## 종료 후보 범위

데몬 설정의 `target_match`가 종료해도 되는 cgroup 이름을 정한다(기본은 컨테이너). 일반 서비스를 후보로 쓰려면 이름을 직접 적는다.

```ini
target_parent = /sys/fs/cgroup/system.slice
target_match  = myapp.service, worker@*.service
```

보호(`OOMScoreAdjust=-1000`)는 이 범위 안에서도 죽이지 않을 서비스를 지정하는 별개의 장치다.
