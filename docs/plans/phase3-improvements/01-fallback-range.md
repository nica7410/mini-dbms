# 1. 성공 INSERT 범위로 fallback 제한

상태: 구현·Release 검증 완료(2026-11-16, 미커밋). TSan은 [05 최소 검증](05-minimal-verification.md)에서 통과했다.  
기존 논의: 2번  
선행 작업: 없음  
다음 작업: [동시성 검증 보강](02-concurrency-tests.md)

## 해결할 문제

현재 `Engine::Impl::may_be_inserted_since_startup()`은 `first_insert_id` 이상인 miss를 모두 writer에 보낸다. 시작 후 ID 6, 7만 INSERT했어도 ID 8이나 999999 조회가 writer mutex를 거친다. 결과는 정확하지만 아직 존재할 수 없는 행까지 직렬 재조회한다.

## 확정 범위와 동작

`first_insert_id`와 함께 `last_insert_id`를 관리한다. 둘 다 0이면 서버 시작 이후 성공한 INSERT가 없는 상태다. 기존 atomic 표현은 유지하되, 정확성은 두 값을 같은 gate 안에서 읽고 쓰는 것으로 보장한다. 두 atomic 값을 잠금 밖에서 읽어 일관된 쌍이라고 가정하지 않는다.

1. 배타 gate 안에서 `executor_run()`이 성공한 경우에만 범위를 갱신한다.
2. 첫 성공이면 `first_insert_id`를 설정한다. 매 성공마다 `last_insert_id`를 반환된 ID로 갱신한다.
3. SELECT는 기존과 동일하게 공유 gate 안에서 reader를 조회한다.
4. 정상 실행된 ID 조회가 0행일 때만 다음 조건으로 fallback을 결정한다.

```text
first_insert_id != 0
AND first_insert_id <= requested_id
AND requested_id <= last_insert_id
```

범위 밖이면 빈 성공 결과를 반환하고 `fallbacks_skipped`를 증가시킨다. 범위 안이면 기존 writer mutex와 오류 처리를 유지한다. reader hit, 스캔, 콜백 실패, 저장 오류의 처리는 바꾸지 않는다.

범위 내부의 ID는 현재 엔진의 성공한 연속 INSERT에 대응한다. 그렇더라도 범위 판정만으로 행을 만들어 반환하지 않으며 실제 writer 조회와 저장 오류 검사를 유지한다.

## 불변식과 경계

- 원래 파일의 ID에 빈 구간이 있어도 시작 시 reader 인덱스는 파일 전체를 반영한다.
- 잘못된 입력이나 쓰기 전 실패는 성공 범위에 반영하지 않는다. 부분 쓰기 등으로 엔진이 사용 불가가 되면 기존 health 검사로 이후 요청을 거절한다.
- 마지막 ID를 계산할 때 `id + 1` 같은 산술을 추가하지 않고 실제 성공 ID를 저장한다. `INT64_MAX` 경계에서 오버플로를 만들지 않는다.
- 재시작하면 범위를 0으로 초기화한다. 이전 실행에서 삽입한 행은 새 reader 인덱스에 포함된다.
- 외부 파일 수정, ID 재사용, UPDATE/DELETE가 도입되면 이 최적화의 전제를 다시 검토한다.

## 작업 순서

1. `server/engine.cpp`에 마지막 성공 ID와 범위 판정을 추가한다.
2. `server/engine.hpp`의 `fallbacks_skipped` 설명을 범위 밖 miss까지 포함하도록 수정한다. 지표 이름과 기존 API 형식은 유지한다.
3. `server/tests/engine_test.cpp`의 gap fixture 테스트를 확장한다. 기존 ID 8 조회의 기대값을 fallback에서 skip으로 바꾼다.
4. `server/tests/api_test.py`에서 INSERT 이후 큰 ID 조회의 결과와 `/metrics` 변화를 확인한다.
5. `server/README.md`, `docs/implementation-phase3.md`의 재조회 범위·비용·지표 설명을 갱신한다.

## 필수 테스트

