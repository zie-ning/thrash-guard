# 포트폴리오 그림

SVG 파일이다. 브라우저, 마크다운 미리보기, 편집기에서 바로 열린다. 글꼴은 맑은 고딕, Apple SD Gothic Neo, Noto Sans CJK KR 순으로 찾는다.

| 파일 | 내용 | 데이터 |
|---|---|---|
| `fig_2_2_livelock_timeline.svg` | 굳어진 livelock 한 번(약 6분)의 시계열. (a) 가용 메모리, (b) 10초 평균 memory full과 io full, 로그인 불가 구간 음영, memory 경로 임계 10%와 io 경로 임계 50% | `results/m3/20261113/G_throughput_dry_1` |
| `fig_2_3_first_decision.svg` | 같은 조건 dry-run 5회: 10초 평균 memory full/io full, 두 임계값, 두 종료 조건 중 가장 먼저 충족된 종료 결정 시각 | `G_throughput_dry_1..3`, `H_iopath_dry_1..2` |

## 다시 만들기

```bash
python portfolio/figures/make_figures.py
```

시각 0은 가용 메모리가 처음 30 MiB 미만이 된 때다. 로그인 불가 구간은 실패한 로그인 시도의 시작부터 다음 성공 시도의 끝까지다(10초 간격 시도라 경계 오차가 있다).

종료 결정 시각은 `make_figures.py`의 `DECISIONS`에 적은 값이고, 같은 기록을 실제 판단 함수에 재생해 구한다(두 종료 조건을 함께 켠 상태에서 처음 종료를 결정한 시각).

```bash
cc -std=c11 -D_GNU_SOURCE -Isrc/common -Isrc/daemon -o replay_judge lab/analyze/replay_judge.c \
   src/daemon/judge.c src/daemon/policy.c src/daemon/config.c src/daemon/match.c \
   src/common/procfs.c src/common/fmt.c src/common/cgscan.c
./replay_judge results/m3/20261113/G_throughput_dry_1/psi.tsv 10 50
```

## 읽을 때 주의

- 처리량 약 10.9 MB/s로 제한한 VM에서 데몬을 dry-run으로 돌린 기록이다. 실제 EBS의 크레딧 소진 과정과 네트워크 지연은 모사하지 않는다.
- 각 1회, 총 5회의 관찰이다(실행 4, 5는 최종 판단 규칙 빌드의 기록). 5회 모두 커널 OOM killer가 개입하지 않아 OOM 종료 시각은 표시할 것이 없고, 정지는 부하가 끝날 때까지 이어졌다. 정지 시간은 부하의 설정 시간(420초)이 끝난 시점에서 잘린 값이라 더 길어질 수 있다.
- 종료 결정 시각은 1초 간격 기록을 재생한 근사다(실제 틱은 250 ms).
