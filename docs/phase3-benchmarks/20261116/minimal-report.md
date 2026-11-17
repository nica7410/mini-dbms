# Phase 3 최소 검증 결과 (2026-11-16)

계획: [05-minimal-verification.md](../../plans/phase3-improvements/05-minimal-verification.md)  
대상: `2cccc9b` + 1·2·3번 미커밋 변경. source id는 [minimal/source-id.txt](minimal/source-id.txt)에 있다.  
환경:
- Docker 이미지 `mini-dbms-phase3-review:3b19667`(Debian bookworm, GCC 12.2, CMake 3.25, Python 3.11)
- 컨테이너 CPU 8개·메모리 6GiB, 호스트 i7-10875H(8코어/16스레드), WSL2
- DB 파일은 컨테이너 `/tmp`에 둠
- 부하 생성기는 서버와 같은 컨테이너에서 4개 프로세스·연결 32개로 실행

측정 시간: 12:11–12:31 UTC(약 20분, 27개 조건)

## 요약

| 질문 | 결과 |
|---|---|
| Q1. 동시성 변경에 데이터 경쟁이 없는가 | **PASS.** TSan 테스트 4개 모두 통과, 경고 0건 |
| Q2. worker를 늘리면 병렬 이득이 있는가 | **있음.** 스캔 혼합 처리량이 worker 8에서 직렬 서버의 4.9배이며, 반복 3회 범위가 겹치지 않음 |
| Q3. 쓰기 우선 gate가 조회를 굶기는가 | **멈춤은 없지만 지연 증가는 큼.** 쓰기 99% 부하에서 읽기 p99가 쓰기 p99의 약 38배(3회 모두). 계획의 판단 규칙 1번을 충족해 공정 gate 후보 실험 조건에 해당 |
| 깨우기 개선. gate 대기가 줄어드는가 | **채택.** 쓰기 99%·worker 8에서 처리량 약 2.5배(1.3만→3.3만 RPS), 배타 gate 대기 270→57µs, 읽기 p99 약 130→20ms(B5) |
| 구조 전환. 인덱스를 공유하면 비용이 줄어드는가 | **채택.** 처리량은 같고, worker 8에서 메모리가 313→41MB, 시작 시간이 약 1초→0.1초로 줄었다(B4) |
| Q3 후속. 공정 gate로 바꾸면 나아지는가 | **채택하지 않음.** 읽기 p99는 약 4.4ms로 개선됐지만 처리량이 34% 줄고 쓰기 p99가 30% 늘었다. 현재 정책을 유지한다(B3) |

## V1. ThreadSanitizer — PASS

```sh
docker run --security-opt seccomp=unconfined ...
cmake -S server -B /build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMINIDB_BUILD_HTTP=OFF -DMINIDB_ENABLE_TSAN=ON
setarch "$(uname -m)" -R /build-tsan/<test>
```

| 테스트 | exit | TSan 경고 | 출력 |
|---|---:|---:|---|
| `minidb_gate_test` | 0 | 0 | Gate: 2 deterministic scenarios passed |
| `minidb_engine_test` | 0 | 0 | observed concurrent C executor calls: 4 |
| `minidb_engine_gate_test` | 0 | 0 | Engine gate integration passed |
| `minidb_pool_test` | 0 | 0 | Thread pool: 7 tests passed |

`-fsanitize=thread`로 빌드됐고, 바이너리에 `__tsan_read*` 심볼이 있음을 확인했다.

이 PC의 WSL2 커널에서는 ASLR 엔트로피 때문에 TSan 런타임이 `unexpected memory mapping`으로 시작하지 못한다. 그래서 seccomp를 해제하고 `setarch -R`로 해당 테스트 프로세스의 ASLR만 껐다. 이는 검증 환경의 제약이며 서버 실행 조건과는 무관하다. HTTP 계층(Drogon)은 TSan 빌드에 포함하지 않았다.

## V2. Release 회귀 — PASS

