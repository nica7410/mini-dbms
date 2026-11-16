# 6. 단일 인덱스 구조로 전환 (worker별 파일 핸들)

작성일: 2026-11-16  
기준 커밋: `1477e83`  
상태: 완료 — 채택(운영 서버 적용). 실행 기록은 문서 끝에 있다.

## 결정해야 할 전제

이 전환은 "Phase 1·2 C 원본을 수정하지 않는다"는 조건을 다음과 같이 완화한다.

> 기존 C 공개 API, CLI 동작, 기존 C 테스트를 그대로 유지하는 범위에서 함수를 추가하는 변경은 허용한다.

과제 요구사항이 C 코드 무수정을 엄격하게 요구한다면 이 계획은 진행하지 않는다.

## 왜 바꾸는가

현재 구조는 worker마다 C `Table`을 하나씩 따로 연다. `Table`의 파일 핸들(`FILE *fp`)이 하나뿐이어서, 여러 스레드가 동시에 seek/read를 하면 위치가 꼬이기 때문이다. 그 결과 인덱스까지 worker 수만큼 복제되고, 다음 비용이 생겼다.

| 비용 | 측정값·내용 |
|---|---|
| 메모리 | worker 1→8에서 RSS 75→313MB (`minimal-report` B1) |
| 시작 시간 | 0.21→0.95초. worker마다 파일 전체를 읽어 인덱스를 다시 만든다 |
| 오래된 인덱스 | 조회 인덱스가 시작 시점 스냅샷이라 재조회 장치가 필요하다. `first/last_insert_id`, writer 재조회 경로, `writer_mutex`, 재조회 지표 2종과 잠금 지표 2종, 관련 테스트가 모두 이 문제 때문에 존재한다 |
| 잠금 순서 | reader mutex → gate → writer mutex의 3단계 |

인덱스는 공유해도 안전하다.
- 조회 함수 `bptree_find`는 `const`이며 트리를 읽기만 한다([bptree.c:123](../../../src/bptree.c)).
- 인덱스를 바꾸는 `bptree_append`는 INSERT 안에서만 호출되고, INSERT는 배타 gate 아래에서만 실행된다.

따라서 **인덱스 하나를 공유하고 파일 핸들만 worker마다 두면**, 병렬 조회를 유지하면서 위 비용을 모두 없앨 수 있다.

## 목표 구조

```
현재  SELECT: reader_mutex[i] → 공유 gate → 조회 Table[i](자기 핸들 + 인덱스 복사본)
               └ ID 없음 & [first, last] 범위 → writer_mutex → 쓰기 Table 재조회
      INSERT: 배타 gate → writer_mutex → 쓰기 Table

전환  SELECT: reader_mutex[i] → 공유 gate → Table(공유 인덱스) + TableReader[i](자기 FILE*)
      INSERT: 배타 gate → Table
```

- `reader_mutex`는 남긴다. `worker_index` 계약 위반에 대한 방어 장치이고 경합이 없으면 비용이 거의 없다.
- 공유 gate(쓰기 우선)의 정책은 바꾸지 않는다.
- 불변 조건은 하나다. **공유 인덱스는 공유 gate 아래에서만 읽고, 배타 gate 아래에서만 바뀐다.**

## 단계

### 1. C 엔진에 읽기 핸들 추가 (약 15분)

`src/storage.h/.c`
- 새 불투명 타입 `TableReader`(`Table *` + 자기 `FILE *`, `"rb"`로 열기)를 추가한다.
- 새 함수:
  - `table_reader_open(Table *t, TableReader **out, DbError *err)`
  - `table_reader_close(TableReader *r)`
  - `table_reader_scan(TableReader *r, RowFn fn, void *ctx, DbError *err)`
  - `table_reader_find_id(TableReader *r, int64_t id, RowFn fn, void *ctx, DbError *err)`
- 내부의 `scan_rows`와 `table_find_id` 본문은 읽을 `FILE *`를 인자로 받도록 바꾼다. 기존 `table_scan`과 `table_find_id`는 `t->fp`를 넘기는 얇은 wrapper가 된다. 검증 규칙(ID 증가, 손상 감지)과 `write_failed` 확인을 복제하지 않고 같은 코드를 쓴다.

`src/executor.h/.c`
- `executor_run_read(TableReader *r, const Statement *st, const ResultSink *sink, DbError *err)`를 추가한다. SELECT만 받고, INSERT가 들어오면 오류를 반환한다. 기존 `executor_run`은 변경하지 않는다.

