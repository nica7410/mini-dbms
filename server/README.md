# Phase 3 — C++ API 서버

기존 C17 SQL Parser·Executor·Storage·B+ 트리를 그대로 링크하는 C++17 서버다. HTTP는 Drogon, SQL 요청은 직접 구현한 고정 크기 Thread Pool로 처리한다. 기존 `src/`와 루트 `Makefile`, Phase 1·2 CLI 실행 방법은 바꾸지 않는다.

- [구현 설명과 동시성 설계](../docs/implementation-phase3.md)
- [실제 검증 환경과 결과](../docs/implementation-phase3.md#10-실제-검증-기록)
- [기존 CLI와 Phase 2](../README.md)

## 빌드와 실행

아래 명령은 Linux/WSL의 Debian·Ubuntu 계열 기준이다. CMake 3.21 이상과 C17/C++17 컴파일러가 필요하다. 기본 빌드는 Drogon v1.9.14의 고정 커밋과 해당 Trantor 서브모듈을 내려받는다. JsonCpp·uuid·zlib는 시스템 개발 패키지를 사용한다.

저장소 루트에서 실행한다.

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build git ca-certificates \
  libjsoncpp-dev uuid-dev zlib1g-dev python3

cmake -S server -B build/server -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/server --parallel 2
ctest --test-dir build/server --output-on-failure

./build/server/minidb_server --data-dir data-api --host 127.0.0.1 --port 8080
```

처음 열 때 데이터 디렉터리와 `users.tsv`를 준비하며, 기존 파일이 있으면 검증하고 인덱스를 재구축한다. 서버가 사용하는 데이터 디렉터리는 서버 한 프로세스가 소유해야 한다. 기존 CLI로 같은 파일을 읽거나 수정하려면 먼저 서버를 종료한다. CLI는 새 서버의 프로세스 잠금에 참여하지 않는다.

설치된 Drogon을 사용하려면 다음과 같이 빌드한다. 이 경로는 버전 1.9.14 이상인 CMake 패키지를 요구하며, 기본 FetchContent 경로의 커밋 고정과는 구별된다.

```bash
cmake -S server -B build/server-installed -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DMINIDB_FETCH_DROGON=OFF
cmake --build build/server-installed --parallel 2
```

이미 준비된 Drogon v1.9.14 소스를 사용하면 `-DFETCHCONTENT_SOURCE_DIR_DROGON=/absolute/path/to/drogon`을 지정할 수 있다. 이 디렉터리에는 Trantor 서브모듈도 있어야 한다. 시스템 개발 패키지는 여전히 필요하다.

### Docker

저장소 루트를 빌드 컨텍스트로 사용한다. 컨테이너는 일반 사용자 UID 10001로 실행하며 `/data`를 데이터 디렉터리로 사용한다.

```bash
docker build -f server/Dockerfile -t mini-dbms .
docker run --rm --name mini-dbms \
  -p 127.0.0.1:8080:8080 -v mini-dbms-data:/data mini-dbms
```

Dockerfile은 HTTP 서버를 Release로 빌드한다. 이미지 빌드 안에서 테스트를 실행하지는 않는다. API에는 인증 기능이 없으므로 위 예시는 호스트의 loopback 주소에 포트를 연결한다. 외부 클라이언트를 허용할 때는 배포 환경에서 접근 범위를 정한다.

## API 사용 예

요청은 UTF-8 JSON이며 정확히 하나의 문자열 필드 `sql`을 갖는다. 한 요청에 SQL 문장 하나만 허용한다. 뒤쪽 공백과 주석은 가능하지만 여러 문장, 잘못된 뒤쪽 문법, NUL 문자는 실행 전에 거절한다.

비어 있는 데이터 디렉터리에서 시작한 예시다.

```bash
curl -sS http://127.0.0.1:8080/query \
  -H 'Content-Type: application/json' --data-binary @- <<'JSON'
{"sql":"INSERT INTO users (name, age) VALUES ('민수', 25);"}
JSON
```

```json
{
  "ok": true,
  "request_id": "1",
  "type": "insert",
  "inserted_id": "1",
  "affected_rows": 1,
  "write_outcome": "applied"
}
```

```bash
curl -sS http://127.0.0.1:8080/query \
  -H 'Content-Type: application/json' \
  --data-binary '{"sql":"SELECT * FROM users WHERE id = 1;"}'
```

```json
{
  "ok": true,
  "request_id": "2",
  "type": "select",
  "rows": [{"id": "1", "name": "민수", "age": 25}],
  "row_count": 1
}
```

`id`와 `inserted_id`는 JSON 문자열이다. 기존 엔진의 64비트 ID를 JavaScript 클라이언트에서 정밀도 손실 없이 전달하기 위한 계약이다. `request_id`도 문자열이며 서버 프로세스 안에서 증가한다. 재시작 사이에 유일한 ID나 중복 실행 방지 키는 아니다.

지원 SQL은 기존 CLI와 같다.

| 문장 | 동작 |
|---|---|
| `INSERT INTO users (name, age) VALUES ('이름', 25);` | 자동 ID를 부여하고 TSV와 writer 인덱스에 추가 |
| `SELECT * FROM users WHERE id = 1;` | B+ 트리를 통한 ID 검색 |
| `SELECT * FROM users WHERE age = 25;` | TSV 순차 탐색 |
| `SELECT * FROM users;` | 전체 순차 탐색, 결과 상한 적용 |

이름은 1~255바이트이며 탭·개행을 포함할 수 없다. 나이는 0~150이다. SQL 문자열의 작은따옴표는 기존 SQL 규칙에 따라 두 번 써서 표현한다. 예: `'O''Brien'`.

## 엔드포인트와 오류

| 경로 | 메서드 | 역할 |
|---|---|---|
| `/query` | POST | SQL 한 문장 실행 |
| `/health` | GET | 사용 가능하면 HTTP 200, 중단 중이거나 엔진이 사용 불가능하면 503 |
| `/metrics` | GET | Thread Pool과 엔진의 JSON 계측값 |

```bash
curl -sS http://127.0.0.1:8080/health
curl -sS http://127.0.0.1:8080/metrics
```

오류 응답은 `ok: false`와 `error.code`, `error.message`를 갖는다. `/query` handler에서 처리한 요청에는 `request_id`도 들어간다. HTTP 프레임워크가 handler 전에 거절한 요청에는 `request_id`가 없을 수 있다.

| HTTP | 주요 오류 코드 | 의미 |
|---:|---|---|
| 400 | `INVALID_REQUEST` | JSON 구조·UTF-8 오류, 중복 JSON 키, 추가 필드 등 |
| 400 | `INVALID_SQL` | 문법 오류, 빈 입력, 다중 문장, NUL 등 |
| 413 | `SQL_TOO_LARGE` 또는 `HTTP_ERROR` | SQL 또는 HTTP 본문 크기 초과 |
| 415 | `CONTENT_TYPE` | `application/json`이 아닌 요청 |
| 422 | `INVALID_VALUES` | 기존 스키마 범위에 맞지 않는 INSERT 값 |
| 422 | `RESULT_TOO_LARGE` | SELECT 행 수·응답 크기 상한 초과 |
| 500 | `STORAGE_ERROR` | 기존 엔진의 저장·읽기 실패 |
| 500 | `INVALID_STORED_TEXT` | 기존 파일의 이름을 UTF-8 JSON으로 반환할 수 없음 |
| 500 | `INTERNAL_ERROR` | 요청 처리 중 내부 실패 |
| 503 | `QUEUE_FULL` | 대기 작업 수 또는 대기 요청 바이트 상한 초과 |
| 503 | `QUEUE_TIMEOUT` | worker가 엔진 실행 전에 확인한 대기 시간 초과 |
| 503 | `SHUTTING_DOWN` | 종료 중이라 신규 작업을 받지 않음 |
| 503 | `DATABASE_UNAVAILABLE` | 저장 상태 확인과 재시작이 필요한 엔진 상태 |

`/query`의 503 오류 응답에는 `Retry-After: 1`이 포함된다. 최대 연결 수나 클라이언트 연결 종료처럼 HTTP 응답 자체를 전달할 수 없는 상황은 별개다.

### INSERT 결과와 재시도

`write_outcome`은 필요한 응답에 다음 값으로 포함된다.

| 값 | 의미 |
|---|---|
| `applied` | C 엔진의 INSERT 성공 콜백을 확인함. 이후 응답 생성·전송 실패로 되돌리지 않음 |
| `not_started` | 큐 수락 실패, 종료 거절, 대기 만료 등으로 SQL을 시작하지 않음 |
| `unknown` | 저장 또는 내부 실패 때문에 이 응답만으로 적용 여부를 확정할 수 없음 |

클라이언트가 응답을 받지 못했다고 INSERT가 실행되지 않은 것은 아니다. 이 API는 idempotency key나 자동 rollback을 제공하지 않으므로 확인 없이 INSERT를 재전송하면 중복 행이 생길 수 있다. `applied`는 `fsync`를 통한 전원 장애 내구성 보장과도 다르다.

## 실행 옵션

```bash
./build/server/minidb_server --help
```

| 옵션 | 기본값 | 의미 |
|---|---:|---|
| `--data-dir` | `data` | DB 파일 디렉터리 |
| `--host` | `127.0.0.1` | 수신 주소 |
| `--port` | `8080` | TCP 포트 |
| `--workers` | `4` | SQL worker와 reader 컨텍스트 수 |
| `--io-threads` | `2` | HTTP event loop 수 |
| `--queue-capacity` | `256` | 대기 중인 SQL 작업 수 상한 |
| `--queue-bytes` | `16777216` | 대기 작업의 원본 JSON 본문 바이트 합 상한 |
| `--max-sql-bytes` | `65536` | 디코딩한 SQL의 UTF-8 바이트 상한 |
| `--max-rows` | `10000` | SELECT 결과 행 수 상한 |
| `--max-result-bytes` | `8388608` | 결과 수집 예산 및 최종 JSON 크기 상한 |
| `--queue-timeout-ms` | `5000` | worker가 엔진 진입 전에 검사하는 대기 만료 기준 |
| `--max-connections` | `1024` | HTTP 연결 수 상한 |

숫자 옵션은 양수다. 포트는 65535 이하, SQL·I/O thread 수는 각각 256 이하, SQL 크기는 16 MiB 이하, 결과 크기는 512바이트 이상, 대기 만료는 1일 이하를 허용한다.

큐 상한은 이미 실행 중인 작업을 제외한다. HTTP 수신 버퍼, JSON 객체, 실행 중인 결과, 응답 전송 버퍼까지 합친 프로세스 전체 메모리 상한은 아니다. SELECT 결과는 보수적인 크기 예산으로 수집하므로 최종 JSON 크기 상한에 도달하기 전에 거절될 수도 있다. 상한을 넘긴 결과를 성공 응답으로 일부만 반환하지 않는다.

HTTP pipelining은 연결당 16개로 제한한다. 프레임워크의 이 한도를 초과하면 연결이 종료될 수 있으며, SQL 큐의 `QUEUE_FULL` JSON 응답과는 별개다.

`--queue-timeout-ms`는 실행 중인 SQL을 중단하는 timeout이 아니다. 엔진 lock 대기와 파일 I/O에는 이 시간 제한이 적용되지 않는다.

## 병렬 처리와 제한

- 서로 다른 reader 컨텍스트의 SELECT는 병렬로 실행할 수 있다. 같은 `FILE*`를 여러 thread가 공유하지 않는다.
- INSERT는 한 writer 컨텍스트에서 직렬화하며, INSERT 실행 중에는 SELECT를 실행하지 않는다.
- reader의 B+ 트리는 서버 시작 시점의 인덱스다. 시작 이후 삽입한 ID나 존재하지 않는 ID는 writer 인덱스에 재조회하며, 이 재조회는 writer mutex를 공유한다.
- worker가 `W`개면 writer를 포함하여 `W + 1`개의 테이블 컨텍스트와 인덱스를 구축한다. worker를 늘리면 초기화 시간과 메모리도 늘어난다.
- 단일 SELECT의 탐색 자체를 여러 thread로 분할하지 않는다. MVCC, 트랜잭션, UPDATE/DELETE, 페이지네이션은 제공하지 않는다.
- TSV 쓰기는 기존 엔진의 `fflush` 계약을 사용한다. WAL·`fsync`·crash recovery를 추가한 것은 아니다.

Thread Pool을 늘리는 것만으로 처리량이 선형 증가하지 않는다. 새 ID 조회의 fallback, 긴 순차 탐색, 메모리 복제, 쓰기 비중과 저장 장치가 성능에 영향을 준다. `peak_active`는 worker 활동 수이고, `engine.peak_executions`는 기존 `executor_run`의 동시 진입 수다. 어느 값도 단독으로 속도 향상을 증명하지 않는다.

## 종료

`Ctrl+C` 또는 `SIGTERM`을 받으면 신규 SQL을 거절하고 수락한 작업을 처리한 뒤 worker를 join한다. 큐에서 만료된 작업은 SQL을 실행하지 않고 만료 응답으로 끝낸다. 이미 시작한 SQL을 강제로 취소하지 않는다.

작업 종료까지 네트워크 event loop를 유지하지만, 클라이언트 연결 종료나 서버 종료 시점까지 포함한 최종 응답 전달을 보장하지 않는다. DB 객체는 worker가 모두 끝난 뒤 해제된다.

## 검증 명령

기존 CLI 회귀 검증은 원래 명령을 그대로 사용한다.

```bash
make test
ctest --test-dir build/server --output-on-failure
```

100만 행을 미리 적재한 TSV에서 병렬 전체 탐색 8건과 동시 INSERT·즉시 조회 64쌍을 검증하려면 다음을 실행한다. 임시 데이터만 사용하며 서버의 계측값도 출력한다. 이 검증은 처리량 비교 벤치마크가 아니다.

```bash
MINIDB_API_TEST_ROWS=1000000 python3 server/tests/api_test.py \
  build/server/minidb_server ApiTest.test_parallel_reads_and_mixed_writes
```

엔진과 Thread Pool만 빌드하면 Drogon과 그 의존성 없이 해당 테스트를 실행할 수 있다.

```bash
cmake -S server -B build/server-core -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DMINIDB_BUILD_HTTP=OFF
cmake --build build/server-core --parallel 2
ctest --test-dir build/server-core --output-on-failure
```

ASan/UBSan과 TSan은 각각 별도 빌드 디렉터리를 사용한다. sanitizer는 이 프로젝트의 C 엔진·C++ 코드에 적용하며 외부 Drogon·Trantor 소스에는 강제로 적용하지 않는다.

```bash
cmake -S server -B build/server-asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DMINIDB_BUILD_HTTP=OFF \
  -DMINIDB_ENABLE_ASAN_UBSAN=ON
cmake --build build/server-asan --parallel 2
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build/server-asan --output-on-failure

cmake -S server -B build/server-tsan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DMINIDB_BUILD_HTTP=OFF \
  -DMINIDB_ENABLE_TSAN=ON
cmake --build build/server-tsan --parallel 2
ctest --test-dir build/server-tsan --output-on-failure
```

TSan 실행에는 해당 runtime이 동작하는 OS·메모리 매핑 환경이 필요하다. HTTP API까지 계측하려면 별도의 sanitizer 빌드에서 `MINIDB_BUILD_HTTP`를 켠다. 기존 Phase 2 벤치마크 수치를 Phase 3 HTTP 서버의 측정값으로 사용하지 않는다.