`MINIDB_BUILD_BENCHMARKS=ON` Release 빌드에서 `ctest`를 실행해 6개 suite가 모두 통과했다(`phase3_gate`, `phase3_engine`, `phase3_pool`, `phase3_engine_gate`, `phase3_api`, `phase3_control_api`). B1·B2는 이 빌드의 `minidb_server`와 `minidb_server_serial`로 측정했다.

C 엔진(`src/`, `tests/`, `Makefile`)은 `git diff`상 변경이 없어 `make test`는 다시 실행하지 않았다.

## B1. 병렬 이득 — 스캔·쓰기 혼합

스캔이 요청의 90%, INSERT가 10%다. 스캔은 100만 행 전체를 읽는다. 측정은 워밍업 5초, 측정 30초, 반복 3회다. RPS는 반복 3회의 중앙값이고 괄호 안은 최소–최대다.

| W | 병렬 RPS | 직렬 RPS | 병렬/직렬 | 병렬 W1 대비 | 큐 만료 합계 (병렬 / 직렬) | 스캔 p50 ms (병렬 / 직렬) | RSS | 시작 시간 |
|---:|---:|---:|---:|---:|---|---|---:|---:|
| 1 | 3.73 [3.70–3.80] | — | — | 1.00× | 242 / — | 5,027 / — | 75 MB | 0.21 s |
| 2 | 6.67 [6.50–6.83] | 3.77 [3.60–3.80] | 1.77× | 1.79× | 3 / 222 | 4,748 / 5,383 | 109 MB | 0.31 s |
| 4 | 11.73 [11.60–11.77] | 3.70 [3.40–3.87] | 3.17× | 3.14× | 0 / 197 | 2,681 / 5,758 | 177 MB | 0.54 s |
| 8 | 18.13 [17.77–18.50] | 3.67 [3.47–3.77] | 4.95× | 4.86× | 0 / 120 | 1,715 / 6,816 | 313 MB | 0.95 s |

- **판정:** worker 2·4·8 모두에서 병렬 서버의 최솟값이 직렬 서버의 최댓값보다 크다. 따라서 "병렬 이득 있음"이다.
- 서버 지표 `peak_executions`가 병렬 서버에서 W와 같고 직렬 서버에서는 1이어서, C 엔진 실행이 실제로 겹쳤음을 뒷받침한다.
- worker 8에서 이득이 4.9배에 그치는 데에는 컨테이너 CPU 8개를 서버와 부하 생성기가 함께 쓰는 점도 영향을 줄 수 있다. 원인은 분리해 측정하지 않았다.
- **비용:** worker마다 조회용 `Table`과 인덱스를 따로 가진다. 그래서 RSS가 75MB에서 313MB로, 시작 시간이 0.21초에서 0.95초로 W에 거의 비례해 늘어난다.
- **큐 만료:** 큐 만료 기준은 5초다. 연결 32개가 스캔(약 5초)을 계속 보내므로 worker가 적으면 대기가 5초를 넘는다. worker 1에서는 서버 종류와 관계없이 큐 만료가 생기고, 병렬 W2에서도 첫 반복에 3건이 생겼다.

## B2. 공정성 — 쓰기 90%·99%, worker 8, 병렬 서버

