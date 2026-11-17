# 7. 다중 행 INSERT (C 파서 문법 확장)

작성일: 2026-11-17  
기준 커밋: `47a0859`  
상태: 구현 완료(미커밋). 실행 기록은 문서 끝에 있다.

## 목표

```sql
INSERT INTO users (name, age) VALUES ('민수', 25), ('지영', 30), ('현우', 40);
```

한 문장으로 여러 행을 넣는다. 100만 행을 넣을 때 HTTP 요청·큐 진입·write gate 획득이 행마다 한 번씩이던 것을 묶음마다 한 번으로 줄인다.

이 작업이 **줄이지 않는 것**도 분명히 한다. 행마다 `fprintf` + `fflush`를 하는 저장 경로([storage.c:401](../../../src/storage.c))는 그대로다. 묶음 끝에서 한 번만 flush하는 것(group commit)은 별도 후속 작업이다.

## 결정해야 할 전제

06번은 "기존 C 공개 API, CLI 동작, 기존 C 테스트 유지 + 함수 추가 허용"으로 조건을 완화했다. 이 작업은 한 단계 더 나아가 **공개 헤더의 구조체를 바꾼다.**

| 항목 | 변경 | 유지 |
|---|---|---|
| `Statement` ([parser.h](../../../src/parser.h)) | `name`, `age` 필드를 행 배열로 교체 | SELECT 필드, `parser_next`·`statement_free` 시그니처 |
| SQL 문법 | `VALUES` 뒤에 `( ... )`를 쉼표로 여러 개 허용 | 기존 한 행 문장은 그대로 유효 |
| CLI 출력 | 여러 행이면 `INSERT N` | 한 행이면 지금처럼 `INSERT 1` |
| HTTP 응답 | 필드 추가 | 기존 필드 이름·타입 유지 |
| 기존 테스트 | 새 테스트 추가 | `Statement.name`·`age`를 직접 읽는 단위 테스트만 새 필드로 수정 |

`Statement`를 직접 읽는 코드는 executor, 서버 [engine.cpp](../../../server/engine.cpp)의 값 검사, 단위 테스트뿐이다. 외부 사용자가 없는 내부 헤더라 호환 필드를 남기지 않는다.

과제 요구사항이 Phase 1의 "한 문장에 한 레코드"([spec.md](../../spec.md) 입출력 계약)를 고정 조건으로 본다면 이 계획은 진행하지 않는다.

## 설계

### 1. 문법과 자료구조

```text
INSERT INTO users ( 컬럼 [, 컬럼] ) VALUES ( 값 [, 값] ) [, ( 값 [, 값] )]* ;
```

- 컬럼 목록은 한 번만 쓰고 모든 행에 같은 순서로 적용한다. `(age, name)`이면 모든 행이 `(정수, 문자열)`이다.
- 행마다 값 개수와 타입을 기존과 같은 규칙으로 검사한다. 오류 메시지에 행 번호를 붙인다(예: `3번째 행: age에는 정수 값이 필요합니다`).
- 끝 쉼표(`VALUES (...),;`)와 빈 `VALUES ;`는 문법 오류다.

```c
typedef struct {
    char *name; /* 따옴표를 풀어낸 문자열. Statement가 소유(malloc) */
    int age;
} InsertRow;

typedef struct {
    StmtKind kind;
    InsertRow *rows;   /* INSERT: row_count개. Statement가 소유 */
    size_t row_count;  /* INSERT: 1 이상 */
    WhereKind where;
    int64_t where_value;
} Statement;
```

- 배열은 `realloc`으로 두 배씩 늘린다. `row_count`는 `name` 할당까지 끝난 행만 센다. 그래서 파싱 도중 실패해도 `statement_free`가 채워진 행만 해제하면 된다.
- `parser_next`는 시작할 때 `rows = NULL`, `row_count = 0`으로 초기화한다. 그래야 오류 경로에서 `statement_free`를 여러 번 불러도 안전하다는 기존 계약이 유지된다.

### 2. 행 수 상한: 1,000행

`INSERT_MAX_ROWS = 1000`을 [parser.h](../../../src/parser.h)에 두고, 넘으면 파서 오류로 처리한다.