| 사례 | 기대 결과 |
|---|---|
| 시작 데이터 ID 1, 2, 5; 아직 INSERT 없음 | gap·미래 ID miss 모두 skip, 기존 행은 reader hit |
| 첫 INSERT ID 6, 다음 INSERT ID 7 | ID 6과 7만 fallback으로 해당 행 반환 |
| ID 0, 음수, gap 3/4, ID 8, 큰 양수 | 빈 성공 결과, skip 증가, fallback 증가 없음 |
| ID 8을 먼저 조회한 뒤 실제로 INSERT | 이전 miss를 캐시하지 않음; 이후 ID 8 조회는 새 행 반환 |
| 첫 INSERT 전 잘못된 값·잘못된 SQL | 범위 변화 없음; 이후 첫 정상 INSERT와 조회 정상 |
| 실제 쓰기 실패 | 기존 global unavailable 정책 유지; skip 경로로 우회하지 않음 |
| 시작 파일 최대 ID가 INT64_MAX-1 | 마지막 가능한 INSERT와 조회 성공; 다음 INSERT 실패 후 범위·기존 조회 보존 |
| 서버 재시작 | 이전 신규 행이 reader hit로 전환; 아직 새 INSERT가 없으면 모든 miss skip |
| 여러 worker의 INSERT와 즉시 조회 | ID 중복·행 누락 없음; 첫/마지막 범위가 경합으로 덮어써지지 않음 |

순차 테스트에서는 fallback/skip 횟수를 정확히 검증한다. 동시 테스트에서는 관찰 순서에 따라 달라지는 miss 횟수를 임의로 고정하지 않는다.

## 검증과 완료 기준

- 수정본 Release 빌드, 엔진·풀·API CTest, 기존 `make test` 통과.
- 위 경계 테스트가 실행되며 신규 ID 조회와 실패 상태 전파에 회귀가 없음.
- 지원 환경의 TSan 결과 또는 실행 불가 이유를 별도로 기록.
- 기존 C 소스 변경 없음, 범위 밖 조회가 writer 재조회 횟수를 늘리지 않음을 확인.
- 처리량 향상 수치는 이 작업에서 주장하지 않는다. 성능 비교는 3번 작업에서 수행한다.

## 제외 범위

reader 인덱스 갱신·공유·재구축, 음성 조회 캐시, gate 정책 변경, 새 timeout/취소 정책, 전반적인 atomic 정리는 하지 않는다.

## 실행 기록 — 2026-11-16

- 기준: `2cccc9b` 위의 미커밋 변경. 검증 snapshot patch SHA-256: `b5fc25b7d0fa984c6641495b6f6a0f87db9e2c3deb8b2456ecee9c6c6e3eeee7` (이 기록 추가 전 코드·테스트·동작 설명 diff).
- 변경: `server/engine.cpp`, `engine.hpp`, `tests/engine_test.cpp`, `tests/api_test.py`, `server/README.md`, `docs/implementation-phase3.md`.
- 환경: Docker Desktop Linux 컨테이너, Debian bookworm, GCC/G++ 12.2, CMake 3.25.1, Drogon 1.9.14. 기존 검토 이미지 `mini-dbms-phase3-review:3b19667`의 소스에 현재 snapshot을 복사한 뒤 재빌드했다. 테스트는 원본 작업 디렉터리에 쓰지 않았다.
- `cmake --build /build --parallel 4` 및 `ctest --test-dir /build --output-on-failure -V`: 3개 suite 통과. 엔진 8개 시나리오, 풀 7개, HTTP 9개. 엔진 동시 실행 관찰값 4.
- `make test`: C 검사 30,407건, CLI 28개 통과. 기존 C 소스·Makefile·기존 tests 변경 없음.
- `cmake -S /source/server -B /tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMINIDB_BUILD_HTTP=OFF -DMINIDB_ENABLE_TSAN=ON`, `cmake --build /tsan --parallel 4`: 성공.
- `ctest --test-dir /tsan --output-on-failure -V`: 엔진·풀 모두 `FATAL: ThreadSanitizer: unexpected memory mapping`으로 0.01초 만에 종료. 테스트 본문이 실행되지 않았으므로 race 없음으로 판정하지 않는다. 컨테이너 보안/ASLR 설정은 변경하지 않았다.
- 로컬 로그: `%TEMP%/mini-dbms-step1-20261116-200420/release.log`, `tsan.log`. 같은 폴더에 snapshot patch와 실행용 `source/verify.py`를 보관했다.
- 추가 검증: diff 공백 검사 통과. HTTP 처리량 개선은 측정하지 않았다. 커밋·푸시하지 않았다.
