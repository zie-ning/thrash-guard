# 실험 환경과 지금까지의 실측 기록

> 개발·검증 환경, 실험용 VM을 고른 경과, M0에서 확인한 사실, 이후에 잰 값을 모아 둔다.
> 데몬 설계는 [design/overview.md](../design/overview.md), 검증 방법은 [validation/overview.md](../validation/overview.md)에 있다.
> 실측과 추론을 구분해 적고, 측정하지 않은 값은 쓰지 않는다.

## 0. 작업 환경

| 단계 | 환경 | 용도 | 비고 |
|---|---|---|---|
| 개발 | WSL Ubuntu | 빌드, 단위 테스트, 가벼운 PSI 확인 | PSI는 확인, MGLRU는 없음 |
| 검증 | 메모리 1 GiB Linux VM (Windows QEMU + WHPX) | 스래싱 재현, 효과 검증 | VM 밖 측정자 = WSL |

- 스래싱 실험은 WSL에서 하지 않는다. Docker Desktop과 한 커널을 공유해 개발 환경까지 멈춘다.
- Ubuntu 22.04 VM은 HWE 커널로 올려 6.8 계열에 맞춘다. 실행 메타데이터에 커널 설정, MGLRU 상태를 기록한다.
- 스래싱으로 VM이 멈출 수 있으므로 매 실행을 기준 스냅샷에서 시작한다.
- 저장소가 Windows 경로에 있으므로 셸 스크립트 줄바꿈이 깨지지 않게 `.gitattributes`로 LF를 강제한다.
- 빌드와 테스트는 WSL에서 `make`, `make test`로 실행한다. 스레드를 쓰는 모듈은 `make test-tsan`으로 데이터 경쟁도 검사한다.

## 1. 개발 환경 (2026-11-10 확인)

| 항목 | 상태 |
|---|---|
| WSL | Ubuntu 24.04.3, 커널 `6.18.33.2-microsoft-standard-WSL2`, 메모리 6 GiB, 4코어 |
| 툴체인 | gcc 13.3, make, python 3.12. clang·cppcheck 없음 |
| PSI / cgroup v2 | `/proc/pressure/memory` 동작, memory 컨트롤러 사용 가능 |
| MGLRU | 없음(`/sys/kernel/mm/lru_gen` 부재) |
| 가상화 | WSL 안 `/dev/kvm` 없음(중첩 가상화 설정 후에도 동일). Windows용 QEMU 11.1.0 + WHPX로 Ubuntu 22.04(6.8.0-138-generic) VM 부팅 확인 |
| 네트워크 | WSL `networkingMode=mirrored`. WSL에서 `127.0.0.1`로 Windows QEMU의 QMP 포트 접속 확인 |
| 기타 | Terraform, AWS CLI 미설치. git remote 없음, 커밋 없음 |

## 2. 실험 VM 플랫폼 선택

| | A. WSL 중첩 가상화 + QEMU/KVM | B. Multipass (Hyper-V) | C. Windows QEMU + WHPX |
|---|---|---|---|
| 준비 | `.wslconfig`에 `nestedVirtualization=true` 후 `wsl --shutdown`. CPU 지원 여부 확인 필요 | Multipass 설치 | QEMU for Windows 설치, Windows 하이퍼바이저 플랫폼 기능 활성화 |
| 디스크 I/O 상한 | QEMU `throttle-group`으로 스크립트에서 바로 설정 | 관리자 PowerShell `Set-VMHardDiskDrive -MaximumIOPS` | A와 같음(`throttle-group`) |
| 멈춘 VM 강제 리셋 | QMP `system_reset`, 스크립트로 자동화 쉬움 | `multipass restart`가 멈춘 VM에서 실패할 수 있어 Hyper-V 명령 필요 | A와 같음(QMP를 TCP로 열어 WSL에서 호출) |
| 스냅샷 | qcow2 백킹 파일로 매 실행마다 깨끗한 디스크 | Multipass 스냅샷(VM 정지 필요) | A와 같음 |
| 측정자 위치 | WSL(VM 밖)에서 bash로 전부 처리 | WSL↔Hyper-V 기본 스위치 간 네트워크 도달성이 환경마다 다름 | WSL. VM 포트는 Windows localhost로 전달되며 WSL에서 Windows 호스트로 접속한다 |
| 단점 | Docker Desktop도 WSL 재시작 영향을 받음. 중첩 가상화 성능 손실 | 자동화 경로가 Windows와 WSL로 나뉨 | WHPX 가속은 KVM보다 덜 쓰이는 경로라 안정성 확인 필요. 프로세스 관리가 Windows 쪽 |