| 근거 | 내용 |
|---|---|
| gate 점유 시간 | 묶음 전체가 배타 gate 안에서 실행된다. INSERT 한 건의 C 실행이 약 13µs(측정치)이므로 1,000행이면 약 13ms(추정)다. 그동안 모든 SELECT가 기다린다. 10,000행이면 약 130ms가 되어 읽기 p99를 크게 늘린다. |
| 서버 SQL 제한과의 정합 | `max_sql_bytes` 기본값은 64 KiB다. 짧은 행(약 14~30바이트) 1,000개가 이 안에 들어간다. |
| 메모리 | 파서가 행마다 이름을 할당한다. 상한이 없으면 16 MiB SQL 하나로 100만 개 넘게 할당될 수 있다. |

100만 행 적재는 요청 1,000번이 된다. 지금은 100만 번이다.

### 3. 실행과 원자성 (executor)

```text
executor_run(INSERT):
  1. 모든 행을 validate_insert로 검사한다. 하나라도 틀리면 아무것도 쓰지 않고 실패.
  2. 행 순서대로 table_append → on_insert(id) 콜백.
  3. k번째 행에서 저장 실패: 앞의 k-1행은 이미 기록·인덱스 공개됨.
     오류 메시지에 "앞의 k-1행은 기록됨"을 붙여 반환한다.
```

- 값 오류는 쓰기 전에 걸리므로 **전부 실패, 아무것도 기록하지 않음**이 보장된다. 지금의 "검증 후 기록" 계약을 묶음 단위로 넓힌 것이다.
- 디스크 오류처럼 쓰는 도중 실패하면 **부분 기록**이 된다. 이 저장소는 추가 전용이고 rollback이 없다. 실패한 행 자체의 상태(줄 일부가 기록됐는지)는 지금 한 행 INSERT의 `unknown`과 같다. 실패 후 `write_failed`로 테이블 전체가 쓸 수 없게 되는 동작도 그대로다.
- `on_insert`는 지금처럼 행마다 한 번 호출한다. 콜백 계약을 바꾸지 않으므로 호출자가 개수를 센다.
- 묶음 안의 ID는 항상 연속이다. 단일 writer이고, 서버에서는 배타 gate를 문장 전체 동안 잡기 때문이다.

### 4. CLI

- `print_insert`가 매번 출력하지 않고 ctx의 카운터만 올린다. `executor_run`이 성공하면 `INSERT N`을 출력한다. 한 행이면 `INSERT 1`이므로 기존 출력과 같다.
- 부분 기록 실패 시에는 stdout에 아무것도 출력하지 않는다. stderr의 오류 메시지(문장 번호 + "앞의 k행은 기록됨")로 알린다.

### 5. HTTP 서버

| 위치 | 변경 |
|---|---|
| [engine.hpp](../../../server/engine.hpp) `QueryResult` | `inserted_id`는 첫 ID로 의미를 유지한다. `last_inserted_id`, `inserted_count`를 추가한다. |
| [engine.cpp](../../../server/engine.cpp) `record_insert` | 첫 호출에서 첫 ID를 기록하고, 매번 마지막 ID와 개수를 갱신한다. 메모리 할당 없이 정수만 갱신하므로 "commit 후 할당·실패 금지" 계약을 지킨다. `committed`는 "1행 이상 기록됨"을 뜻한다. |
| [engine.cpp](../../../server/engine.cpp) `valid_insert_values` | 모든 행을 검사한다. 422 분류 규칙은 그대로다. |
| [main.cpp](../../../server/main.cpp) 성공 응답 | `inserted_id`(첫 ID, 문자열), `last_inserted_id`(문자열, 추가), `affected_rows`(행 수, 지금은 항상 1)를 넣는다. 한 행이면 `last_inserted_id == inserted_id`, `affected_rows == 1`로 기존 응답의 상위 호환이다. |
| [main.cpp](../../../server/main.cpp) 저장 오류 응답 | 기록된 행이 0개면 지금처럼 `write_outcome: unknown`이다. 1개 이상이면 새 값 `partial`과 함께 `affected_rows`, `inserted_id`, `last_inserted_id`를 넣는다. 지금은 `committed`이면 오류 응답도 `applied`로 표시하는데, 부분 기록을 `applied`로 보이지 않게 고친다. |
| 지표 | `successful_inserts`는 문장 수 그대로 둔다. 행 수 지표는 추가하지 않는다(필요해지면 별도 작업). |

