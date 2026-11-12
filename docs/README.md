# 문서 안내

Thrash Guard의 문서 색인이다. 처음 읽는다면 **plan, design, validation** 순서를 권한다.

## 프로젝트

| 문서 | 내용 |
|---|---|
| [plan.md](plan.md) | 무엇을, 왜 하는가: 목적, 배경, 범위 |
| [roadmap.md](roadmap.md) | 개략적인 진행 순서와 현재 상태, 미결 사항, 작업 규칙 |

## 설계: 어떻게 만드는가

| 문서 | 내용 |
|---|---|
| [design/overview.md](design/overview.md) | 데몬 설계의 핵심: 언제 개입하고, 누구를 종료하고, 어떻게 종료하는가 |
| [design/victim-selection.md](design/victim-selection.md) | 희생자 선택: 왜 압력이 아니라 사용량인가, 규칙, 예시, 대안, 한계 |
| [design/protection.md](design/protection.md) | 보호 설계: 필수 프로세스 보호, 데몬 자기 보호, 후보 스캔 구조 |
| [design/log-ringbuffer.md](design/log-ringbuffer.md) | 로그 링버퍼와 기록 스레드 |

## 검증: 어떻게 확인하는가

| 문서 | 내용 |
|---|---|
| [validation/overview.md](validation/overview.md) | 효과 검증 설계: 구성 요소, 조건, 합격 기준, 위험 |
| [validation/measurement.md](validation/measurement.md) | 측정 정의와 규칙 |

## 환경과 기록

| 문서 | 내용 |
|---|---|
| [lab/environment.md](lab/environment.md) | 실험 환경, VM 플랫폼 선택 경과, 지금까지의 실측 기록 |
| [reports/related-work.md](reports/related-work.md) | 선행 도구 조사 (작성 시점의 설계 기준) |

## 폴더 구성과 문서를 추가할 때

```text
docs/
├── plan.md, roadmap.md   # 무엇을·왜, 순서·진행 상황 (짧게 유지)
├── design/               # 어떻게 만드는가: overview.md가 핵심, 나머지는 주제별 상세 설계
├── validation/           # 어떻게 확인하는가: overview.md가 검증 설계, measurement.md가 측정 규칙
├── lab/                  # 어디서 돌리고 무엇을 쟀는가: 환경과 실측 기록
└── reports/              # 조사와 결과 보고서
```

- 목적이나 범위가 바뀌면 `plan.md`, 순서나 진행 상황은 `roadmap.md`에 적는다.
- 데몬 설계의 핵심은 `design/overview.md`에 쓰고, 주제별 상세 설계는 `design/`에 새 문서로 만들어 이 색인에 추가한다. 설계 결정이 바뀌면 해당 설계 문서에 반영한다.
- 검증 방법과 측정 규칙은 `validation/`, 환경과 실측 값은 `lab/`에 둔다.
- 조사나 측정 결과 보고서(예: M4 효과 검증 보고서)는 `reports/`에 둔다.