| 부하 | 반복 | RPS | 읽기 p95 / p99 ms | 쓰기 p95 / p99 ms | 읽기 p99 ÷ 쓰기 p99 | read_gate 평균 / 최대 대기 | write_gate 평균 | 쓰기만 완료되고 읽기 무진행인 최장 구간 |
|---|---:|---:|---|---|---:|---|---:|---:|
| write90 | r0 | 13,090 | 10.98 / 16.36 | 2.79 / 3.44 | 4.8× | 2.95 ms / 41.9 ms | 0.24 ms | 0.014 s |
| write90 | r1 | 13,474 | 10.78 / 15.46 | 2.64 / 3.01 | 5.1× | 2.91 ms / 30.6 ms | 0.23 ms | 0.013 s |
| write90 | r2 | 12,964 | 11.33 / 16.89 | 2.79 / 3.77 | 4.5× | 3.01 ms / 53.4 ms | 0.24 ms | 0.019 s |
| write99 | r0 | 13,170 | 81.86 / 124.35 | 2.77 / 3.31 | 37.6× | 24.3 ms / 281.6 ms | 0.27 ms | 0.110 s |
| write99 | r1 | 13,281 | 81.91 / 120.58 | 2.73 / 3.17 | 38.0× | 23.9 ms / 256.8 ms | 0.27 ms | 0.098 s |
| write99 | r2 | 12,863 | 83.85 / 131.03 | 2.87 / 3.44 | 38.1× | 24.6 ms / 251.9 ms | 0.28 ms | 0.113 s |

- 정확성 오류(`WRONG_RESULT`, 응답한 INSERT 유실)와 HTTP 오류는 0건이다.
- 최대 대기 값은 워밍업과 준비 단계를 포함한 실행 전체의 최댓값이다. 읽기 표본 수는 write90에서 약 3.9만, write99에서 약 3,900이다.
- **원인 분리:** 스레드 풀 큐의 평균 대기는 약 1.5ms로 모든 요청에 비슷하다. 반면 쓰기 99%에서 읽기는 read_gate에서 평균 24ms를 기다리고, 쓰기는 write_gate에서 0.27ms를 기다린다. 읽기 지연이 늘어난 주원인은 큐가 아니라 쓰기 우선 gate다. 대기 중인 writer가 거의 항상 있어서 새 reader가 계속 밀린다.

### 판단 (계획 05의 규칙 적용)

| 규칙 | 기준 | 관측 | 충족 |
|---|---|---|---|
| 1 | 반복 3회 모두 읽기 p99 ≥ 쓰기 p99 × 10 | write99: 37.6×, 38.0×, 38.1× | **예** |
| 2 | 반복 3회 모두 읽기 무진행 최장 구간 ≥ 1초 | 최대 0.113초 | 아니오 |

- 규칙 1을 충족하므로 [04](../../plans/phase3-improvements/04-lock-fairness.md)의 B절, 즉 도착 순서(티켓) 기반 공정 gate 후보 실험의 진입 조건에 해당한다.
- 관측된 현상은 **읽기가 멈추는 기아가 아니라 지연 증가**다. 이 측정은 연결 32개가 쉬지 않고 요청을 보내는 포화 조건이다. 쓰기 90%에서는 읽기 p99가 쓰기 p99의 약 5배로 기준 아래였다.
- 정책 변경은 아직 하지 않았다. 후보를 구현할지는 사용자가 결정한다. 구현하지 않으면 "포화 상태의 쓰기 99% 부하에서 읽기 p99 약 120–131ms"를 알려진 한계로 기록한다.

## B3. 공정 gate 후보 비교 — 쓰기 99%, worker 8

후보는 도착 순서(티켓) gate인 [`server/benchmarks/fair_gate.hpp`](../../../server/benchmarks/fair_gate.hpp)이다. 벤치마크 전용 빌드 `minidb_server_fair`(`MINIDB_BENCH_FAIR`)에서만 사용하고, 운영 서버의 gate는 그대로 두었다.

- **동작:** 먼저 대기 등록한 요청부터 들어간다. 연달아 등록된 reader는 함께 들어간다.
- **검증:** `minidb_gate_test`에 순서 시나리오 2개를 추가했다. 하나는 "늦게 온 reader가 대기 중인 writer를 앞지르지 못함", 다른 하나는 "대기 중인 reader 둘이 함께 들어가고 늦게 온 writer가 이들을 앞지르지 못함"이다. TSan(경고 0건)과 Release ctest 6/6을 통과했다.
- **측정 조건:** B2와 같다. 현재 정책의 값은 B2의 write99 결과를 그대로 썼다. 공정 gate 결과는 [minimal/fair-ticket/](minimal/fair-ticket)에 있다.