`partial`의 의미: 응답에 적힌 ID 범위의 행은 기록되었다. 그다음 행 하나는 상태를 알 수 없고(기존 `unknown`과 같음), 나머지는 시작하지 않았다.

큐, 만료, gate 정책은 바꾸지 않는다. 묶음 하나가 작업 하나로 큐에 들어가고, 바이트 계산도 SQL 크기 그대로다.

## 작업 순서

| 단계 | 내용 | 수정 파일 |
|---|---|---|
| 1 | `InsertRow`·`Statement` 변경, `parse_values` 반복, 행 수 상한, `statement_free` | `src/parser.h`, `src/parser.c` |
| 2 | 전체 검증 후 기록, 부분 기록 오류 메시지 | `src/executor.c` |
| 3 | 기존 단위 테스트를 새 필드에 맞추고 새 테스트 추가 → `make test` 통과 | `tests/test_unit.c` |
| 4 | CLI `INSERT N` 출력과 CLI 테스트 | `src/main.c`, `tests/cli_test.py` |
| 5 | 서버 결과 필드, 값 검사, 응답 JSON, `partial` | `server/engine.hpp`, `server/engine.cpp`, `server/main.cpp` |
| 6 | 서버 엔진·API 테스트 | `server/tests/engine_test.cpp`, `server/tests/api_test.py` |
| 7 | 문서 갱신 | `docs/spec.md`, `docs/implementation-phase3.md`, `server/README.md`, 이 문서의 실행 기록 |

1~4단계가 끝나면 Phase 1·2만으로도 기능이 완결된다(CLI에서 사용 가능). 5~6단계는 서버 쪽 확장이다.

## 테스트

### C 단위 (`tests/test_unit.c`)

- 3행 파싱: `row_count == 3`, 각 행의 값, 문자열 이스케이프(`'O''Brien'`) 포함.
- 컬럼 순서 `(age, name)`이 모든 행에 적용된다.
- 한 행 문장: `row_count == 1`. 기존 테스트가 그대로 통과하는 것이 곧 하위 호환 확인이다.
- 문법 오류: 끝 쉼표, 빈 `VALUES`, 2번째 행의 값 개수 부족·초과, 2번째 행의 타입 오류. 오류 메시지에 행 번호가 있는지 확인한다.
- 상한: 1,000행은 성공하고 1,001행은 오류.
- executor: 마지막 행의 값이 틀리면 0행 기록(파일 크기와 `last_id` 불변). 성공하면 ID가 연속이고 `on_insert`가 행 순서대로 N번 호출된다.

### CLI (`tests/cli_test.py`)

- 3행 INSERT 뒤 SELECT: 출력이 `INSERT 3` + 3행이고, 파일 내용이 기대와 같다.
- 2번째 행의 값 오류: 실패 종료, stderr에 문장 번호, 파일에 아무것도 기록되지 않음.
- 기존 한 행 테스트의 `INSERT 1` 출력은 수정 없이 통과해야 한다.

### 서버

- `engine_test.cpp`
  - 다중 행 결과: `inserted_id`, `last_inserted_id`, `inserted_count`.
  - **ID 연속성**: 여러 worker가 동시에 묶음 INSERT를 보내도 각 묶음의 ID 범위가 연속이고 서로 겹치지 않는다. 묶음 전체가 gate 하나 안에서 실행된다는 불변식을 확인한다.
  - **부분 기록**: 기존 `RLIMIT_FSIZE` 주입 테스트(`test_write_failure_is_global`)를 응용한다. 첫 행만 들어갈 크기로 제한하고, 결과가 `StorageError` + `committed` + `inserted_count == 1`인지 확인한다. Linux 전용이며 기존 테스트와 같은 조건이다.
- `api_test.py`
  - 다중 행 성공 응답의 세 필드.
  - 한 행 응답이 기존 필드·값과 같다.
  - 1,001행은 400 `INVALID_SQL`.

