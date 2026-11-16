# 5. 최소 검증 계획 (3·4번 실행 범위 축소)

작성일: 2026-11-16  
기준: `2cccc9b` + 1·2·3번 미커밋 변경(코드 수정 완료 상태)  
대체 범위: [03](03-metrics-and-benchmarks.md)의 E절 정식 matrix(168 + 고정 도착률 18 + fallback 전후 12 + 스캔 연장 6)와 [04](04-lock-fairness.md)의 A절 반복 평가. 03의 A절(지표 계약)과 D절(측정 방법)은 그대로 유효하다.

## 왜 줄이는가

중단 전 `closed-v2` 첫 반복(worker 1·2·4) 결과로 다음이 이미 드러났다.

- ID 단건 조회(기존·신규·없는 ID)와 쓰기 50%는 병렬·직렬 서버 모두 약 2.1만~3.9만 RPS로 차이가 없다. 4개 프로세스 Python 부하 생성기의 한계를 재고 있어 서버 비교 근거가 되지 못한다.
- 두 서버의 차이가 보이는 부하는 스캔·쓰기 혼합뿐이다(worker 2: 병렬 6.47 RPS, 직렬 3.67 RPS + 큐 만료 79건).

남은 측정을 그대로 돌리면 3시간 이상 걸리지만 결론은 바뀌지 않는다. 그래서 아래 세 질문에 답하는 데 필요한 측정만 한다.

| 질문 | 근거로 쓸 검증 |
|---|---|
| Q1. 이번 동시성 변경(gate 분리, 잠금 지표 계측, `last_insert_id`)에 데이터 경쟁이 없는가 | V1 TSan |
| Q2. worker를 늘리면 실제로 병렬 처리 이득이 있는가 | B1 스캔·쓰기 혼합 |
| Q3. 쓰기 우선 gate가 지속적인 쓰기 부하에서 조회를 굶기는가 | B2 쓰기 90%·99% |

## 실행 항목

공통 환경: 이미지 `mini-dbms-phase3-review:3b19667`(Debian bookworm, GCC 12.2, CMake 3.25), 컨테이너 CPU 8개·메모리 6GiB. 기존 측정과 같다. 현재 작업 트리를 컨테이너 내부 `/source`로 복사해 빌드한다. DB 파일은 컨테이너 `/tmp`에 둔다.

| ID | 내용 | 조건 | 판정 기준 |
|---|---|---|---|
| V1 | TSan | Debug, `MINIDB_ENABLE_TSAN=ON`, `MINIDB_BUILD_HTTP=OFF`. `minidb_gate_test`, `minidb_engine_test`, `minidb_engine_gate_test`, `minidb_pool_test` | 4개 모두 exit 0이고 `WARNING: ThreadSanitizer` 출력이 없어야 한다 |
| V2 | Release 회귀 | `MINIDB_BUILD_BENCHMARKS=ON` Release 빌드 후 `ctest` 6개 suite 1회 | 모두 통과. B1·B2는 이 바이너리로 측정한다 |
| B1 | 병렬 이득 | `scan_write`. 병렬 서버는 worker 1·2·4·8, 직렬 서버는 worker 2·4·8. 워밍업 5초, 측정 30초, 반복 3회. 총 21개 | worker별 RPS 중앙값과 범위, 큐 만료 수를 비교한다. 반복 3회 모두에서 병렬 최솟값이 직렬 최댓값보다 크면 "병렬 이득 있음"으로 기록한다 |
| B2 | 공정성 | `write90`, `write99`. 병렬 서버만, worker 8. 워밍업 5초, 측정 30초, 반복 3회. 총 6개 | 아래 "공정성 판단 규칙"을 따른다 |

B1과 B2를 합쳐 27개 조건이다. 조건 하나에 약 1분이 걸리므로 약 30분이 예상된다. V1과 V2는 약 10분이다.

### 실행 환경 메모 — TSan

이 PC의 Docker(WSL2 커널)에서는 TSan 런타임이 `unexpected memory mapping`으로 시작 전에 종료된다. ASLR 엔트로피 문제다. 이 계획에서는 다음 조건에서 실행한다.

- `docker run --security-opt seccomp=unconfined`
- 각 테스트를 `setarch "$(uname -m)" -R <test>`로 실행해 해당 프로세스의 ASLR만 끈다

운영 실행 조건을 바꾸는 것이 아니라 검증 환경의 제약이므로, 결과에 이 조건을 함께 적는다.

### 공정성 판단 규칙

과제에 정해진 읽기 SLO가 없다. 그래서 이 규칙은 정책 변경 후보 실험을 시작할지 정하는 내부 기준이며, 성능 보장치가 아니다.

다음 중 하나가 **반복 3회 모두에서** 관찰되면 04의 B절(공정 gate 후보 실험)로 넘어간다.

- 읽기 p99가 같은 조건 쓰기 p99의 10배 이상이다.
- 쓰기는 계속 완료되는데 제출된 읽기가 하나도 완료되지 못한 최장 구간이 1초 이상이다(`report.py`의 pending-reader 구간 분석).