| 지표 (반복 3회) | 현재: 쓰기 우선 | 후보: 티켓 순서 | 변화 |
|---|---|---|---|
| 처리량 RPS 중앙값 [범위] | 13,170 [12,863–13,281] | 8,709 [8,700–8,751] | **−34%** |
| 읽기 p99 ms | 120.6–131.0 | 4.37–4.43 | **약 28배 개선** |
| 쓰기 p99 ms | 3.17–3.44 | 4.37–4.44 | **+약 30% 악화** |
| 쓰기 p95 ms | 2.73–2.87 | 4.09–4.12 | +약 45% 악화 |
| read_gate 평균 / 최대 대기 | 24 ms / 252–282 ms | 0.83 ms / 1.4–1.6 ms | 개선 |
| write_gate 평균 대기 | 0.27 ms | 0.80 ms | 악화 |
| 읽기 무진행 최장 구간 | 0.098–0.113 s | 0.005–0.011 s | 개선 |
| 정확성 오류, HTTP 오류 | 0 | 0 | — |

### 판단 — 채택하지 않음, 현재 정책 유지

- [04 D절](../../plans/phase3-improvements/04-lock-fairness.md)은 "읽기는 개선되고 쓰기나 처리량이 반복 변동을 넘어 나빠지면 자동 채택하지 않는다"고 정했다. 이번 결과가 여기에 해당한다.
  - 처리량 감소 폭(−34%)과 쓰기 p99 증가 폭(+30%)이 모두 반복 3회의 범위를 넘는다.
- 원인은 측정하지 않았다. 가장 유력한 원인은 다음과 같다.
  - 엄격한 순서 보장 때문에 잠금을 넘길 때마다 다음 순번 스레드를 깨워야 한다.
  - 그래서 이미 실행 중인 writer가 바로 이어받는 경로가 사라진다.
- 따라서 운영 기본 gate는 **쓰기 우선으로 유지**한다. 알려진 한계는 다음과 같다.
  - worker 8에서 쓰기 99%로 포화시키면 읽기 p99가 약 120–131ms까지 늘어난다. 이후 B5의 깨우기 개선으로 약 20ms가 됐다.
  - 읽기 최대 대기시간은 보장하지 않는다.
- 쓰기 비중이 높은 상황에서 읽기 지연이 요구사항이 되면, 이 후보와 측정 결과를 출발점으로 쓸 수 있다. 예를 들어 연속 writer 수에 상한을 두는 절충안을 검토할 수 있다.

## B4. 단일 인덱스 구조 전환 — 인덱스 복제 대비

[계획 06](../../plans/phase3-improvements/06-single-index.md)에 따라 구조를 바꿨다. 이전에는 worker마다 `Table`을 열어 인덱스를 복제했고, 이제는 인덱스 하나를 공유하고 worker마다 읽기 핸들(`TableReader`)만 둔다.

- **비교 방법:** 기존 구조(`1477e83`)와 새 구조를 같은 컨테이너에서 빌드했다. `run.py --old-binary`로 조건마다 순서를 바꿔 가며 번갈아 측정했다(`scan_write`, worker 1·8, 반복 3회, 12개 조건). 결과는 [minimal/single-vs-replica/](minimal/single-vs-replica)에 있다.
- **같은 날 B1과 직접 비교하지 않는 이유:** 새 구조만 따로 측정한 [minimal/single-index/](minimal/single-index)는 B1보다 처리량이 5~10% 낮았다. 그런데 그 직전에 Docker VM이 재시작되어 환경이 달라졌다. 그래서 같은 환경에서 두 구조를 번갈아 잰 결과로 판단한다.

