# 15. 작은 후속 작업 — fsync 실패 경로 테스트, TDM-GCC 경로 확인

작성일: 2026-11-18  
기준 커밋: `f89f37c` (브랜치 `feat/phase3-cpp`)  
상태: A 완료(2026-11-18). B는 TDM-GCC 설치가 필요해 대기 중이다(미검증).

## A. fsync 실패 경로 테스트

### 문제

[10번 후속(파일 교체 fsync)](10-update-delete.md#후속-파일-교체-fsync-2026-11-18)에 실패 분기 두 개가 생겼다. 둘 다 코드를 읽어서만 확인했다.

| 분기 | 기대 동작 |
|---|---|
| 임시 파일 `fsync` 실패 | 교체 전이므로 아무것도 바뀌지 않는다. 임시 파일은 지운다. Table은 계속 쓸 수 있다. |
| 교체 뒤 폴더 `fsync` 실패 | 변경은 적용된다. `*changed > 0`과 함께 실패를 반환하고 `write_failed`로 Table을 막는다. 실행기 오류는 "변경 N행은 적용됨: …"이다. |

두 번째 분기는 "데이터는 바뀌었는데 오류를 돌려주는" 유일한 경우다. 서버의 `write_outcome: "applied"` 응답과 `DATABASE_UNAVAILABLE` 전환이 이 분기에 기대고 있다.

### 범위와 결정

- 포함: 테스트 빌드에서만 켜지는 실패 주입 지점을 하나 둔다. 두 분기를 단위 테스트 1개로 고정한다.
- 제외: 서버 응답 테스트. 서버의 "적용됨" 처리는 기존 "다시 열기 실패" 경로와 같은 코드다. 그 코드는 이미 `api_test.py`에서 응답 형식을 확인했다. 또 POSIX 전용 쓰기 실패 테스트(`RLIMIT_FSIZE`)와 겹치는 부분도 다시 하지 않는다.
- 결정: 실패 주입은 `#ifdef MINIDB_TEST_HOOKS`로 감싼 전역 카운터 하나로 한다(`storage_test_fail_sync`: 0이면 정상, 1이면 다음 임시 파일 sync 실패, 2이면 다음 폴더 sync 실패). Makefile의 `test_unit`·`sanitize` 단위 빌드에만 `-DMINIDB_TEST_HOOKS`를 준다. 제품 바이너리(`minidb`, 서버)에는 들어가지 않는다.

### 작업 단계

1. `storage.c`의 `sync_file`과 `sync_parent_dir` 앞에 주입 지점을 추가한다. 주입이 실패를 만들면 `errno = EIO`로 둔다.
2. `Makefile`의 `$(TEST_BIN)` 규칙과 `sanitize` 단위 빌드 줄에 `-DMINIDB_TEST_HOOKS`를 추가한다.
3. `tests/test_unit.c`에 `test_rewrite_sync_failures`를 추가한다. 위 표의 두 분기를 확인한다.
   - 첫 번째: 파일 불변, 임시 파일 없음, 이어서 INSERT 성공.
   - 두 번째: 파일은 새 내용, `changed > 0`, 이후 `table_scan` 실패.
4. 10번 문서 후속 절의 "미실행" 항목을 갱신한다.

### 테스트 (최소)

- 실행: 컨테이너 `make test`(경고 0건, 새 테스트 포함). Windows 실행 파일로 단위 테스트 1회. Windows의 `sync_parent_dir`는 아무것도 하지 않으므로, 두 번째 분기는 Windows에서도 주입으로 같은 결과가 나와야 한다.
- 실행하지 않음:
  - 서버 테스트: 서버 코드는 바뀌지 않는다.
  - TSan: 잠금이 바뀌지 않는다.
  - 전체 sanitizer: 새 코드는 정수 비교뿐이다.

### 완료 기준

두 분기가 테스트로 고정돼 있고, 제품 빌드에 주입 코드가 없다. 제품 빌드에서 `storage_test_fail_sync` 심볼이 없는지 `nm`으로 한 번 확인한다.

## B. TDM-GCC `mingw32-make test` 경로 확인

### 문제

README([빌드와 실행 (Windows, TDM-GCC)](../../../README.md))는 Windows 빌드 방법으로 `mingw32-make test`를 안내한다. 그런데 10번과 그 후속에서 Windows 전용 코드를 추가했다.

- `MoveFileExA`
- `_commit`
- `<io.h>`

이 코드는 MinGW-w64 교차 빌드(Debian 12 패키지)로만 확인했다. TDM-GCC는 버전에 따라 헤더와 런타임 구성이 다를 수 있다.

### 범위와 결정

- 포함: TDM-GCC가 설치된 Windows에서 README 명령을 그대로 실행한다.
  - `mingw32-make clean`
  - `mingw32-make`
  - `mingw32-make test`

  경고·실패가 있으면 기록하고, 원인이 이번 변경이면 고친다.
- 제외: CI 구성 추가. TDM-GCC 이외 Windows 컴파일러(MSVC 등) 지원.
- 결정: 이 PC에는 TDM-GCC가 없다. 설치는 외부 프로그램을 내려받는 일이므로 사용자가 직접 설치하거나 설치를 허락한 뒤에만 진행한다. 그 전까지는 "미검증"으로 둔다.

### 테스트

- 실행: 위 세 명령 1회. 결과(경고 수, 단위 검사 수, CLI 통과 수, TDM-GCC 버전)를 이 문서에 기록한다.
- 실행하지 않음: `mingw32-make bench`. 성능은 이 작업의 대상이 아니다.

### 완료 기준

README 명령이 경고 없이 통과했다는 기록이 있다. 또는 실패 원인과 수정 커밋이 기록돼 있다.

## 결과 (2026-11-18)

### A — 완료

계획대로 구현했다.

- `src/storage.c`
  - `#ifdef MINIDB_TEST_HOOKS` 안에 `storage_test_fail_sync`와 `test_sync_fails`를 두었다.
  - `sync_file`(값 1)과 `sync_parent_dir`(값 2)의 맨 앞에서 확인한다.
- `src/storage.h`: 같은 조건부 선언.
- `Makefile`: `$(TEST_BIN)`과 `sanitize`의 단위 빌드에만 `-DMINIDB_TEST_HOOKS`를 준다.
- `tests/test_unit.c`: `test_rewrite_sync_failures`를 추가했다.
  - **임시 파일 fsync 실패:** `changed == 0`, 파일 불변, 임시 파일 없음, 이어서 INSERT가 성공한다.
  - **폴더 fsync 실패:** `DELETE … WHERE id = 2`가 -1을 돌려주고 `affected == 1`이다. 오류는 "변경 1행은 적용됨"이다. 파일은 새 내용이고 `table_scan`은 거부된다. 다시 열면 3행이 보인다.

검증:

| 검사 | 결과 |
|---|---|
| 컨테이너 `make test` | 경고 0건, 단위 34779개 검사 실패 0, CLI 33개 PASS |
| 제품 빌드에 훅 없음 | `nm build/minidb`: `storage_test_fail_sync` 0개, `nm build/test_unit`: 1개 |
| Windows(MinGW-w64 `-static`, 호스트 실행, `-DMINIDB_TEST_HOOKS`) | 단위 34721개 검사 실패 0, CLI 33개 PASS |

### B — 대기

이 PC에는 TDM-GCC가 없다. 사용자가 설치하거나 설치를 허락하면 `mingw32-make clean`, `mingw32-make`, `mingw32-make test`를 실행한다.

- 참고로 B에서 확인하려던 Windows 전용 코드(`MoveFileExA`·`_commit`·`<io.h>`·새 `users.meta` 교체)는 MinGW-w64 교차 빌드로 실행해 통과했다.
- TDM-GCC 고유의 헤더·런타임 차이는 여전히 미검증이다.
