# Thrash Guard

리눅스 PSI(Pressure Stall Information)로 메모리 스래싱을 감지해, 시스템이 응답 불능에 빠지기 전에
원인 cgroup을 감속하거나 종료하는 유저스페이스 데몬이다. 기존 수단(systemd-oomd, MGLRU
`min_ttl_ms`)과 같은 조건에서 비교하는 재현 실험을 함께 만든다.

> 상태: M0(기반) 진행 중. 아직 측정 결과가 없으므로 성능 수치를 주장하지 않는다.

- 계획: [docs/plan.md](docs/plan.md)
- 로드맵과 결정 사항: [docs/roadmap.md](docs/roadmap.md)

## 구성 요소

| 경로 | 내용 |
|---|---|
| `src/common/` | `/proc`·cgroup 파서, 자기 보호, 할당 없는 문자열 조립 (수집기·데몬 공용) |
| `src/sampler/` | `tg-sampler`: 스래싱 중에도 버티도록 메모리를 고정하고, 지표를 UDP로 VM 밖에 보내는 수집기 |
| `lab/collect/` | `tg_receiver.py`: VM 밖에서 수집기 출력을 받아 저장하는 수신기 |
| `tests/` | 단위 테스트와 실제 커널에서 캡처한 `/proc` fixture |

## 빌드

리눅스(cgroup v2, PSI 활성)와 gcc가 필요하다.

```bash
make                # glibc 정적 링크 → build/glibc/tg-sampler
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