`tests/test_unit.c`에 추가할 검사:
- reader를 연 뒤 `table_append`로 추가한 행이 `table_reader_find_id`와 `table_reader_scan`에 바로 보인다(공유 인덱스 + `fseek`로 버퍼 무효화).
- reader 스캔 결과가 `table_scan` 결과와 같다.
- 없는 ID는 0행을 반환한다.
- `write_failed` 상태에서는 reader 함수도 실패한다.
- `executor_run_read`에 INSERT를 넘기면 거부한다.

완료 기준: `make test`(기존 30,407건 + 신규)와 `make sanitize`(ASan) 통과. 기존 CLI 28개 통과.

### 2. C++ 엔진 정리 (약 15분)

`server/engine.cpp`
- 쓰기 `Table` 하나만 열고, worker마다 `TableReader`를 연다. 조회용 `Table` 복제를 없앤다.
- SELECT는 `executor_run_read`로 실행한다. 재조회 분기 전체를 삭제한다. 삭제 대상은 `may_be_inserted_since_startup`, `first/last_insert_id`, `writer_mutex`다.
- INSERT의 `writer_mutex` 획득을 삭제한다. 배타 gate만으로 충분하다.
- 저장 실패 확인용 probe(`table_find_id(writer, 0, …)`)는 유지한다.

`server/engine.hpp`, `server/main.cpp`
- `LockKind`에서 `InsertWriterMutex`와 `FallbackMutex`를 삭제한다.
- `EngineStats`와 `/metrics`에서 `fallbacks`, `fallbacks_skipped`를 삭제한다.
- `/metrics`의 해당 필드도 제거한다. 외부 소비자는 이 저장소의 테스트와 `report.py`뿐이다.

### 3. 테스트 갱신 (약 10분)

- `engine_test.cpp`
  - 재조회 관련 테스트(`test_snapshot_fallback_and_eof`의 재조회 횟수 검사, `test_fallback_skips_ids_present_at_startup`)를 삭제한다.
  - "INSERT 직후 새 ID를 다른 worker에서 바로 조회", "스캔이 새 행을 봄", "없는 ID는 0행" 검사는 유지하거나 옮긴다.
- `api_test.py`: 재조회 카운터를 검사하는 부분을 삭제한다. 재시작 후 조회 정확성 검사는 유지한다.
- `engine_gate_test.cpp`, `api_executor_control.cpp`, `CMakeLists.txt`: SELECT 경로가 `executor_run_read`로 바뀌므로 `--wrap=executor_run_read`를 추가하고 같은 제어 지점을 둔다.
- `gate_test.cpp`: 변경하지 않는다.
- `server/benchmarks/report.py`: 없는 ID의 재조회 카운터 검사와 `old`/`missing_future` 분기를 삭제한다.

### 4. 문서 (약 10분)

- `docs/implementation-phase3.md`: 동시성 설계 절을 새 구조로 다시 쓴다. "C 원본 무수정" 서술을 "공개 API 유지, 읽기 함수 추가"로 바꾸고, 공유 인덱스 불변 조건과 잠금 순서를 적는다.
- `server/README.md`와 `server/benchmarks/README.md`: 재조회 설명과 `--old-binary` 사용법을 정리한다.
- 계획 01(재조회 범위 제한)은 "06으로 대체됨"으로 표시한다. 계획 README와 이 문서에 실행 기록을 남긴다.

### 5. 검증 (약 10분, Docker)

| 검증 | 기준 |
|---|---|
| `make test`, `make sanitize` | C 기존 + 신규 통과 |
| Release `ctest` | 6개 suite 통과 |
| TSan | gate·엔진·엔진 gate 통합·풀 4개 통과, 경고 0건 |

### 6. 효과 측정 (약 7분)

`minimal-report` B1과 같은 조건으로 `scan_write`를 병렬 서버만, worker 1·8, 반복 3회 측정한다(6개 조건). 다음을 비교한다.

| 지표 | 기대 |
|---|---|
| RSS | worker 수와 거의 무관해야 한다(현재 worker 8: 313MB) |
| 시작 시간 | worker 수와 거의 무관해야 한다(현재 worker 8: 0.95초) |
| 처리량 | 현재(worker 1: 3.7, worker 8: 18.1 RPS)와 반복 변동 범위 안에서 같아야 한다 |

