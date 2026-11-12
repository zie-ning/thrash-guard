| anon | file | pin | IOPS | 속도 | 판정 | SSH 최장 무응답(초) | MemAvailable 최소(MiB) | OOM kill | 실행 |
|---|---|---|---|---|---|---|---|---|---|
| 700 | 0 | 0 | None | 4 | oom_kill | 123.0 | 0.0 | 1 | 01-a700f0r4-1 |
| 700 | 0 | 0 | None | 4 | oom_kill | 340.6 | 0.0 | 1 | 02-a700f0r4-2 |
| 700 | 0 | 0 | None | 4 | oom_kill | 286.2 | 0.0 | 1 | 03-a700f0r4-3 |
| 700 | 0 | 0 | 150 | 4 | livelock | 605.0 | 0.0 | 0 | 04-a700f0r4-4 |
| 700 | 0 | 0 | 150 | 4 | livelock | 602.2 | 0.0 | 0 | 05-a700f0r4-5 |

합계: livelock 2회, oom_kill 3회