## 검증 범위 (최소 실행 원칙)

[README의 사용자 정정](README.md#사용자-정정--필수-테스트만-최소-실행)에 따라 변경에 직접 필요한 검사만 실행한다.

| 검사 | 실행 여부 |
|---|---|
| `make test` (C 단위 + CLI) | 실행 |
| 서버 Release 빌드 + `ctest --test-dir build/server --output-on-failure` | 실행 |
| `api_test.py` | 실행 |
| TSan | 실행하지 않는다. gate·pool·인덱스 공유 방식이 바뀌지 않는다. ID 연속성 테스트는 Release에서 기능으로 확인한다. |
| 성능 측정 | 자동으로 실행하지 않는다. 요청하면 같은 행 수(예: 10만)를 한 행씩 넣을 때와 1,000행 묶음으로 넣을 때의 소요 시간을 1회씩 비교한다. 결과는 `docs/phase3-benchmarks/`에 둔다. |

## 하지 않는 것

- 묶음 단위 rollback이나 트랜잭션. 부분 기록은 `partial`로 알리기만 한다.
- 묶음 끝에서 한 번만 flush하는 group commit, `fsync`. 후속 작업 후보다.
- `INSERT ... SELECT`, `DEFAULT VALUES`, 행마다 다른 컬럼 목록.
- HTTP의 별도 대량 적재 엔드포인트. `/query`의 SQL 하나로만 받는다.
- 읽기·쓰기 큐 분리, 클라이언트별 제한, 과부하 벤치마크. 별도 계획 대상이다.

## 시작 전 확인

- 미커밋 변경(`minimal-report.md`, `03-metrics-and-benchmarks.md`의 각 한 줄)을 먼저 커밋할지 버릴지 정한다.
- 작업 브랜치: `feat/phase3-cpp`에서 계속할지, 새 브랜치(예: `feat/multi-row-insert`)를 만들지 정한다.

## 실행 기록

### 2026-11-17 구현 (기준 커밋 `47a0859`, 미커밋 diff)

사용자 결정: 행 수 상한 1,000과 `partial` 응답을 계획대로 진행. 브랜치는 `feat/phase3-cpp`를 그대로 썼다. 이전 작업의 미커밋 문서 두 줄은 손대지 않았다.

| 단계 | 수정 파일 |
|---|---|
| C 파서·executor·CLI | `src/parser.h`, `src/parser.c`, `src/executor.c`, `src/main.c` |
| C 테스트 | `tests/test_unit.c`(기존 필드 사용 수정 + 다중 행 테스트 5개), `tests/cli_test.py`(2개 추가) |
| 서버 | `server/engine.hpp`, `server/engine.cpp`, `server/main.cpp`, `server/c_api.hpp`(주석) |
| 서버 테스트 | `server/tests/engine_test.cpp`(3개 추가), `server/tests/api_test.py`(1개 추가) |
| 문서 | `docs/spec.md`, `docs/implementation.md`, `docs/implementation-phase3.md`, `server/README.md`, `README.md`, 이 문서와 계획 README |

계획과 달라진 점:
- CLI의 `INSERT N`은 `%zu` 대신 `%lu`로 출력한다. MinGW printf가 `%zu`를 지원하지 않을 수 있기 때문이다.
- 서버의 예외 경로(`INTERNAL_ERROR`)도 부분 기록이면 `applied`가 아니라 `partial`로 표시하도록 고쳤다.

### 리뷰 반영 (2026-11-17)

별도 리뷰어(opus) 판정은 APPROVE(CRITICAL·HIGH 0). 아래 지적을 반영했다.

| 지적 | 수정 |
|---|---|
| MEDIUM: 경로가 긴 저장 오류 메시지가 256바이트에서 잘리면 `(앞의 k행은 기록됨)` 꼬리가 사라지고, UTF-8 중간에서 잘리면 서버 메시지가 일반 문구로 바뀜 | 문구를 앞으로 옮김(`앞의 k행은 기록됨: <원래 메시지>`). `db_error_set`이 잘린 끝의 미완성 UTF-8 문자를 지움(`src/common.c`) |
| LOW: 중간 행에서 결과 콜백이 실패하면 기록 행 수가 빠짐 | 여러 행 문장이면 `앞의 k행은 기록됨: 결과 출력에 실패했습니다`. 한 행 메시지는 그대로 |
| LOW: 서버 예외 경로의 `partial` 응답에 범위 필드가 없음 | 범위를 정수로 복사해 두고 예외 응답에도 `inserted_id`·`last_inserted_id`·`affected_rows`를 넣음 |
| LOW: 쉼표 직후 토큰의 lexer 오류에 행 번호가 빠짐 | 그 경로에도 `N번째 행:`을 붙임 |
| LOW: 서버 값 검사에 행 수 상한 없음 | `row_count > INSERT_MAX_ROWS`면 `InvalidValues` |
| 문서: spec 흐름도 `INSERT 1`, README의 64 KiB 제한, `successful_inserts` 단위 | 수정 |

추가 테스트: 잘림 UTF-8 정리, 쉼표 뒤 lexer 오류의 행 번호. 반영 후 같은 Docker 이미지에서 `make test`(단위 30,497건, CLI 30개, 경고 0), 벤치마크 컴파일, 서버 Release 빌드 + `ctest` 6개 suite가 모두 PASS했다. sanitizer는 다시 실행하지 않았다. 결과 콜백 실패 경로와 HTTP `partial` JSON 자체는 여전히 직접 테스트하지 않는다(엔진 테스트로 범위만 확인).

### 검증

환경: Docker 이미지 `mini-dbms-phase3-review:3b19667`(Debian 12, GCC/G++ 12.2.0, CMake 3.25.1), 호스트 WSL2 커널 6.18. Drogon은 이미지에 있는 소스(`-DFETCHCONTENT_SOURCE_DIR_DROGON=/build/_deps/drogon-src`)를 썼다.

| 검사 | 명령 | 결과 |
|---|---|---|
| C 단위 + CLI | `make test` | PASS. 단위 검사 30,491건(기존 30,407건), CLI 30개(기존 28개). 경고 0 |
| 벤치마크 컴파일 | `make build/benchmark` | PASS(실행하지 않음) |
| 서버 Release | `cmake -S server -B build/server -G Ninja -DCMAKE_BUILD_TYPE=Release ...` → `ctest --test-dir build/server --output-on-failure` | PASS. 6개 suite(gate, engine, pool, engine_gate, api, control_api). 프로젝트 코드 경고 0 |
| 새 API 테스트 | `python3 server/tests/api_test.py build/server/minidb_server ApiTest.test_multi_row_insert_response` | PASS |
| ASan/UBSan | `make sanitize` | **환경 문제로 판정 불가.** 이 커널에서 ASan 런타임이 시작 중 `AddressSanitizer:DEADLYSIGNAL`로 무한 반복하는 일이 확률적으로 생긴다. 변경 전 코드도 20회 중 7회, 변경 후 20회 중 3회 발생했다. `setarch -R`은 기본 seccomp에서 거부되어 쓰지 않았다(보안 설정 완화를 기본 조건으로 두지 않는다는 원칙). 단위 테스트의 ASan·누수 검사 실행은 30,491건 통과했고, 시작에 성공한 CLI 실행에서 다중 행 INSERT의 메모리 오류·누수 보고는 없었다. |
| TSan | 실행하지 않음 | gate·pool·인덱스 공유 방식이 바뀌지 않았다. 묶음 ID 연속성은 Release 엔진 테스트로 확인했다. |
| 성능 측정 | 실행하지 않음 | 요청 시 진행 |

### 남은 한계

- 행마다 `fflush`하는 비용은 그대로다. 묶음 단위 flush(group commit)는 후속 작업이다.
- 부분 기록 실패 시 실패한 행은 stdio 버퍼에 남는다. 테이블을 닫을 때(`fclose`) 이 행이 다시 쓰일 수 있다. 기존 한 행 INSERT의 `unknown`과 같은 성질이며, 이번 작업에서 바꾸지 않았다. 단위 테스트는 닫기 전의 파일 상태만 확인한다.
- 행 수 지표(`inserted_rows`)는 추가하지 않았다. `successful_inserts`는 문장 수다.
