# 2. 동시성 순서와 서버 종료 연결 검증

상태: 구현·Release 검증 완료(2026-11-16, 미커밋)  
기존 논의: 1번  
선행 작업: [fallback 범위 제한](01-fallback-range.md)  
다음 작업: [지표와 성능 비교](03-metrics-and-benchmarks.md)

## 문제 범위

기존 `test_concurrent_shutdown_drains_and_joins`는 풀 자체의 drain/join을 이미 확정적으로 검증한다. 이 테스트를 대체하거나 같은 내용을 다시 대규모로 구현하지 않는다.

이번 공백은 다음과 같이 구분한다.

1. 가장 우선: reader A 실행 → writer 대기 등록 → 후속 reader B 차단 → writer 실행 → B 실행이라는 gate 순서.
2. 엔진 통합: 실제 스캔 실행 동안 공유 gate가 유지되고 INSERT가 실행되지 않는지.
3. 서버 통합: 대기 작업이 남아 있는 동안 SIGTERM → 신규 요청 차단 → pool drain/join → app 종료가 연결되는지.
4. 낮은 우선순위: 파일 처리 속도에 의존하는 1ms 큐 만료 테스트 안정화.

`accepted > completed` 또는 writer가 `lock()` 직전에 설정한 플래그만으로 대기 등록을 확정하지 않는다. 확인과 실제 처리 사이의 경쟁 조건을 남긴 채 '확정적 검증'이라고 기록하지 않는다.

## A. gate를 독립 내부 모듈로 분리

- `WriterPreferredGate`를 `server/writer_preferred_gate.hpp`의 내부 namespace로 이동한다. 기존 잠금 정책과 획득 순서는 유지한다.
- `server/tests/gate_test.cpp`와 CTest 대상을 추가한다.
- 테스트에서 gate의 private mutex 아래 `waiting_writers_`를 읽는 friend test accessor를 사용한다. accessor 구현은 테스트 파일에만 둔다. 운영 바이너리에 대기 제어용 callback, 환경변수, HTTP 경로를 추가하지 않는다.
- shared 진입 조건을 공통 내부 함수로 모으고 표준 Lockable 형태의 `try_lock_shared()`를 제공한다. blocking/nonblocking 경로가 같은 진입 조건을 사용하게 해 조건 분기를 서로 다르게 구현하지 않는다.

### 확정적인 순서 검증

1. A가 공유 잠금을 보유한다.
2. writer를 시작하고 test accessor로 `waiting_writers_ > 0`인 실제 등록 상태를 확인한다. A가 잠금을 유지하므로 writer는 이 상태에서 실행될 수 없다.
3. 이 상태에서 B의 `try_lock_shared()`가 실패함을 확인한다. writer가 아직 실행 중이 아니라 '대기 중'이라는 것이 핵심이다.
4. B의 blocking 공유 잠금 획득을 시작한다. A를 해제한 후 writer가 먼저 획득하고, 테스트 latch로 배타 잠금을 유지한다.
5. writer가 잠금을 보유하는 동안 B가 진입하지 못함을 확인하고 writer를 해제한다. 이후 B가 진입하는 순서를 기록한다.
6. writer가 없으면 복수 reader가 함께 진입하는 경우, 모든 reader 종료 전 writer가 진입하지 못하는 경우도 검증한다.

writer 대기 등록의 관찰은 mutex로 동기화하고, 관찰 루프에는 실패 시 종료할 deadline을 둔다. sleep 길이를 순서 증거로 삼지 않는다. 모든 실패 경로에서 latch 해제와 thread join을 보장한다.

## B. 엔진이 실제 C 호출 전체를 보호하는지 검증

gate 단위 테스트만으로 `Engine`의 잠금 사용까지 검증됐다고 판단하지 않는다.

- Linux/GNU 호환 linker의 `--wrap=executor_run`을 사용하는 별도 엔진 통합 테스트를 추가한다.
- 테스트 wrapper는 선택한 SELECT의 C 호출 진입을 알린 뒤 latch에서 대기하고, 해제되면 `__real_executor_run`을 호출한다. INSERT 진입도 기록한다.
- 테스트 타깃에만 읽기 전용 Engine test accessor를 제공하여 실제 사용 중인 gate의 상태를 같은 mutex 아래 관찰한다. private Impl에 대한 friend bridge는 테스트 빌드에서만 컴파일하고 운영 타깃에는 포함하지 않는다. 대기 상태를 강제로 변경하거나 실행을 멈추는 API는 추가하지 않는다.
- 실제 `Engine::execute()` 경로로 스캔을 시작한다. wrapper가 붙잡은 시점에 공유 잠금 보유를 확인하고, writer 요청을 겹친 후 실제 writer 대기 등록을 확인한다. C 호출 직전 잠금이 해제되는 회귀는 이 상태 검사에서 실패해야 한다. writer 호출 직전의 플래그만으로 대기를 추정하지 않는다.
- wrapper는 테스트 타깃에만 링크한다. 원래 `src/executor.c`, 운영 engine 경로에 테스트 전용 정지 코드를 넣지 않는다.
- 스캔 결과는 쓰기 이전 상태, INSERT 이후 재조회는 쓰기 이후 상태인지 확인한다. gate의 writer 대기 등록 순서 자체는 A의 단위 테스트가 담당한다.

## C. SIGTERM 연결과 큐 만료 API 검증