**경과**: 처음에는 A를 골랐다. 그러나 `.wslconfig`에 `nestedVirtualization=true`를 넣고 재시작해도 WSL 커널에 `svm` 플래그와 `/dev/kvm`이 나타나지 않았다(Ryzen 5 7530U, Windows 11 26200, WSL 2.7.1). WSL은 "지원 안 함" 경고를 띄우지 않았고, Hyper-V Compute 로그에는 VM 생성 설정이 남지 않아 원인을 특정하지 못했다. Windows 보안 설정을 근거 없이 바꾸는 대신 C로 결정했다. C는 A의 자동화 장점(I/O 상한, QMP 리셋, qcow2 복원)을 유지한다. C도 막히면 B로 간다. 어느 쪽이든 VM 밖 측정자 = WSL이다.

## 3. M0 결과: 저장소, 공용 모듈, 수집 경로

> **핵심**: 실험 전에 "블랙박스"와 "실험실"을 갖춘다. 컴퓨터가 멈춰 가는 동안에도 메모리 상태를 기록해 VM 밖으로 보내는 수집기를 만들고, 길벗 서버와 비슷한 1 GiB 실험용 VM을 준비한다. 여기서 만드는 메모리 상태 읽기·자기 보호 코드는 데몬이 그대로 재사용한다.

| ID | 작업 | 산출물 |
|---|---|---|
| M0-1 | 저장소 골격: `.gitattributes`(LF 강제), `.editorconfig`, `.gitignore`, `Makefile`, README 초안 | 빈 빌드 통과 |
| M0-2 | procfs 파서: `pressure`(some/full의 avg·total), `vmstat` 필요 필드, `meminfo`, cgroup `memory.stat`·`memory.pressure`. 호출자 버퍼만 쓰고 할당하지 않는다 | `src/common/procfs.[ch]`, fixture 기반 단위 테스트 |
| M0-3 | 자기 보호 모듈: `mlockall`, 스택 사전 접근, 스레드 스택 크기 고정, `M_ARENA_MAX=1`, `oom_score_adj` | `src/common/selfprot.[ch]` |
| M0-4 | `tg-sampler`: 250 ms 주기로 PSI·vmstat·meminfo를 읽어 UDP로 전송. 소켓·버퍼는 초기화 때 준비 | 정적 바이너리 |
| M0-5 | CI(GitHub Actions): gcc·clang `-Werror` 빌드, cppcheck, ASan/UBSan 단위 테스트 | `.github/workflows/ci.yml` |
| M0-6 | VM 구축: Ubuntu 22.04 클라우드 이미지 → HWE 6.8 커널, 메모리 1 GiB, 2 vCPU, 디스크 I/O 상한, Docker, `workload.slice`, 기준 스냅샷 | `lab/vm/` |
| M0-7 | 수신기·프로브: UDP → CSV, SSH 프로브, 실행 메타데이터 수집(커널, config, `lru_gen`, swappiness, I/O 상한, git SHA) | `lab/collect/` |

**완료 조건**: VM에서 `stress-ng --vm`으로 압력을 줬을 때, WSL 수신기의 CSV에 PSI 상승과 SSH 프로브 지연이 같은 시간축으로 기록된다.

**진행 (2026-11-10): 완료**