처리량이 반복 변동을 넘어 떨어지면 원인을 기록하고 전환 채택 여부를 다시 판단한다.

## 예상 소요와 변경 규모

- 합계 약 65분.
- C: 약 50~70줄 추가. 기존 함수의 동작은 그대로다.
- C++: 순감소가 예상된다(재조회·`writer_mutex`·지표 2종 삭제).
- 테스트·문서: 재조회 관련 항목은 삭제하고, 새 구조의 서술을 추가한다.
- 커밋은 C 변경과 서버 전환을 하나로 묶는다. 중간 상태에서 C++ 서버가 새 C API를 쓰지 않는 시점이 없도록 하기 위해서다.

## 위험과 대응

| 위험 | 대응 |
|---|---|
| 공유 인덱스를 gate 밖에서 읽는 코드가 생김 | 엔진에서 SELECT 경로는 공유 gate 획득 뒤에만 `executor_run_read`를 호출한다. TSan 엔진 테스트로 확인한다 |
| reader 핸들이 새로 추가된 행을 못 봄 | writer는 `fflush`하고, reader는 읽기 전 `fseek`로 버퍼와 EOF 상태를 초기화한다. 현재 조회 `Table`도 같은 메커니즘으로 새 행을 스캔한다. C 단위 테스트로 직접 검증한다 |
| link-wrap 테스트의 제어 지점이 바뀜 | 두 테스트에 `executor_run_read` wrap을 추가한다 |
| `/metrics` 필드 제거가 외부 사용자에 영향 | 저장소 밖 소비자는 없다. 문서에 변경을 기록한다 |
| 쓰기 우선 gate의 읽기 지연 한계 | 이 전환과 무관하며 그대로 남는다(`minimal-report` B2·B3) |

## 하지 않는 것

- gate 정책 변경
- 인덱스 구조(B+ 트리) 변경
- 요청마다 파일을 새로 여는 방식. 핸들은 worker마다 시작할 때 한 번 연다.
- 전체 벤치마크 matrix 재실행

## 실행 기록 — 2026-11-16 완료

- **C:**
  - `TableReader` API 4개와 `executor_run_read`를 추가했다. `scan_rows`와 `find_id_rows`는 읽을 `FILE *`를 인자로 받는다. 기존 `table_scan`과 `table_find_id`는 wrapper가 됐다.
  - `tests/test_unit.c`에 reader 테스트 3개를 추가했다(쓰기 실패 테스트는 POSIX에서만 실행).
  - `make test`: 30,442건과 CLI 28개 통과(기존 30,407건 + 신규 35건).
  - `setarch -R make sanitize`: ASan·UBSan·LeakSanitizer 통과. ASLR을 끄지 않으면 이 Docker 환경에서는 `DEADLYSIGNAL`로 시작하지 못한다.
- **C++:**
  - 조회용 `Table` 복제, 재조회 분기, `first/last_insert_id`, `writer_mutex`, `fallbacks`·`fallbacks_skipped`, `insert_writer_mutex`·`fallback_mutex` 지표를 삭제했다.
  - `/metrics`의 `lock_wait`는 3종이다.
- **테스트:**
  - 재조회 카운터 검사를 삭제하고 "새 행이 즉시 보임", "ID 공백", "INT64 경계" 검사는 유지했다.
  - link-wrap 테스트 2개는 SELECT 제어 지점을 `executor_run_read`로 옮겼다.
- **검증:** TSan 4개(경고 0건), Release ctest 6/6 통과.
- **측정:** 기존 구조와 번갈아 측정했다(scan_write, worker 1·8, 3회).
  - 처리량: 범위가 겹친다.
  - RSS: worker 8에서 313 → 41MB
  - 시작 시간: worker 8에서 0.82~1.18 → 0.10~0.13초
  - 결과는 [minimal-report B4](../../phase3-benchmarks/20261116/minimal-report.md#b4-단일-인덱스-구조-전환--인덱스-복제-대비)에 있다.
- **계획과 다른 점:** 새 구조만 따로 측정했을 때는 처리량이 B1보다 5~10% 낮게 나왔다. 측정 직전 Docker VM이 재시작되어 환경이 달라진 것으로 판단하고, 같은 환경에서 기존 구조와 번갈아 측정하는 비교를 추가했다(12개 조건).