| W | 구조 | 처리량 RPS 중앙값 [범위] | 스캔 p50 | RSS | 시작 시간 |
|---:|---|---:|---:|---:|---:|
| 1 | 인덱스 복제 | 3.70 [3.47–3.87] | 4,978 ms | 75 MB | 0.21–0.23 s |
| 1 | **단일 인덱스** | 3.67 [3.57–3.77] | 5,013 ms | **41 MB** | **0.10–0.13 s** |
| 8 | 인덱스 복제 | 18.80 [17.33–19.53] | 1,706 ms | 313 MB | 0.82–1.18 s |
| 8 | **단일 인덱스** | 18.27 [17.03–19.17] | 1,721 ms | **41 MB** | **0.10–0.13 s** |

- **처리량:** 두 구조의 범위가 겹친다. 같은 반복 안의 짝끼리 비교하면 worker 8에서 새 구조가 1.6~3% 낮았지만, 반복 사이의 변동(17.0~19.5 RPS)보다 작다. 판정 기준인 "반복 변동을 넘는 저하"에는 해당하지 않는다.
- **메모리:** worker 수와 무관하게 41MB다. worker 8에서 313MB의 13% 수준이다.
- **시작 시간:** worker 수와 무관하게 약 0.1초다. 인덱스를 한 번만 만들기 때문이다.
- **정확성:** 오류는 worker 1의 큐 만료뿐이었고 두 구조가 같은 수준이었다. `WRONG_RESULT`와 응답한 INSERT 유실은 없었다.

### 판단 — 채택

처리량을 유지하면서 메모리와 시작 시간이 worker 수에 비례해 늘던 비용을 없앴다. 재조회 장치와 3단계 잠금 순서도 사라져 구조가 단순해졌다. 운영 서버에 적용했다.

## B5. gate 깨우기 방식 개선 — 2026-11-17

B2의 쓰기 99% 부하에서 INSERT 한 건의 C 실행 시간은 약 14.5µs였지만, 배타 gate 대기는 약 270µs였다. 처리량으로 역산하면 INSERT 사이 간격이 약 76µs라서, 실행 외 약 61µs가 잠금을 다음 스레드로 넘기는 비용이었다. 원인 후보는 gate가 풀릴 때마다 `notify_all`로 기다리는 스레드를 모두 깨우는 방식이었다.

**변경:** [`writer_preferred_gate.hpp`](../../../server/writer_preferred_gate.hpp)
- 대기용 condition variable을 reader용과 writer용으로 나눴다.
- 대기 중인 writer가 있으면 writer 하나만(`notify_one`) 깨우고, 없으면 reader를 모두 깨운다.
- 알림은 mutex를 놓은 뒤에 보낸다.
- 쓰기 우선 정책은 그대로다.

**검증:**
- 기존 gate 순서 테스트, TSan 4개(경고 0건), Release ctest 6/6이 통과했다.
- 스트레스 테스트를 추가했다. writer 8개와 reader 8개가 각 2,000회 진입하며 겹침이나 누락이 없어야 한다.
  - `unlock_shared`의 깨우기를 일부러 빼면 이 테스트가 멈춰 시간 초과로 실패한다. 즉 깨우기 누락을 잡아낸다.

**측정:** 개선 전(`71733e7`)과 같은 컨테이너에서 번갈아 측정했다. worker 8, 각 3회. 결과는 [minimal/wake/compare/](minimal/wake/compare)에 있다.

| 부하 | 지표 | 개선 전 | 개선 후 |
|---|---|---|---|
| write99 | 처리량 RPS | 13,312 / 13,463 / 10,945 | **32,522 / 33,300 / 32,968** (약 2.5배) |
| write99 | write_gate 평균 대기 | 265–323 µs | **56–59 µs** |
| write99 | 읽기 p99 | 123–161 ms | **19.6–20.2 ms** |
| write99 | 쓰기 p99 | 3.2–4.6 ms | **1.7–1.8 ms** |
| write99 | read_gate 평균 대기 | 23.7–29.2 ms | **2.1–2.3 ms** |
| scan_write | 처리량 RPS | 17.2–19.7 | 18.5–19.9 (범위 겹침) |
| scan_write | 스캔 p99 | 2.19–2.54 s | 2.03–2.25 s |