| ID | 상태 | 비고 |
|---|---|---|
| M0-1 | 완료 | |
| M0-2 | 완료 | WSL 6.18과 VM 6.8 fixture 모두에서 모든 필드를 읽음. 단위 테스트 71개 통과 |
| M0-3 | 완료 | VM에서 root로 `VmLck` 약 1 MB, `oom_score_adj=-1000` 적용 확인. 스레드 스택 크기 고정은 데몬(M3)에서 적용 |
| M0-4 | 완료 | VM → 10.0.2.2 → Windows → WSL(mirrored) UDP 경로로 250 ms 주기 약 380샘플, 누락 0 |
| M0-5 | 작성 | 원격 저장소가 없어 아직 실행하지 않음. 로컬에 clang·cppcheck가 없어 해당 단계는 미검증 |
| M0-6 | 완료 | `lab/vm/`: 기준 이미지(`build-base.sh`), 일회용 실행(`reset-run.sh`), 종료(`stop.sh`), 패치(`patch-base.sh`) |
| M0-7 | 완료 | 수신기, SSH 프로브, 메타데이터(`run_meta.sh`), CSV 변환. 점검 스크립트 `lab/m0_check.sh` |

**완료 조건 확인** (`results/m0-check/20261110-132424`, I/O 상한 3000 IOPS / 125 MiB/s)

| 구간 | SSH 프로브 평균 / 최대 | PSI |
|---|---|---|
| 기준 15초 | 205 / 219 ms | stall 없음 |
| stress-ng 60초 | 310 / 1120 ms | 부하 시작 직후 250 ms 구간 최대 some 4.3%, full 3.1% |
| 회복 20초 | 218 / 266 ms | stall 없음 |

압력 상승과 SSH 지연 증가가 같은 시간축에 기록됨을 확인했다. 다만 압력의 크기는 작다(아래 3번).

**M0에서 알게 된 사실**

1. **MGLRU가 기본으로 켜져 있다.** 6.8 HWE 커널에서 `lru_gen/enabled=0x0007`, `min_ttl_ms=0`. 따라서 C0은 "MGLRU on, 보호 시간 0"이다. 길벗 AWS 커널의 기본값은 M6에서 확인한다.
2. **systemd-oomd는 기본 설치되어 있지 않다.** 22.04에서는 별도 패키지다. C2 측정(M2) 때 설치한다.
3. **stress-ng는 스래싱 재현에 부적합하다.** `--vm-bytes`를 퍼센트로 주든 절대값(380M × 2)으로 주든 익명 메모리를 약 430 MiB까지만 잡았고, 남은 메모리가 140~190 MiB로 유지되었다. 가용 메모리를 넘지 않도록 스스로 줄이는 것으로 보인다(미확인). M1의 재현 부하는 계획대로 직접 만든 `tg-load`로 한다.
4. **WHPX에서 게스트 재부팅이 실패한다.** 재부팅 순간 `WHPX: Unexpected VP exit code 4`로 VM이 일시정지된다. QEMU에 `-no-reboot`을 주고, 재시작은 "종료 후 새로 켜기"로 처리한다. QMP `system_reset`도 같은 문제를 겪을 수 있어 멈춘 VM은 강제 종료 후 새로 켠다.
5. **PSI avg10은 짧은 stall을 놓친다.** 위 실행에서 avg10 최대는 0.00%였지만 누적값 차분으로 계산한 250 ms 구간 stall은 4.3%였다. 분석과 데몬 판단은 `total` 차분을 기본으로 쓴다.
6. **SSH 새 연결 지연의 기준값은 약 200 ms다**(VM이 한가할 때). 복구 판정의 "1초 안에 성공" 기준은 이 값과 충분히 떨어져 있다.

위 1~2번에서 말한 C0, C2는 이전 계획의 비교 구성 이름이다. 기존 도구와의 비교는 후속 과제(roadmap M7)로 미뤘다.

## 4. PSI 트리거 실측 (M3 작업 중)

