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
