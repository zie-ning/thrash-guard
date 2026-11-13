# M3 VM 검증 기록 (2026-11-13)

데몬(`thrash-guard`)을 실험 VM에서 스래싱 부하와 함께 돌린 원본 기록이다. 모든 실행은 같은 VM 구성이다:
Ubuntu 22.04, 커널 6.8.0-138, 메모리 1 GiB, 2 vCPU, swap 없음, **디스크 상한 100 IOPS / 125 MiB/s**(M1에서 스래싱이 나는 조건으로 확인한 유도 조건. 실제 길벗 장애의 병목은 IOPS가 아니라 처리량 약 10.9 MB/s였다고 전달받았다. `docs/lab/environment.md` 5.6절),
도커(cgroup 드라이버 systemd), 도커 밖에 `mlock` 43 MiB 고정(`tg-load pin`). 부하는 `tg-load anon`을 도커 컨테이너 안에서 실행했다. 컨테이너 이미지는 `tg-load` 하나만 담아 로컬에서 만들었다.

## 실행 목록

| 디렉터리 | 데몬 모드 | `full_pct` | 부하 | 결과 |
|---|---|---|---|---|
| `A1_full40_single_kill` | 실제 종료 | 40 | hog 650 MiB, 초당 4 MiB | **데몬이 종료**(10초 평균 40.7%). 시스템 정지 약 2분 10초 |
| `A2_full40_two_containers_kill` | 실제 종료 | 40 | hog 250 MiB(초당 4) + prot 450 MiB(초당 2, `oom_score_adj=-1000`) | 데몬은 종료하지 않음(평균 최대 18.4%). 커널 OOM killer가 hog를 종료. 정지 약 1분 50초 |
| `B1_full40_psirec_dryrun` | dry-run | 40 | hog 650 MiB, 초당 4 | 데몬은 종료하지 않음(dry-run). 커널 OOM killer가 hog를 종료. PSI(memory, io, cpu) 1초 기록 있음 |
| `C1`~`C3_full10_kill` | 실제 종료 | **10** | hog 650 MiB, 초당 4 | 3회 모두 **데몬이 종료**. 가용 메모리 30 MiB 미만에서 종료까지 12~15초 |
| `D_protect_plain_1`~`3` | 실제 종료 | 10 | prot 450 MiB(초당 2) + hog 250 MiB(초당 4), 둘 다 보호 없음 (대조군) | 3회 모두 **데몬이 prot을 종료**(prot 약 400 MiB, 증가 중), hog는 생존 |
| `D_protect_protected_1` | 실제 종료 | 10 | 위와 같고 prot에 `--oom-score-adj=-1000` | 데몬은 종료하지 않음(종료 0회). **커널 OOM killer가 hog를 종료**. 배너 정지 약 59초. 결과에서 제외 |
| `D_protect_protected_2`, `3` | 실제 종료 | 10 | 위와 같음 (처치군) | 2회 모두 **데몬이 hog를 종료**, prot은 `protected=1 eligible=0`으로 로그에 남고 생존 |
| `E_memorylow_none_1`~`3` | dry-run | 10 | hog 650 MiB(초당 4), `MemoryLow` 없음 | 로그인 불가 333, 315, 198초(3회차는 커널 OOM killer가 종료해 짧음) |
| `E_memorylow_chain_1`~`3` | dry-run | 10 | 같은 부하, `ssh.service`, `system.slice`, `user.slice`에 `MemoryLow=32M` | 로그인 불가 359, 484, 305초. **개선 없음**. `memory.events`의 `low`(보호 구간 안인데도 회수된 횟수)가 3.7만~57만회 |
| `G_throughput_dry_1`~`3` | dry-run | 10 | **디스크 처리량 10.9 MB/s 제한**(IOPS 3000), hog 650 MiB(초당 4) | 3회 모두 livelock 5분 이상(가용 메모리 30 MiB 미만 376, 309, 302초), 커널 OOM 개입 없음, 로그인 불가 378, 313, 310초 |
| `G_throughput_kill_1`~`3` | 실제 종료 | 10 | 위와 같음 | 3회 모두 **데몬이 종료**, 가용 메모리 30 MiB 미만 지속 21, 13, 21초, 로그인 불가 19, 16, 0초 |
| `H_iopath_dry_1`~`2` | dry-run | 10 | G와 같은 조건, **io 경로를 추가한 새 빌드**(`io_full_pct=50`, `io_mem_avail_pct=5`) | 2회 모두 livelock 5분 안팎(가용 30 MiB 미만 363, 296초, 로그인 불가 372, 291초), 커널 OOM 개입 없음 |
| `H_iopath_kill_1`~`2` | 실제 종료 | 10 | 위와 같음 | 2회 모두 **io 경로(`path=io`)로 데몬이 종료**. 결정 시점의 평균 memory full은 9.5%, 9.2%로 임계 10% 아래, io full 90%, 79%, 가용 0%. 가용 30 MiB 미만 지속 22초, 로그인 불가 20초와 0초 |

`D_protect_protected_2`의 `run.log`에는 컨테이너 시작 직후의 `oom_score_adj` 확인 출력이 없다(클라이언트 시간 초과로 출력만 유실, 컨테이너는 정상 시작). 보호 적용은 판단 로그의 `protected=1`로 확인한다.

## 파일

- `run.log`: 실행 스크립트 로그(10초 간격 상태, 데몬 로그, dmesg의 OOM 줄, 데몬 통계).
- `ssh_login.probe`(E만): 10초 간격 전체 로그인 시도. `시작에포크 걸린초 종료코드 | 상태`.
- `ssh_banner.probe`: WSL에서 2초 간격으로 VM의 sshd 배너를 받아 본 결과(`에포크초 ok|fail`). 정지 구간 측정용. A1에는 없다.
- `psi.tsv`: `psirec` 기록. G 실행은 끝에 `vda` 읽기 완료 횟수와 읽은 섹터 수(512바이트) 두 열이 더 붙은 10열이다.
- `psi.tsv`(8열, G 이전): `psirec` 기록. `에포크_ms memory_some memory_full io_some io_full cpu_some cpu_full MemAvailable_kB`, 누적 us. A1, A2에는 없다.
- `scripts/`: 실행 스크립트 원본과 `psirec.c`. 스크립트는 작성 당시 경로(WSL의 `/mnt/c/...`)를 그대로 쓴다.

## 읽을 때 주의

- 각 조건당 1~3회 관찰이다. 같은 조건에서도 정지 시간이 14초~2분으로 크게 달랐다. 통계로 읽지 않는다.
- A1~B1의 정지 시간은 사람이 쓰는 SSH 접속 시도(10초 간격)로 잰 값이 섞여 있어 오차가 크다. B1과 C의 배너 감시가 더 정확하다.
- 데몬을 끈 대조군이 없어 커널 OOM killer보다 빠르다고는 말할 수 없다.
- H의 dry-run 로그에는 `hourly_limit`와 `no_candidate` 건너뜀이 번갈아 남는다. dry-run의 가상 종료도 시간당 상한을 소모하기 때문이고, `no_candidate`는 후보의 윈도 준비 상태가 틱 간격의 흔들림에 따라 오가는 것으로 보이나 확인하지 못했다(실제 종료 모드에서는 첫 결정에서 종료되어 영향이 작았다).
- 이 기록은 길벗 장애 재현이 아니다. 디스크 상한 100 IOPS는 장애의 실제 병목(처리량 약 10.9 MB/s)과 다른 축의 유도 조건이고, 이 환경에서는 정지의 지속 병목이 memory가 아니라 io였다(io full 약 96~100%).