**PSI 트리거 실측** (WSL 6.18, root): 윈도 상한 10초, 400 ms 윈도도 등록됨, stall이 윈도보다 크면 거부. **일반 사용자는 1초 윈도 등록이 거부되고 10초는 허용**되므로 데몬은 root(또는 `CAP_SYS_RESOURCE`)로 돌려야 한다(원인은 미확인). VM 6.8 커널에서의 경계는 M3-2에서 확인한다.

## 5. 참고: M1 재현 판정 기록 (2026-11-10~11)

**참고용 기록이다.** 이전 계획(길벗 재현 중심)에서 만든 도구와 측정이며, 현재의 설계([design/overview.md](../design/overview.md))와 검증 설계([validation/overview.md](../validation/overview.md))는 이 결과에 의존하지 않는다.
원본 데이터는 `results/m1/`(`20261110`의 stage1~5, `20261111`의 stage6~12)에 있다. 아래 수치는 모두 이 VM(1 GiB, swap 없음) 기준의 실측이다.

### 5.1 구성 요소

| 구성 | 내용 |
|---|---|
| `workload/tg_load.c` (`tg-load`) | 합성 부하. 모드: `anon`(익명 메모리를 목표 크기까지 키우며 전체 무작위 접근), `file`(mmap 파일 무작위 읽기), `pin`(`mlock`으로 회수 불가 메모리 고정), `canary`(TCP 응답 서버, 메모리 보호 없음) |
| `lab/run.sh` | 실행 하네스. 기준 시나리오 S1과 컨테이너 모사 시나리오 S2(`--scenario S2`). 판정이 확정되면 즉시 종료하고 QMP로 VM을 강제 종료한다 |
| `lab/sweep.sh` | 조건 탐색. 조건 형식은 `anon,file,rate[,pin]` |
| `lab/analyze/` | `verdict.py`(판정), `summary.py`, `s2_summary.py` |
| `tg-sampler --cg-parent` | 부모 cgroup의 직계 하위 cgroup(최대 8개)을 `kind=cg` 줄로 함께 보낸다. 부모 `cgroup.stat`의 `nr_descendants`가 바뀌거나 읽기에 실패하면 다시 찾는다. VM에서 서비스 하나를 내렸다 새로 띄웠을 때 목록이 교체됨을 확인했다. Docker 컨테이너(`docker-<id>.scope`)로는 이미지가 없어 확인하지 못했다 |

### 5.2 재현 판정

판정은 세 갈래다: **oom_kill**, **livelock**, **normal**.

- **livelock**: OOM kill 0건, SSH 성공이 한 번도 없는 구간이 10분 이상, 그 구간의 `MemAvailable`이 20 MiB 이하. 경계 조건은 normal로 분류하고 수치는 `verdict.json`에 남긴다.
- **조기 종료**: anon이 목표 크기까지 다 자란 뒤 5분 동안 SSH 프로브가 한 번도 실패하지 않고 `MemAvailable`이 20 MiB를 넘는 상태가 유지되면 normal로 확정하고 끝낸다(`early_normal=true`로 표시). 하한은 처음 100 MiB였으나 550~650 MiB 실행이 조기 종료되지 못해 20 MiB로 낮췄다. 임계값은 임시값이며 5분 뒤에 늦게 시작되는 스래싱을 놓칠 수 있다(미확인).

### 5.3 탐색과 결과