둘 다 아니면 **현재 쓰기 우선 정책을 유지**하고, 측정 범위와 "읽기 최대 대기시간은 보장하지 않음"을 기록한다.

## 실행하지 않는 항목과 근거

| 제외 항목 | 근거 |
|---|---|
| ID 조회 3종과 쓰기 50%의 전체 matrix | 부하 생성기 한계로 병렬·직렬 차이가 없다(`closed-v2` r0 참고). 정확성은 엔진·API 테스트가 검증한다 |
| fallback 최적화 전후 처리량 비교(12개) | 효과는 "없는 ID 조회 시 `fallbacks=0`" 카운터로 확정된다. `engine_test`와 `api_test`가 정확한 값으로 검사한다. 처리량 차이는 부하 생성기 한계에 가려진다 |
| 쓰기 비율별 직렬 서버 비교 | 직렬 서버는 공정성 정책의 후보가 아니다. 공정성은 운영 서버만 보면 된다 |
| 공정성 측정의 worker 1·2·4 | 동시에 기다릴 수 있는 writer 수가 가장 많은 worker 8이 최악 조건이다 |
| 고정 도착률 18개 | SLO가 없어 판정 기준이 없다. 과제 요구 범위 밖이다 |
| 스캔 120초 연장 | B1의 결론은 처리량 중앙값과 큐 만료 수로 내린다. 스캔 p99는 결론에 쓰지 않는다 |
| 직렬 서버 worker 1 | worker 1에서는 두 서버의 실행 구조가 같다 |
| 기존 `make test`(C 엔진) | `git diff -- src tests Makefile`이 비어 있어 C 코드는 바뀌지 않았다. 같은 트리에서 Codex 실행(30,407건, CLI 28개)이 통과했다 |

## 결과물

- 원시 결과: `docs/phase3-benchmarks/20261116/minimal/`
  - 조건별 `summary.json`과 `server.log`를 보관한다.
  - `requests.jsonl.gz`는 쓰기 부하 한 조건에 약 67MB라 커밋하지 않는다. `.gitignore`에 추가한다.
- 집계: `server/benchmarks/report.py`가 축소 matrix도 집계할 수 있도록 데이터셋과 예상 조건 수를 인자로 받게 한다.
- 보고서: `docs/phase3-benchmarks/20261116/minimal-report.md`. V1·V2 결과, B1 표, B2 표와 공정성 판단을 담는다.
- 03·04·README의 상태를 이 문서의 결과로 갱신한다.

## 완료 기준

- V1·V2가 PASS다. 실패하면 원인을 기록하고 B 단계로 넘어가지 않는다.
- B1·B2 27개 조건이 모두 실행된다. 각 조건에 정확성 오류(`WRONG_RESULT`, 응답한 INSERT 유실)가 없어야 한다.
- 보고서에 병렬 이득 판정과 공정성 판정, 그리고 제외 항목이 적혀 있다.

## 실행 기록 — 2026-11-16 완료

- **V1 TSan:** PASS. 4개 테스트 모두 exit 0이고 TSan 경고는 0건이다. 계측된 빌드인지는 `-fsanitize=thread`와 `__tsan_read*` 심볼로 확인했다.
- **V2 Release ctest:** 6/6 PASS.
- **B1·B2:** 27개 조건을 12:11–12:31 UTC에 실행했다. 정확성 오류와 응답한 INSERT 유실은 없었다.
  - B1: 병렬 이득 있음. worker 8에서 직렬 서버의 4.95배다.
  - B2: 판단 규칙 1을 충족했다. write99에서 읽기 p99가 쓰기 p99의 37.6–38.1배였다. 규칙 2는 충족하지 않았다(최장 0.113초).
- 상세 결과와 판단은 [minimal-report.md](../../phase3-benchmarks/20261116/minimal-report.md)에 있다.
- `report.py`에 `--dataset NAME=CASES` 인자를 추가했다. 인자를 생략하면 기존 전체 matrix 기준으로 동작한다.
- 공정 gate 후보 구현(04 B절)은 사용자 결정을 기다린다.

## 추가 실행 — 공정 gate 후보 축소 실험

- `server/benchmarks/fair_gate.hpp`(티켓 순서 gate), `MINIDB_BENCH_FAIR`, `minidb_server_fair` 타깃을 추가했다. 운영 서버의 gate는 그대로다.
- `gate_test.cpp`에 후보 gate의 순서 시나리오 2개를 추가했다. TSan 4개 테스트 통과(경고 0건), Release ctest 6/6 통과.
- write99, worker 8, 3회 측정 결과:
  - 읽기 p99: 120–131ms → 4.4ms
  - 처리량: 13,170 → 8,709 RPS (−34%)
  - 쓰기 p99: 3.3 → 4.4ms (+30%)
- 04 D절에 따라 채택하지 않았다. 결과는 [minimal-report.md B3](../../phase3-benchmarks/20261116/minimal-report.md#b3-공정-gate-후보-비교--쓰기-99-worker-8)에 있다.
