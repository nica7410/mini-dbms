# 미니 DBMS — Phase 1~3

C17로 구현한 SQL 엔진과 CLI(Phase 1·2), 그리고 그 엔진을 여러 스레드에서 쓰는 C++17 HTTP API 서버(Phase 3)다. 레코드는 TSV 파일에 저장하고, ID 조회는 메모리 B+ 트리로 처리한다. Phase 3 요약은 [아래 절](#phase-3--c-api-서버)에 있다.

## Phase 1 문서

1. [개발 명세](docs/spec.md): 무엇을 만들고 어떻게 확인하는가
2. [설계와 결정](docs/design.md): 처리 흐름, 저장 방식, 구현 전 결정
3. [작업 목록](docs/tasks.md): 구현 단위와 검증 기준
4. [구현설명서](docs/implementation.md): 전체 흐름에서 INSERT·SELECT 코드 따라가기

지원 SQL은 `INSERT INTO users (name, age) VALUES (...)[, (...)]`(한 문장 최대 1,000행), `SELECT * FROM users [WHERE id = 정수 | WHERE age = 정수]`, `--`·`/* */` 주석이다.

## 빌드와 실행 (Windows, TDM-GCC)

```text
mingw32-make                 # build/minidb.exe
mingw32-make test            # 단위 테스트 + CLI 통합 테스트
build\minidb.exe --data-dir data examples/basic.sql
```

```text
INSERT 1
INSERT 1
id	name	age
1	민수	25
2	O'Brien	30
```

메모리 검사(ASan/UBSan/누수)는 Docker Desktop을 켠 뒤 Linux 컨테이너에서 실행한다.

```text
docker run --rm -v "%cd%:/src" -w /src gcc:13 make sanitize
```

## B+ 트리 인덱스

[Phase 2 구현설명서](docs/implementation-phase2.md)

- 테이블을 열 때 `users.tsv` 전체를 한 번 읽어 레코드를 검증하고, 리프에 `ID → 레코드 시작 바이트 위치`를 등록한다. TSV는 Phase 1과 같은 형식을 사용한다.
- INSERT는 최대 ID에 1을 더한다. 인덱스 공간을 먼저 확보하고 TSV 쓰기가 성공하면 새 ID와 위치를 등록한다. 증가 순서로 들어오는 ID는 항상 가장 오른쪽 리프에 추가된다.
- 리프가 가득 차면 둘로 나누고, 오른쪽 리프의 첫 키를 부모의 경계 키로 올린다. 내부 노드도 가득 차면 분할되며, 루트가 분할되면 트리의 높이가 한 단계 늘어난다.
- `WHERE id = ...`는 경계 키로 트리를 내려가 리프의 위치를 찾은 뒤 해당 행만 읽는다. ID가 없으면 데이터 파일을 읽을 필요가 없다. `WHERE age = ...`와 전체 조회는 TSV를 순차 탐색한다.
- 테이블은 단일 프로세스에서 사용한다. 열린 테이블의 파일을 외부에서 수정하면 인덱스의 바이트 위치가 유효하지 않을 수 있다.

## 100만 건 성능 비교

Windows에서 `mingw32-make bench`, Linux에서 `make bench`를 실행한다. 매번 임시 데이터 디렉터리에서 기존 SQL 파서와 실행기로 INSERT 100만 건을 실행한다. 테이블을 다시 연 뒤 두 SELECT가 같은 한 행을 반환하는지 검증한다.

| 항목 | 측정값 |
|---|---:|
| SQL INSERT 1,000,000건 | 7.748초 |
| 테이블 재열기 및 인덱스 재구축 | 0.313초 |
| `WHERE id = 500000` B+ 트리, 100,000회 평균 | 2.454 µs |
| `WHERE age = 150` 순차 탐색, 10회 평균 | 303.132 ms |

Windows 10 Pro, Intel Core i7-10875H, LLVM-MinGW Clang 23.1.5, `-O2`에서 측정했다. 조회 시간은 각 쿼리를 한 번 예열한 후 같은 테이블에서 반복한 평균이다. 쿼리 파싱, CLI 출력, 테이블 재열기 시간은 조회 측정에서 제외했다. CLI를 매번 새로 실행하면 인덱스 재구축 시간도 포함된다.

이 수치는 Phase 3에서 줄 읽기를 `getc`에서 잠금 없는 `getc_unlocked`/`_getc_nolock`으로 바꾸기 전의 측정이다. 바꾼 뒤 Windows에서는 다시 재지 않았다. Linux 컨테이너에서는 단일 스레드 CLI의 순차 탐색이 약 65ms에서 약 47ms로 줄었다.

## 코드 구성

| 파일 | 역할 |
|---|---|
| src/main.c | CLI: 인자·SQL 파일 읽기, 문장 순차 실행, 결과·오류 출력, 종료 코드 |
| src/parser.c | Lexer + 재귀 하강 파서: SQL 텍스트 → Statement |
| src/executor.c | 값 검증, ID 인덱스 조회와 순차 탐색 선택, 결과 콜백 전달 |
| src/storage.c | TSV 검증·읽기·추가, 다음 ID와 인덱스 관리. Phase 3에서 인덱스를 공유하는 읽기 핸들(`TableReader`) 추가 |
| src/bptree.c | 리프·내부 노드 분할과 ID 조회 |
| tests/test_unit.c | Parser·Storage·Executor 단위 테스트, 노드 분할과 재구축 검증 |
| tests/cli_test.py | 실제 프로세스를 실행하는 CLI 통합 테스트 |
| tests/benchmark.c | SQL INSERT 100만 건과 ID·age 조회 성능 측정 |

## Phase 3 — C++ API 서버

기존 C17 엔진과 CLI를 유지하고 C++17·Drogon API 서버와 고정 크기 Thread Pool을 추가했다. HTTP 요청은 Drogon I/O 스레드가 받고, SQL은 크기가 제한된 큐를 거쳐 고정 수의 SQL worker가 실행한다.

### 동시성 구조

```
SELECT: worker i → reader mutex → 공유 gate → TableReader[i](자기 FILE*) + 공유 B+ 트리
INSERT:                         배타 gate → Table(쓰기 FILE*, 공유 B+ 트리 갱신)
```

- **B+ 트리는 하나를 모든 worker가 공유한다.** 공유 gate 아래에서만 읽고, INSERT의 배타 gate 아래에서만 바뀐다. 그래서 INSERT 직후 어느 worker에서 조회해도 새 행이 바로 보인다.
- **파일 핸들은 worker마다 따로 연다.** 같은 `FILE*`를 두 스레드가 동시에 쓰지 않으므로 SELECT끼리는 병렬로 실행된다.
- **gate는 쓰기 우선이다.** 대기 중인 writer가 있으면 새 reader는 기다린다. 풀릴 때는 대기 writer 하나만 깨운다.
- C 엔진에는 이 구조에 필요한 읽기 함수(`TableReader`, `executor_run_read`)만 추가했다. 기존 공개 함수, CLI 동작과 테스트는 그대로다.

### 측정 결과

100만 행, Linux 컨테이너(CPU 8개)에서 측정했다. 부하 생성기는 서버와 같은 컨테이너에서 연결 32개로 돌렸고, 각 조건을 3회 반복했다.

| 부하 | worker 1 | worker 8 |
|---|---:|---:|
| 스캔 90% + INSERT 10% 처리량 | 20.5 RPS | 약 100 RPS |
| 위 부하에서 INSERT p99 (큐 분리 전) | 1.7 s | 0.35–0.40 s |
| INSERT 99% + ID 조회 1% 처리량 | — | 약 3.3만 RPS (큐 분리 전) |
| 위 부하에서 읽기 p99 / 쓰기 p99 (큐 분리 전) | — | 약 20 ms / 약 1.7 ms |
| 서버 메모리(RSS) | 41 MB | 41 MB |
| 시작 시간(인덱스 구축 포함) | 약 0.1 s | 약 0.1 s |

- 직렬 비교 서버(DB 실행을 한 번에 하나만)는 worker 수와 관계없이 스캔 부하 처리량이 늘지 않았다.
- 읽기·쓰기 큐 분리(2026-11-17): INSERT는 쓰기 큐(쓰기 worker 기본 2개), 나머지는 읽기 큐로 간다. worker 8, 같은 컨테이너에서 변경 전후를 번갈아 쟀다([계획 09](docs/plans/phase3-improvements/09-read-write-queues.md#11-14단계-결과-2026-11-17)).
  - 쓰기 99%: 처리량 약 3.1만 → 3.9만 RPS, 읽기 p99 약 18 → 1.4ms, 쓰기 p99 약 1.9 → 1.4ms
  - 스캔 혼합: INSERT p99 약 0.37 → 0.095초. 스캔 지연은 약 5% 늘었다
  - 100행 INSERT 과부하: 읽기 p99 약 25 → 1.8ms, SELECT의 `QUEUE_FULL` 0건
- 알려진 한계:
  - 긴 스캔이 실행되는 동안 INSERT는 실행 중인 스캔이 끝날 때까지 기다린다(큐에 쌓인 스캔은 기다리지 않는다).
  - INSERT는 `fflush`까지만 하며 `fsync`는 하지 않는다.
- 측정 과정과 채택·기각한 개선(공정 gate, 단일 인덱스 전환, 깨우기 방식, 읽기 잠금 제거)은 [측정 보고서](docs/phase3-benchmarks/20261116/minimal-report.md)에 있다.

### 문서

- [빌드·실행·HTTP API·제약](server/README.md)
- [구현 설명과 동시성 설계](docs/implementation-phase3.md)
- [측정 보고서](docs/phase3-benchmarks/20261116/minimal-report.md), [개선 작업 계획과 기록](docs/plans/phase3-improvements/README.md)

Linux/WSL에서 필요한 개발 패키지를 설치한 뒤 저장소 루트에서 실행한다. 의존성 설치와 Docker 실행은 서버 README에 있다.

```bash
cmake -S server -B build/server -DCMAKE_BUILD_TYPE=Release
cmake --build build/server --parallel 2
ctest --test-dir build/server --output-on-failure
./build/server/minidb_server --data-dir data-api --port 8080
```

같은 데이터 디렉터리에 기존 CLI와 서버를 동시에 실행하지 않는다. 기존 `make test`와 Phase 2 벤치마크 명령은 그대로 사용할 수 있다.