- 단계적으로 탐색했다(anon 크기, file 크기, 증가 속도를 한 축씩). 탐색 지점은 1회씩 돌리고 후보에서만 5회 반복했다. 한 번의 부하는 최대 20분이다.
- **같은 조건에서도 livelock과 OOM kill이 갈린다.** 예: 150 IOPS, anon 650 MiB 5회에서 livelock 3회, OOM kill 2회. 그래서 "5회 중 4회" 같은 합격선 대신 재현율을 N회 중 k회와 신뢰 범위로 보고한다.
- **기준 조건(평가용 유도 조건)**: anon 650 MiB(4 MiB/s), file 0, mlock 43 MiB, I/O 상한 100 IOPS. **15회 중 9회 livelock(60%, 신뢰구간 약 33~83%).** 목표였던 80%에는 닿지 못했다.
- 시도한 레버(I/O 상한 150에서 100, anon 650/700 MiB, mlock 43 MiB, file 50 MiB) 중 재현율을 구분 가능하게 올린 것은 없었다. **3000 IOPS에서는 약 20회 모두 livelock이 나오지 않아**, I/O 상한을 낮추는 것만 필수로 확인됐다.
- 이 조건은 길벗 장애를 **IOPS 상한으로 모사한 유도 조건**이다(아래 5.6절: 실제 장애는 IOPS가 아니라 EBS 처리량 상한이었다고 정정됨). mlock 43 MiB는 길벗의 unevictable 관측값을 모사한 것이다. 가장 높은 값은 650 MiB·150 IOPS의 4/6(67%)였지만 표본이 작아 구분되지 않아 표본이 큰 조건을 택했다.
- 같은 조건에서 livelock과 OOM kill이 갈리는 이유는 **미확인**이다. 가설은 회수 효율이 낮을수록 OOM kill로 간다는 것이지만, 일부 실행은 이 가설로 설명되지 않는다.

### 5.4 컨테이너 내부 스래싱 예비 확인 (S2, 현재 범위 밖)

- 컨테이너는 Docker 대신 `workload.slice` 아래 `workload-container.slice`에 `MemoryMax`와 `MemorySwapMax`를 걸어 모사했다. swap 2 GiB, `swappiness=10`, VM 1 GiB, 컨테이너 상한 600 MiB, 부하 anon 800 MiB, 100 IOPS. 커널의 `min_ttl_ms`를 0과 1000으로 각각 3회 번갈아 측정했다(`results/m1/20261111/stage12-s2-minttl/`, 1000초 상한).
- 6회 모두 `normal`이었다. 컨테이너 cgroup의 PSI `full`이 53~77%로 상한에 계속 걸렸지만 카나리 무응답이 10분에 미치지 못했고(최장 111초) OOM kill도 없었다.
- 카나리 최장 무응답은 `min_ttl_ms=1000`에서 3~19초, 0에서 16~111초로 낮았지만 3회씩이라 범위가 겹쳐 결론을 내지 못한다.
- 컨테이너 스래싱이 SSH 지연(최대 51초)과 시스템 `MemAvailable` 하락으로 번졌다. 원인은 미확인이다(가설: swap I/O가 같은 디스크를 공유).
- 새 목적에서는 컨테이너 내부 스래싱을 범위 밖으로 두었으므로, 후속 확장 때 참고한다.

### 5.5 남은 한계

- 하네스가 복구(해소) 시간을 재지 않는다. `pin`의 `VmLck` 값이 샘플에 기록되지 않는다.
- anon 접근 패턴은 전체 무작위만 구현했다. 스래싱이 가장 잘 생기는 최악 조건이며 길벗의 실제 패턴은 알 수 없다(미확인).
- 초기 측정은 부하를 SSH로 직접 실행해 `workload.slice` 밖에서 돌았다(`cg_n=0`). 새 검증에서는 부하를 각각 별도 cgroup으로 실행한다.
- livelock 판정 경로는 스모크 실행과 합성 데이터 단위 테스트로 먼저 확인했고, 이후 반복 측정에서 실제 livelock이 관측됐다.

### 5.6 정정: 길벗 장애의 디스크 병목은 IOPS가 아니라 처리량이었다 (2026-11-13)

**출처와 신뢰도**: 다른 작업 세션("리클레임 스래싱 개선 방안")이 길벗 계정의 CloudWatch(AWS/EC2, 1시간 집계)를 조회해 전달한 내용이다. 이 저장소의 작업자가 직접 확인하지는 않았다.