동일한 link wrapper 방식을 별도 `minidb_server_test` 타깃에만 적용한다. 운영 `minidb_server`는 테스트용 IPC나 환경변수를 해석하지 않는다. 테스트 전용 wrapper와 Python harness는 pipe 등의 handshake로 C 호출 진입/해제를 주고받는다.

### SIGTERM 시나리오

1. worker 1개로 실행한다. 첫 SELECT가 wrapper에 진입했다는 통지를 받고 붙잡아 둔다.
2. INSERT들을 제출하고 `active=1`, `queued=N`과 수락 수를 확인한다. 첫 작업이 붙잡혀 있어 큐가 비워질 수 없도록 한다.
3. SIGTERM을 보낸다. 첫 작업을 계속 붙잡은 채 HTTP `/health` 또는 신규 `/query`로 종료 상태/신규 작업 거절을 확인한다. 큐 만료 시간은 이 handshake의 최대 테스트 시간보다 충분히 길게 설정한다.
4. 첫 작업을 해제한다. 수락한 INSERT가 저장되고 정상 종료하는지 확인한다. 최종 소켓 응답 전달은 기존 계약처럼 별도 보장하지 않는다.
5. 재시작하여 저장된 ID와 행을 확인한다. 실패·timeout에서도 프로세스와 IPC를 정리한다.

### 큐 만료 시나리오

첫 작업을 붙잡은 상태에서 후속 작업의 큐 진입을 확인한다. monotonic 시간으로 설정한 만료 기간을 넘긴 뒤 첫 작업을 해제한다. 후속 작업이 `QUEUE_TIMEOUT`/`not_started`로 끝나고 저장 부작용이 없는지 검증한다. 오래 걸리는 스캔을 가정하거나 1ms의 OS 스케줄링 차이에 의존하지 않는다.

## 변경 파일과 플랫폼 범위

- `server/engine.cpp`, 신규 gate 헤더, 신규 gate/엔진 통합 테스트.
- `server/tests/api_test.py`, 신규 테스트 전용 wrapper/harness, `server/CMakeLists.txt`.
- `server/README.md`, `docs/implementation-phase3.md`의 검증 범위와 실행 명령.
- gate 단위 테스트는 C++17 범용으로 유지한다. link wrapper·POSIX SIGTERM 테스트는 지원 Linux 빌드에 한정하고, 비지원 플랫폼은 명시적 skip으로 보고한다.
- 테스트 타깃은 `BUILD_TESTING` 아래 두며 운영 바이너리에 wrapper 심볼이 링크되지 않는지 확인한다.

## 완료 기준

- 기존 풀 테스트를 유지하면서 A/B/C 시나리오가 통과한다.
- 테스트용 gate 복사본에서 writer 우선 조건을 제거하면 A의 테스트가 실패하는 것을 확인한다.
- 검증용 임시 복사본에서 엔진 잠금 범위를 C 호출 전에 끝내거나 종료 시 drain을 생략하면 해당 통합 테스트가 실패하는지 확인한다. 이 변형은 저장소 코드나 커밋에 남기지 않는다.
- 반복 실행은 시간 의존성 확인용으로 사용하고, 여러 번 통과한 사실을 handshake의 대체 근거로 쓰지 않는다.
- Release 전체 CTest와 기존 `make test` 통과, 지원 환경에서 동시성 sanitizer 결과 기록.
- 정책 변경, fairness 개선, 실행 중 SQL 취소, 종료 시간 상한 도입은 포함하지 않는다.

## 실행 기록 — 2026-11-16

- 별도 gate 헤더와 테스트 accessor, 엔진 link-wrapper 테스트, POSIX pipe 기반 제어 API 테스트를 추가했다. 기존 시간 의존 1ms 만료 테스트는 제어 API 테스트로 대체했고 일반 SIGTERM smoke 테스트는 유지했다.
- Debian bookworm / GCC 12.2 / CMake 3.25.1에서 Release 빌드 및 `ctest --test-dir /build --output-on-failure -V`: 6개 suite 통과. gate 2개 시나리오, 엔진 gate 통합, 일반 API 8개, 제어 API 2개를 포함한다.
- `make test`: 기존 C 검사 30,407건, CLI 28개 통과.
- `nm -C /build/minidb_server`: 운영 실행 파일에 `__wrap_executor_run`, `EngineTestAccess::snapshot` 심볼이 없음을 확인했다.
- 컨테이너 내부 검증 복사본에서 writer 우선 조건 제거 → gate 테스트 실패; C 호출 전 공유 gate 해제 → 엔진 통합 테스트 실패; shutdown 후 대기 작업 미처리 → 제어 API 테스트의 저장 행 검사 실패를 각각 확인했다. 의도적 변형은 작업 디렉터리에 적용하지 않았다.
- 테스트 전용 타깃은 Linux GNU/Clang linker wrapper 조건에서만 생성되며 비지원 플랫폼은 CMake가 skip을 알린다. Windows 실행은 검증하지 않았다.
- 기본 컨테이너에서는 TSan 런타임이 시작하지 못했다. 그래서 [05 최소 검증](05-minimal-verification.md)에서 `seccomp=unconfined`와 `setarch -R` 조건으로 다시 실행했다. gate·엔진·엔진 gate 통합·풀 테스트 4개가 통과했고 TSan 경고는 0건이었다.
- 로컬 snapshot/실행 스크립트/로그: `%TEMP%/mini-dbms-step2-20261116-201028/`. 커밋·푸시하지 않았다.
