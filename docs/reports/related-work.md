# 관련 오픈소스 조사

> 조사일: 2026-11-11. M3 판단 로직(D15, D16, D20)을 정하는 과정에서, 비슷한 문제를 다루는 오픈소스가
> 어떻게 설계했는지 웹 검색과 원문(소스·문서) 읽기로 확인한 기록이다.
> **확인한 것**과 **확인하지 못한 것**을 구분해 적는다. 모든 내용은 조사 시점의 `master` 기준이라
> 배포판에 들어 있는 버전과 다를 수 있다.

## 1. 조사 대상

| 프로젝트 | 성격 | 조사 깊이 |
|---|---|---|
| systemd-oomd | systemd 일부. PSI + cgroup v2 기반 사용자 공간 OOM 종료 | man 페이지와 소스(미러) 원문 확인 |
| Facebook oomd | systemd-oomd의 원형으로 알고 있음(이번 조사에서 확인하지 못함). 플러그인 구조 | 문서·소스 일부 원문 확인 |
| Android lmkd | 안드로이드의 저메모리 종료 데몬. PSI 트리거 사용 | 공식 문서 전문, 소스는 앞부분만 원문 |
| earlyoom | 작은 C 데몬. 남은 메모리·swap 비율 기준 | 검색 요약만 |
| nohang | 설정이 풍부한 데몬. PSI 지원 | 검색 요약만 |

## 2. systemd-oomd (원문 확인)

| 항목 | 내용 |
|---|---|
| 압력 한도 | `DefaultMemoryPressureLimit` 기본 60% |
| 지속 시간 | `DefaultMemoryPressureDurationSec` 기본 30초. 한도를 넘은 상태가 이 시간 이상 지속돼야 동작 |
| 비교하는 값 | PSI **full의 avg10** |
| 지속 판정 | **연속형**. 초과가 시작된 시각을 기록하고, 한도 이하로 한 번이라도 내려가면 초기화 |
| 종료 대상 | 하위 cgroup 중 **pgscan 증가율**이 큰 것, 동률이면 메모리 사용량 |
| 종료 수단 | 하위 프로세스 전체에 SIGKILL(`cg_kill_recursive`). `cgroup.kill` 파일 사용 여부는 확인 못함 |
| swap | `SwapUsedLimit` 기본 90%. 시스템 메모리 사용률과 swap 사용률이 **둘 다** 넘을 때 동작 |
| 제외·회피 | `user.oomd_omit`(제외), `user.oomd_avoid`(순위 맨 뒤) cgroup 속성 |

확인하지 못한 것: 압력을 확인하는 타이머 간격(코드에 상수 `MEM_PRESSURE_INTERVAL_USEC`가 있으나 값은 헤더에 있어 읽지 못함),
`cgroup.kill` 사용 여부. Ubuntu 22.04의 systemd 249와 `master`는 세부 동작이 다를 수 있다.

## 3. Facebook oomd (원문 일부 확인)

| 항목 | 내용 |
|---|---|
| 목적 | PSI와 cgroup v2로 시스템을 보고, 커널 OOM 전에 사용자 공간에서 조치. Facebook은 30분짜리 livelock이 사라졌다고 보고(측정 조건은 글에 자세히 없음) |
| 압력 판정 | `pressure_above`: 10초 평균(sec_10)을 임계와 비교. **연속형**(처음 넘은 시각 기록, 내려가면 초기화). 60초·300초 값은 여러 cgroup 중 고를 때의 가중 비교에만 사용 |
| 설계 글의 주장 | "PSI만으로는 부족해서 시스템을 함께 본다". 구체적 이유는 글에 없음 |
| 병용 지표 | 메모리·anon 사용량, swap 사용량·여유, 최근 회수 여부, 형제 대비 사용량 증가율, page scan 증가량, io cost 등(플러그인별) |

종료 대상 선택 플러그인:

| 플러그인 | 기준 |
|---|---|
| `kill_by_pg_scan` | 한 이벤트 루프 tick의 page scan 증가량이 양수인 것 중 큰 순. 첫 호출은 기준값 샘플링만 하고 종료하지 않음 |
| `kill_by_pressure` | 압력을 가장 많이 만드는 하위 cgroup |
| `kill_by_memory_size_or_growth` | 3단계: 형제 합계의 일정 비율(기본 50%) 이상인 것 → 증가율이 높은 것(백분위 80, 평균 대비 1.25배 이상) → 나머지 |
| `kill_by_swap_usage` | swap 사용량 순 |