- 기존 해석("읽기 143~147 IOPS, 요청당 약 6.9 ms는 gp3 지연")은 틀렸다고 전달받았다. t3.micro 인스턴스의 **EBS 처리량 크레딧**(`EBSByteBalance%`)이 장애 시작 직후(07-15 01시) 0%로 소진되었고 이후 계속 0%였다. 읽기는 인스턴스의 기준 대역폭 **약 10.9 MB/s(87 Mbps)** 에 묶였다(`EBSReadBytes` 시간당 약 39.13 GB로 거의 일정).
- 평균 읽기 크기는 약 76 KiB(추정: readahead). IOPS 크레딧(`EBSIOBalance%`)은 최저 68%로 소진되지 않았고, `CPUCreditBalance`도 0까지 가지 않았다(최저 약 15). 따라서 병목은 IOPS도 CPU도 아니었다.
- 이 저장소의 VM 실험은 지금까지 **IOPS 100**(대역폭은 125 MiB/s)으로 디스크를 막았다. 이 조건에서 스래싱이 나기는 했지만 장애의 실제 병목(처리량)과는 다른 축이다. 100 IOPS에서 읽기 크기가 76 KiB라면 약 7.6 MB/s로 우연히 비슷하지만, 작은 읽기가 많으면 훨씬 느려진다.
- 영향: M3 VM 검증(`results/m3/20261113`)의 정지 시간과 io full 수치는 이 유도 조건에서 나온 것이라, 실제 장애 조건의 값으로 읽지 않는다. 처리량 상한(약 10.9 MB/s)으로 다시 확인한다(M4 선행 실험).

### 5.7 처리량 제한 조건 재확인 (2026-11-13)

QEMU 디스크를 처리량 약 10.9 MB/s(`throttling.bps-total=10,900,000`), IOPS 3000으로 제한하고 M1 기준 부하(단일 hog 650 MiB, 초당 4 MiB, `mlock` 43 MiB)를 걸었다. 데몬 dry-run 3회와 실제 종료 3회다. 원본은 `results/m3/20261113/G_throughput_*`.

| | dry-run 3회 | 실제 종료 3회 |
|---|---|---|
| 가용 메모리 30 MiB 미만 지속 | 376, 309, 302초 (hog의 부하 시간 420초가 끝나 마침, 더 길어질 수 있음) | 21, 13, 21초 |
| 로그인 불가 | 378, 313, 310초 | 19, 16, 0초 |
| 커널 OOM killer 개입 | 0/3 | 0/3 (데몬이 먼저 종료) |
| 읽기 처리량(정지 구간) | 중앙값 10.9 MB/s, 평균 10.6 MB/s, 평균 읽기 크기 94~99 KiB, 평균 약 105 IOPS | 평균 5.9~6.5 MB/s (정지가 짧아 평균이 낮음) |
| memory full / io full(정지 구간 평균) | 5.9~9.2% / 92.7~94.5% | 8.0~13.4% / 29.6~41.1% |

- 읽기 처리량이 제한값인 10.9 MB/s에 평평하게 붙는 것은 전달받은 장애 신호(10.87 MB/s)와 같은 모양이다. 평균 읽기 크기(94~99 KiB)와 IOPS(약 105)는 장애(76 KiB, 약 143)와 다르다.
- 굳어진 livelock에서는 io full이 93~95%이고 memory full은 4~9%다. 현재 판단 규칙(memory full 10초 평균 ≥ 10% 이고 가용 메모리 ≤ 10%)을 기록에 오프라인으로 적용하면 가용 메모리가 바닥난 뒤 첫 충족까지 12, 63, 142초가 걸렸고 정지 구간의 11~41%만 충족했다. io 조건을 더한 규칙은 12~19초, 94~97%였다([design/overview.md](../design/overview.md) 4절).
- 한계: 각 3회, VM 한 종류. 처음부터 일정한 상한이라 EBS 크레딧이 소진되며 느려지는 과정과 네트워크 지연은 모사하지 않는다. 쓰기도 같은 한도에 포함된다(`bps-total`).
