# Thrash Guard 프로젝트 계획

> 상태: 계획 단계. 이 문서의 수치 목표는 모두 **측정 전 가설**이며, 아직 구현이나 실험은 하지 않았다.
> 작업 단위, 완료 조건, 결정 사항은 [roadmap.md](roadmap.md)에서 관리한다.

Thrash Guard는 리눅스 PSI(Pressure Stall Information)로 메모리 스래싱을 감지해, 시스템이
응답 불능에 빠지기 전에 원인 cgroup을 감속하거나 종료하는 유저스페이스 데몬이다. 데몬 구현과
함께, 기존 수단(systemd-oomd, MGLRU `min_ttl_ms`)과 같은 조건에서 비교하는 재현 실험을 만든다.

## 1. 배경

팀 캡스톤 프로젝트 "길벗"의 운영 EC2(t3.micro, 가용 메모리 913 MiB, Ubuntu 22.04,
커널 `6.8.0-1060-aws`)가 2026-07-15~16에 약 27시간 응답 불능 상태에 빠졌다.

- 가용 메모리가 0~11 MiB 사이를 오갔지만 커널 OOM killer는 한 번도 발동하지 않았다.
- SSM Agent, CloudWatch Agent, rsyslog 같은 유저스페이스 데몬이 먼저 멈췄고, 수동 재부팅으로 끝났다.
- 분석 결론은 리클레임 스래싱 livelock이다. 다만 `pgscan`/`pgsteal`/PSI를 확보하지 못해, 메커니즘 자체는 **간접 추론**으로 남아 있다.

1차 대응은 swap 2 GiB, `vm.swappiness=10`, 컨테이너 `mem_limit`이었다.