안전장치와 종료 수단:

| 항목 | 내용 |
|---|---|
| 종료 후 대기 | `post_action_delay`, 문서상 기본 15초 |
| dry-run | `dry=true` 옵션(대상만 로그에 남김) |
| 선호·회피 | cgroup xattr `oomd_prefer`(먼저 종료)·`oomd_avoid`(맨 뒤). `prefer`가 `avoid`보다 우선, 상속하지 않음 |
| 종료 기록 | 종료된 cgroup에 `oomd_kill` xattr를 프로세스 수만큼 증가 |
| 종료 수단 | 기본은 프로세스별 SIGKILL(블로그 문장 기준, 호출 코드는 읽지 못함). `kernelkill=true`이면 **대상을 freeze한 뒤 `cgroup.kill`**(기본 false). `reap_memory`(기본 true)는 `process_mrelease`로 메모리 회수 가속 |

확인하지 못한 것: 감시 주기, `pressure_above`의 some/full 구분, 예시 임계값, 재시도 횟수 상한,
`-inl.h` 구현 파일과 `BaseKillPlugin` 소스.

## 4. Android lmkd (문서 전문, 소스는 앞부분만 원문)

| 항목 | 내용 |
|---|---|
| 배경 | 커널 4.12에서 커널 내 low memory killer가 빠지면서 사용자 공간으로 이동. 예전 vmpressure 신호는 오탐이 많아 PSI 트리거로 교체 |
| PSI 트리거 | partial(some)과 complete(full)을 등록. 코드 배열 초기값은 some 70ms, some 100ms, full 70ms(1초 윈도). 문서의 기본값은 partial 70ms(저램 200ms), complete 700ms로 **full 값이 코드와 달라** 실제 적용값은 확인하지 못함 |
| 트리거 이후 | 윈도(1000ms) 동안 폴링. 폴링 주기는 상수 주석상 10ms(압력 높음)·100ms(낮음). 둘 중 고르는 조건은 확인 못함 |
| 스래싱 판정 | workingset refault가 file-backed pagecache 대비 비율이 `thrashing_limit`(기본 100, 저램 30)을 넘으면 스래싱. 킬 후에도 회복되지 않으면 `thrashing_limit_decay`만큼 임계를 낮춤 |
| swap 지표 | `swap_free_low_percentage`(기본 20, 저램 10), `swap_util_max`(기본 100, 사실상 비활성) |
| 대상 선택 | `oom_score_adj`가 높은 순. 구간에 따라 가장 큰 프로세스(RSS)를 고름(소스 요약 기준) |
| 쿨다운 | `kill_timeout_ms` 기본 0(비활성). PSI 경로 적용 여부는 확인 못함 |
| 종료 수단 | pidfd 사용 확인. SIGKILL 여부와 cgroup 사용 여부는 확인 못함 |

확인하지 못한 것: 종료 결정의 지속 시간 조건, 최종 종료 결정 로직(`lmkd.cpp`의 잘린 부분), README(503 오류).

## 5. earlyoom, nohang (검색 요약만)

- **earlyoom**: 작은 C 데몬. 가용 메모리와 swap이 모두 기본 10% 아래로 내려가면 가장 큰 프로세스를 종료. PSI를 쓰지 않음. Fedora 32 Workstation에서 기본 활성화.
- **nohang**: earlyoom보다 설정이 풍부. PSI 지원, zram 인식 트리거, 커널 로그 확인. README가 스스로 문서가 부실하다고 적음.

두 프로젝트의 소스는 읽지 않았다. 언어와 규모도 확인하지 못했다(earlyoom이 C라는 것만 확인).

## 6. 우리 설계와의 비교

