# Thrash Guard

메모리 압박(스래싱)으로 시스템이 사실상 멈췄는데도 커널 OOM killer가 발동하지 않는 상황에서, OOM killer를 기다리지 않고
더 빨리 메모리 압박의 원인을 종료해 시스템을 되살리는 리눅스 유저스페이스 데몬이다. 리눅스 PSI(Pressure Stall Information)로
멈춤을 감지하고, 스래싱 중에도 필수 프로세스(sshd 등)는 지킨다.

> 상태: 개발 중(데몬 MVP 구현 진행). 아직 측정 결과가 없으므로 성능 수치를 주장하지 않는다.

문서는 [docs/README.md](docs/README.md)에서 찾는다. 목적과 범위는 [docs/plan.md](docs/plan.md), 진행 상황은 [docs/roadmap.md](docs/roadmap.md)에 있다.

## 구성 요소

| 경로 | 내용 |
|---|---|
| `src/common/` | `/proc`·cgroup 파서, 자기 보호, 할당 없는 문자열 조립 (수집기·데몬 공용) |
| `src/sampler/` | `tg-sampler`: 스래싱 중에도 버티도록 메모리를 고정하고, 지표를 UDP로 VM 밖에 보내는 수집기 |
| `src/daemon/` | `thrash-guard` 데몬: 설정, 안전장치, 후보 탐색, 판단, 로그, 이벤트 루프, 종료 경로 (보호 drop-in과 서비스 파일은 구현 예정) |
| `lab/collect/` | `tg_receiver.py`: VM 밖에서 수집기 출력을 받아 저장하는 수신기 |
| `tests/` | 단위 테스트와 실제 커널에서 캡처한 `/proc` fixture |

## 빌드

리눅스(cgroup v2, PSI 활성)와 gcc가 필요하다.

```bash
make                # glibc 정적 링크 → build/glibc/tg-sampler, build/glibc/thrash-guard
make LIBC=musl      # musl 정적 링크 (musl-tools 필요)
make test           # 단위 테스트 (ASan/UBSan)
make lint           # cppcheck
```

## 수집기 실행 예

```bash
# VM 밖(수신 측)
python3 lab/collect/tg_receiver.py --port 9999 --out run.log

# VM 안(측정 대상). mlockall에는 root 또는 충분한 RLIMIT_MEMLOCK이 필요하다.
sudo ./tg-sampler --dest <수신기 IP>:9999 --interval-ms 250
```