- [EC2 응답 불능 문제 분석](https://github.com/kookmin-sw/2026-capstone-26/blob/930b40aa2a42dd0923ee06d277151f0f9ddf826c/backend/docs/tech/infra/oom-repro-evidence/production/EC2%20응답%20불능%20문제%20분석.md)
- [EC2 응답 불능 문제 해결하기](https://github.com/kookmin-sw/2026-capstone-26/blob/930b40aa2a42dd0923ee06d277151f0f9ddf826c/backend/docs/tech/infra/EC2%20응답%20불능%20문제%20해결하기.md)

### 1차 대응 후에도 남은 문제

| 문제 | 내용 |
|---|---|
| 컨테이너 내부 스래싱 | `memswap_limit`이 없어 앱은 `mem_limit`(590m)만큼 swap을 더 쓸 수 있다. "cgroup OOM으로 깔끔히 재기동"의 근거 실험은 swap 0 조건이었다. |
| `swappiness=10`의 부작용 가능성 | 파일 캐시를 우선 회수하므로, swap을 일상적으로 쓰는 설계에서 실행 코드 페이지 재적재를 늘릴 수 있다. |
| I/O 비격리 | 메모리는 cgroup으로 나눴지만 swap과 시스템 코드 재적재는 같은 디스크를 쓴다. |
| 판단 주체 부재 | 커널의 기본 OOM 판단은 "회수 진척 여부"만 본다. PSI를 보고 행동하는 프로그램이 없었다. |

## 2. 목표와 비목표

**목표는 스래싱 "제거"가 아니라 "지속 시간과 피해 범위의 제한"이다.** 작업집합이 RAM보다 크면
어떤 설정으로도 스래싱 자체를 없앨 수 없다.

### 목표

- 스래싱을 직접 측정해, 길벗 사건의 간접 추론을 재현 환경에서 검증한다.
- 스래싱 시작부터 복구까지의 시간을 수십 초 수준으로 제한한다.
- 스래싱 중에도 호스트 접속 경로(SSH)를 유지한다.
- 정상적인 메모리 압력(JVM 기동 등)에서는 프로세스를 죽이지 않는다.
- 기존 수단 대비 장단점을 재현 가능한 수치로 제시한다.

### 비목표

- 커널 수정. 기존 수단을 측정해 한계가 입증된 뒤의 선택 과제로만 둔다.
- 길벗 운영 서버 적용. 해당 인프라는 이미 철거됐으므로 운영 효과를 주장하지 않는다.
- Kubernetes 등 다중 노드 오케스트레이션 지원.
- 범용 메모리 관리 대체. 단일 호스트의 컨테이너 워크로드에 집중한다.

### 초기 성공 기준

기준선 측정(M2) 후 재확정한다.

| 항목 | 초기 목표 |
|---|---|
| 복구 시간(스래싱 시작 ~ 외부 프로브 정상화) | 60초 이내 |
| 정상 압력 시나리오의 오탐 kill | 전 반복 0회 |
| 스트레스 중 감지 후 kill까지의 지연 | 측정 후 결정 |
| 유휴 시 메모리 사용량 | 2 MiB 이하 |
| 유휴 시 CPU 사용량 | 측정 오차 수준 |

길벗의 메모리 예산 여유는 3 MiB에 불과했다. 메모리를 고정(`mlockall`)하는 데몬은 그 안에서
동작할 만큼 작아야 한다.

## 3. 검증할 가설

- **H1. 재현성**: 익명 메모리와 파일 기반 작업집합의 합이 RAM을 넘으면, OOM kill 없이 장시간
  응답 불능이 재현된다.
- **H2. 메커니즘**: 그 상태에서 `pgsteal/pgscan` 효율이 떨어지고 refault와 PSI full이 치솟는다.
- **H3. 현행 구성의 취약성**: swap과 `mem_limit`이 있어도 컨테이너 내부 스래싱은 깔끔한 kill 없이
  오래 지속될 수 있다.
- **H4. 기존 수단의 유효성**: systemd-oomd와 MGLRU `min_ttl_ms`는 livelock을 끊지만, 복구 시간이나
  오탐에서 한계가 있다. 특히 `min_ttl_ms`는 전역 회수 경로에서만 동작해, 컨테이너가 자기 메모리 상한에
  걸린 내부 스래싱(S2)은 막지 못할 수 있다.
- **H5. 차별화 효과**: 이벤트 기반 감지, 복합 판단, 자기 보호는 복구 시간과 오탐을 개선한다.

H4와 H5는 반증될 수 있다. systemd-oomd가 더 낫거나 차이가 없다면 그 결과를 그대로 기록한다.
기준선 측정(M2) 직후 기존 수단이 무엇을 막고 무엇을 놓치는지 보고, 데몬이 집중할 빈틈을 다시 정한다.
데몬 로직은 커널 안전망(`min_ttl_ms`) 없이도 동작하도록 만들고, 함께 쓸 때의 효과는 C6로 잰다.

## 4. 데몬 설계

### 4.1 동작 흐름

```text
[초기화] 설정 로드 -> mlockall -> 버퍼 사전 할당 -> PSI 트리거 등록
    |
[대기] poll(POLLPRI)로 잠듦
    |  PSI 트리거 발화
[판단] 일정 구간 동안 PSI + refault + pgsteal/pgscan 샘플링
    |
    +-- 일시적 압력 -> 대기로 복귀
    +-- 스래싱 지속 -> [감속] memory.high 낮추기 (2단계 구현)
                         |  개선 없음
                         v
                      [종료] 스냅샷 보관 -> cgroup.kill -> 로그 기록 -> 쿨다운
```

### 4.2 판단 입력

| 입력 | 출처 | 용도 |
|---|---|---|
| 시스템 PSI | `/proc/pressure/memory` | 이벤트 기반 깨우기 |
| cgroup PSI | 각 cgroup의 `memory.pressure` | 대상별 압력 비교 |
| 재적재율 | `workingset_refault_anon`, `workingset_refault_file` | 무의미한 회수 판별 |
| 회수 효율 | `pgscan_*`, `pgsteal_*` | 스캔 대비 실제 회수 |
| swap I/O | `pswpin`, `pswpout` | swap 스래싱 판별 |

PSI 트리거의 초깃값은 `full 150000 1000000`(1초 중 150 ms)으로 시작하고, 정상 부하 측정으로
보정한다.

### 4.3 대상 선정과 안전장치

- 설정에 명시한 부모 cgroup의 하위만 대상으로 삼고, `system.slice`의 시스템 서비스는 제외한다.
- 후보 중 압력과 회수 활동이 가장 큰 cgroup을 고른다.
- 컨테이너 시작 직후의 유예 시간, 판단 후 쿨다운, 시간당 kill 횟수 상한을 둔다.
- **dry-run 모드**를 기본 제공한다. 판단만 기록하고 kill은 하지 않아, 오탐 검증에 쓴다.

### 4.4 자기 보호 요구사항

길벗 사건에서 먼저 멈춘 것은 관측 에이전트들이었다. 이 데몬은 같은 상황에서도 동작해야 한다.

| 요구사항 | 방법 |
|---|---|
| 페이지 회수 대상에서 제외 | `mlockall(MCL_CURRENT \| MCL_FUTURE)`, 스택 사전 접근 |
| 라이브러리 재적재 방지 | 정적 링크 |
| 핵심 경로의 할당 금지 | 초기화 때 버퍼 할당, 이후 `malloc` 금지 |
| OOM 대상 제외 | `OOMScoreAdjust=-1000` |
| dockerd 비의존 | Docker API 대신 `cgroup.kill`에 직접 쓰기 |
| 디스크 지연 격리 | 로그는 고정 메모리 버퍼에 쌓고 별도 스레드가 기록 |
| 감속과 종료 분리 | `memory.high` 쓰기는 회수를 동반할 수 있어 kill 경로와 다른 스레드에서 수행 |

구현 시 측정으로 확인할 위험은 다음과 같다.

- `/proc` 파일 읽기는 커널 내부 할당을 일으킬 수 있어 완전한 무할당을 보장하지 못한다.
- `SCHED_FIFO` 등 실시간 스케줄링은 효과와 시스템 영향을 측정한 뒤 적용 여부를 정한다.
- 컨테이너가 재시작되면 cgroup 경로가 바뀌므로, 대상 탐색은 핵심 경로 밖에서 미리 수행한다.

### 4.5 기술 선택

- 언어는 **C**로 한다. 핵심 경로가 시스템 콜 몇 개이고, 런타임과 GC가 없어 무할당을 검증하기 쉽다.
- 대상 환경은 cgroup v2, PSI 활성, `cgroup.kill` 지원(리눅스 5.14 이상)이다.
- 저장소, 실행 파일, 서비스 이름은 `thrash-guard`로 통일한다.

## 5. 재현 환경

### 5.1 합성 부하

길벗 앱에 의존하지 않도록 `workload/`에 재현 도구를 만든다.

- 익명 메모리를 천천히 늘려 RAM에 근접시키고 계속 접근한다.
- mmap한 파일을 무작위로 읽어 "곧 다시 필요한 파일 페이지"를 만든다.
- 처리량과 지연을 기록하는 카나리 작업을 함께 돌린다.

**재현 합격 기준**: `MemAvailable` 20 MiB 이하, OOM kill 0건, SSH 무응답 10분 이상을 동시에
만족해야 한다. 단순 부하는 OOM kill로 끝나기 쉬워 이 단계가 가장 큰 위험이다.

### 5.2 시나리오

| ID | 시나리오 | 목적 |
|---|---|---|
| S1 | swap 없음, 컨테이너 제한 없음 | 길벗 사건 재현 |
| S2 | swap 2 GiB, 메모리 상한 있음 | 길벗 현행 구성의 취약성 확인 |
| S3 | JVM 기동 버스트, 대량 파일 읽기 | 정상 압력 오탐 측정 |
| S4 (선택) | 실제 Spring Boot 컨테이너 | 합성 부하의 현실성 검증 |

### 5.3 비교 구성

| ID | 구성 |
|---|---|
| C0 | 방어 없음 |
| C1 | swap 2 GiB + `swappiness=10` + `mem_limit` (길벗 현행) |
| C2 | C1 + systemd-oomd (기본 설정) |
| C2t | C1 + systemd-oomd (지속 시간·임계를 데몬과 비슷하게 튜닝) |
| C3 | C1 + MGLRU `min_ttl_ms=1000` |
| C4 | C1 + Thrash Guard MVP (감지 + kill) |
| C5 | C1 + Thrash Guard 전체 기능 |
| C6 | C4 + MGLRU `min_ttl_ms=1000` (커널 안전망과 함께 쓸 때의 효과) |

여력이 있으면 C1에서 `swappiness` 10/60/100, zswap, `watermark_scale_factor`를 변수로 추가한다.

### 5.4 측정 항목

| 분류 | 항목 |
|---|---|
| 복구 | 감지 시간, 복구 시간, 미복구 여부 |
| 커널 | PSI some/full, `pgsteal/pgscan`, refault, swap in/out |
| 외부 관측 | 대상 밖에서 측정한 SSH 접속 지연, 카나리 요청 성공률, EBS 읽기/쓰기(EC2) |
| 부작용 | 오탐 kill 수, 데몬 메모리·CPU, 스트레스 중 데몬 루프 지연 |

**측정 규칙**

- 복구는 외부 프로브가 연속 N회 정해진 시간 안에 성공한 시점으로 정의한다.
- 구성·시나리오별 5회 이상 반복하고 중앙값과 범위를 보고한다.
- 실행당 30분 안에 복구되지 않으면 "미복구"로 기록한다.
- 측정자는 반드시 대상 호스트 밖에 둔다.
- 원본 데이터는 `results/`에 커밋해 재검증할 수 있게 한다.
- 표본 부족 등 해석의 한계를 결과와 함께 적고, 실측과 추론을 구분한다.

## 6. 작업 환경

| 단계 | 환경 | 용도 | 비고 |
|---|---|---|---|
| 개발 | WSL Ubuntu | 빌드, 단위 테스트, 가벼운 PSI 확인 | PSI는 확인, MGLRU는 없음 |
| 통합 | 메모리 1 GiB Linux VM | 스래싱 재현, 반복 실험 | Multipass 설치 필요 |
| 최종 측정 | 일회용 t3.micro | 공식 수치 측정 | 실험 후 즉시 destroy |

- 스래싱 실험은 WSL에서 하지 않는다. Docker Desktop과 한 커널을 공유해 개발 환경까지 멈춘다.
- Ubuntu 22.04 VM은 HWE 커널로 올려 6.8 계열에 맞추고 MGLRU 지원 여부를 확인한다.
- t3.micro의 AMI는 길벗과 같은 22.04로 하고, 실행 전에 커널 설정을 확인한다.
- `grep LRU_GEN /boot/config-$(uname -r)`, `/sys/kernel/mm/lru_gen/enabled`로 MGLRU를 확인한다.
- t3의 CPU 크레딧 모드와 EBS 설정은 모든 실행에서 고정하고 기록한다.
- Terraform 상태는 길벗과 분리하고, AWS 작업 전에 사용할 프로파일을 확인한다.
- 스래싱으로 VM이 멈출 수 있으므로 시작 전 스냅샷을 만든다.
- 저장소가 Windows 경로에 있으므로 셸 스크립트 줄바꿈이 깨지지 않게 `.gitattributes`로 LF를 강제한다.

## 7. 저장소 구조

```text
thrash-guard/
├── src/                    # C 데몬
├── packaging/              # systemd 서비스 파일
├── workload/               # 스래싱 재현 합성 부하
├── lab/                    # 실험 실행, 수집 스크립트, 구성별 프로파일
├── infra/                  # t3.micro Terraform
├── results/                # 원본 측정 데이터
├── docs/                   # 계획, 설계 결정, 실험 보고서
└── .github/workflows/      # 빌드, 경고, 단위 테스트
```

CI는 빌드, 정적 분석, 파서 단위 테스트까지 수행한다. 스래싱 실험은 VM과 EC2에서만 수행한다.

## 8. 마일스톤

| 단계 | 내용 | 완료 조건 |
|---|---|---|
| M0 | 저장소, VM, 수집 도구 준비 | VM에서 PSI·vmstat를 호스트 밖으로 수집 |
| M1 | 재현 | S1이 합격 기준을 충족하고 H1·H2 판정 |
| M2 | 기준선 측정 | C0~C3 결과 확보, 성공 기준 재확정 |
| M3 | 데몬 MVP | dry-run, 트리거 감지, 자기 보호, kill 동작 |
| M4 | MVP 평가 | C4를 VM에서 측정, 오탐 확인 |
| M5 | 차별화 기능 | 복합 판단, 단계적 대응, 스냅샷을 기능별로 평가 |
| M6 | 최종 측정과 공개 | t3.micro에서 전 구성 측정, README·보고서 작성 |
| M7 (선택) | 심화 | eBPF 직접 회수 지연 측정, QEMU 커널 패치 실험 |

MVP를 측정하기 전에는 M5 기능을 구현하지 않는다.

## 9. 위험과 대응

| 위험 | 대응 |
|---|---|
| livelock이 재현되지 않고 OOM kill로 끝남 | 익명·파일 비율과 증가 속도를 체계적으로 탐색, 재현 범위를 문서화 |
| 합성 부하와 실제 앱의 차이 | S4로 교차 확인 |
| 데몬이 oomd보다 낫지 않음 | 결과를 그대로 공개하고, 어떤 조건에서 어느 쪽이 나은지 정리 |
| 임계값 과적합 | 보정용과 평가용 시나리오를 분리 |
| VM과 EC2의 결과 차이 | 공식 수치는 t3.micro로 한정 |
| AWS 비용 발생 | 실험 시간에만 생성, 종료 후 destroy 확인 |
| 범위 확대 | 마일스톤 순서를 지키고 튜닝 변수는 선택 과제로 둠 |

## 10. 포트폴리오 서술 원칙

- 길벗과는 별도 프로젝트로 두고, 길벗 장애 분석에서 출발했다는 맥락을 링크로 잇는다.
- 성과는 "길벗 장애를 재현한 환경에서의 결과"로 표현하고, 운영 적용 효과처럼 쓰지 않는다.
- 측정 전에는 수치를 쓰지 않는다.
- 길벗 자료를 옮길 때는 호스트명·인스턴스 ID를 제거한다. 팀 프로젝트 자료이므로 원문은 링크를 우선한다.

## 11. 참고 자료

- [PSI](https://docs.kernel.org/accounting/psi.html)
- [Multi-Gen LRU](https://docs.kernel.org/admin-guide/mm/multigen_lru.html)
- [Control Group v2](https://docs.kernel.org/admin-guide/cgroup-v2.html)
- [systemd-oomd.service](https://www.freedesktop.org/software/systemd/man/latest/systemd-oomd.service.html)
- [oomd.conf](https://www.freedesktop.org/software/systemd/man/latest/oomd.conf.html)
- [Docker 리소스 제약](https://docs.docker.com/engine/containers/resource_constraints/)