| 항목 | 우리 (결정 ID) | systemd-oomd | oomd | lmkd |
|---|---|---|---|---|
| 깨우는 방식 | PSI 트리거, `workload.slice` full (D16) | 주기 확인으로 추정 | 확인 못함 | PSI 트리거 |
| 판단 입력 | 증가분의 최근 N초 평균 (D15) | full avg10 | 10초 평균 | 윈도 내 폴링 |
| 지속 판정 | 평균형, 평균이 X/2 미만이면 관찰 종료 | 연속형 | 연속형 | 확인 못함 |
| 대상 선택 | 자격 통과자 중 `pgscan` 증가량 최대 (D20) | pgscan 증가율 | 플러그인별(`pg_scan` 등) | `oom_score_adj`, 크기 |
| 보조 지표 | M3는 PSI와 `pgscan`뿐, 복합 판단은 M5 | swap | 다수 | refault, swap |
| 쿨다운 | 60초 + 시간당 3회 | 확인 못함 | 15초(문서) | 기본 0 |
| dry-run | 기본 켬 | 확인 못함 | 옵션 | 확인 못함 |
| 보호 목록 | 없음 | omit/avoid | prefer/avoid | `oom_score_adj` |
| 종료 수단 | `cgroup.kill` 직접 쓰기 | SIGKILL(재귀) | SIGKILL, 옵션으로 freeze + `cgroup.kill` | pidfd |

### 확인된 점

- D20(`pgscan` 증가량 기준)은 systemd-oomd와 oomd `kill_by_pg_scan`이 쓰는 기준과 같다. 첫 샘플은 기준값으로만 쓰고 종료하지 않는 방식도 같다.
- 두 oomd 계열은 지속 판정을 연속형으로 한다. 다만 입력이 이미 10초 평균이다. 우리는 원시 증가분을 평균하는 평균형(D15)이라 입력과 판정이 모두 다르다. 이 차이가 효과로 이어지는지는 측정하지 않았다.
- lmkd와 oomd는 PSI 외 지표(refault, swap, 사용량)를 함께 쓴다. M5의 복합 판단(F1) 방향과 일치한다.

## 7. 비교에서 나온 후보 (결정 아님)

| # | 후보 | 확인할 것 |
|---|---|---|
| 1 | 컨테이너 보호·우선 종료 목록(oomd의 `avoid`/`prefer` 같은 수단) | 이름 패턴 설정으로 M3-3 후보 표에 붙일 수 있는지 |
| 2 | 종료 후 메모리 회수 가속(oomd `process_mrelease`) | `cgroup.kill` 뒤 커널이 어떻게 회수하는지, 복구 시간에 의미가 있는지 |
| 3 | 종료 전 freeze(oomd `kernelkill`) | `cgroup.kill` 자체가 fork 경합을 처리하는지(제 기억은 처리하도록 설계됨, 미확인) |
| 4 | 트리거 뒤 폴링 간격 단축(lmkd 10~100ms) | 우리 250ms가 충분한지. 측정 대상 |

## 8. 한계

- 위 내용 중 소스 일부는 비공식 미러(systemd)와 도구가 만든 요약(lmkd.cpp 일부)에 기대고 있다.
- 서로를 객관적으로 비교한 벤치마크는 찾지 못했다. 이 문서는 설계 비교이며 성능 비교가 아니다.
- 우리 설계의 효과는 아직 측정하지 않았다.

## 출처

- [systemd oomd.conf(5)](https://man7.org/linux/man-pages/man5/oomd.conf.5.html)
- [systemd.resource-control(5)](https://man7.org/linux/man-pages/man5/systemd.resource-control.5.html)
- [systemd oomd-manager.c (미러)](https://third-party-mirror.googlesource.com/systemd/+/refs/heads/master/src/oom/oomd-manager.c)
- [systemd oomd-util.c (미러)](https://third-party-mirror.googlesource.com/systemd/+/refs/heads/master/src/oom/oomd-util.c)
- [facebookincubator/oomd](https://github.com/facebookincubator/oomd)
- [Open-sourcing oomd (Facebook Engineering, 2018-07-19)](https://engineering.fb.com/production-engineering/oomd/)
- [Android lmkd 문서](https://source.android.com/docs/core/perf/lmkd)
- [lmkd.cpp (AOSP)](https://android.googlesource.com/platform/system/memory/lmkd/+/refs/heads/main/lmkd.cpp)
- [earlyoom (AlternativeTo)](https://alternativeto.net/software/earlyoom)
- [hakavlad/nohang](https://github.com/hakavlad/nohang)