- 정확성 오류와 HTTP 오류는 0건이다.
- 개선 전 r2는 다른 반복보다 처리량이 낮았다(10,945). 이를 빼고 비교해도 차이는 2.4배 이상이다.

### 판단 — 채택

- write99에서 처리량, gate 대기, 읽기·쓰기 p99가 모두 반복 변동을 크게 넘어 좋아졌다.
- 스캔 부하는 반복 변동 안에서 같았다.
- 사전에 정한 채택 기준을 모두 충족해 운영 gate에 적용했다.

### 해석과 남은 점

- **원인 확인:** 병목은 쓰기 실행이 아니라 깨우기 방식이었다.
- **처리량 상한:** 개선 후 처리량 약 3.3만 RPS는 이 부하 생성기가 경합이 적을 때 낸 처리량(약 3만~3.4만 RPS, `closed-v2` worker 1·2)과 비슷하다. 따라서 지금은 서버보다 부하 생성기가 상한일 가능성이 높다.
- **묶음 쓰기(A):** 이 상태에서 묶음 쓰기의 추가 이득은 측정 도구를 바꾸기 전에는 확인할 수 없다.
- **공정성 비율:** 읽기 p99가 쓰기 p99의 약 11배로, 계획 05 규칙 1의 기준(10배)을 아직 조금 넘는다. 하지만 절대값이 120ms대에서 20ms로 줄었다. B3의 공정 gate(읽기 p99 4.4ms, 처리량 8,709)와 비교해도 처리량과 읽기 지연의 절충이 훨씬 낫다.

## 범위 밖 관찰

worker 8의 쓰기 혼합 처리량(약 1.3만 RPS)이 중단된 `closed-v2` 측정의 worker 1·2 값(약 2.9만–3.5만 RPS, 반복 1회)보다 낮다. 가능한 원인으로 다음을 생각할 수 있으나 검증하지 않았다.

- writer 8개의 gate 경합과 깨우기 비용
- 서버와 부하 생성기의 CPU 경쟁

이 관찰은 이번 판단에 쓰지 않았다.

## 실행하지 않은 항목

ID 단건 조회 3종, 쓰기 50%, fallback 전후 비교, 고정 도착률, 스캔 연장, 직렬 서버 W1, 공정성 측정의 W1·2·4는 실행하지 않았다. 이유는 [05 계획의 제외 표](../../plans/phase3-improvements/05-minimal-verification.md#실행하지-않는-항목과-근거)에 있다.

중단된 `closed-v2/`(35개 조건, 반복 1회)는 참고 자료로만 보존하며 이 판단에는 쓰지 않았다.

## 재현

```sh
# 컨테이너 안, 현재 작업 트리를 /source에 복사한 뒤
cmake -S server -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMINIDB_BUILD_BENCHMARKS=ON && cmake --build /build
python3 server/benchmarks/run.py --binary /build/minidb_server --serial-binary /build/minidb_server_serial \
  --workers 2 4 8 --scenarios scan_write --output minimal/scan --source-id <id>
python3 server/benchmarks/run.py --binary /build/minidb_server --workers 1 --scenarios scan_write \
  --output minimal/scan-w1 --source-id <id>
python3 server/benchmarks/run.py --binary /build/minidb_server --workers 8 --scenarios write90 write99 \
  --output minimal/fair --source-id <id>
python3 server/benchmarks/report.py minimal --dataset scan=18 --dataset scan-w1=3 --dataset fair=6
```

집계 파일은 [AGGREGATES.md](minimal/AGGREGATES.md), [classes.csv](minimal/classes.csv), [fairness.csv](minimal/fairness.csv), [medians.json](minimal/medians.json)이다. 조건별 원시 요청 기록 `requests.jsonl.gz`(전체 147MB)는 `.gitignore` 대상이다.
