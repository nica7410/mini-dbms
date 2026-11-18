# 10. UPDATE·DELETE

작성일: 2026-11-18
기준 커밋: `bb8a0cf4b335bf9cc691a9a1f666188754787987` (브랜치 `feat/phase3-cpp`)
상태: 구현·검증 완료

## 요청과 범위

2026-11-18 사용자 지시: "phase 3에 추가 구현을 진행하자. Update랑 Delete 추가해줘."

[공통 범위](README.md#공통-범위)는 UPDATE/DELETE를 제외했으나 이 지시로 해제한다. 단일 writer, 모든 파일 변경이 이 서버(또는 CLI 한 프로세스)를 통한다는 전제, 트랜잭션·자동 재시도 제외는 그대로다.

## 문법과 결과

```sql
UPDATE users SET name = '값' [, age = 정수] [WHERE id|age = 정수];
DELETE FROM users [WHERE id|age = 정수];
```

| 항목 | 결정 | 이유 |
|---|---|---|
| SET 컬럼 | `name`, `age` 각각 최대 한 번, 하나 이상. `id`는 오류 | id는 자동 부여 규칙(INSERT에서 지정 금지)과 같게 둔다 |
| WHERE | SELECT의 `parse_where`를 그대로 사용. 생략하면 모든 행 | 조건 규칙을 한 곳에 둔다 |
| 값 검증 | INSERT와 같은 name·age 규칙(Executor, 서버 `INVALID_VALUES`) | 저장 형식이 같다 |
| 행 수 | 조건에 맞은 행 수(값이 같아도 센다). 0행도 성공 | PostgreSQL의 `UPDATE N`과 같은 의미 |
| CLI 출력 | `UPDATE N`, `DELETE N` | 기존 `INSERT N` 형식 |
| HTTP | `type: "update"|"delete"`, `affected_rows`, `write_outcome: "applied"` | 기존 INSERT 응답 필드 재사용 |

## 저장 방식

후보:

1. **임시 파일에 다시 쓰고 교체 (채택)** — 기존 파일을 검증하며 읽어 행마다 KEEP/REPLACE/DELETE를 정하고 `users.tsv.tmp`에 쓴 뒤 원래 파일을 대체한다. 새 인덱스도 같은 순서로 만들어 교체 성공 뒤 바꾼다.
2. 같은 파일에 삭제·변경 기록을 추가 — 파일 형식(증가하는 id, 한 줄 한 행)과 기존 검증·CLI 호환이 깨진다.
3. 제자리 덮어쓰기 — 이름 길이가 바뀌면 줄 길이가 달라 불가능하고, 실패하면 원본이 손상된다.

채택한 방식의 성질:

- 파일 형식, 저장 순서, id는 그대로다. KEEP 행은 정규형이므로 원래 바이트 그대로 다시 쓰인다.
- 교체 전(검증·쓰기·flush·close·이름 바꾸기) 실패는 아무것도 바꾸지 않고 Table을 계속 쓸 수 있다. 교체 뒤 다시 열기에 실패하면 변경은 적용된 채 Table을 막는다(`write_failed`, 서버는 `DATABASE_UNAVAILABLE`).
- 일치하는 행이 없으면 파일을 다시 쓰지 않는다.
- Windows의 `rename`은 대상이 있으면 실패하므로 `MoveFileExA(MOVEFILE_REPLACE_EXISTING)`을 쓴다. 열린 파일은 대체할 수 없으므로 교체 전에 Table과 모든 reader의 `FILE*`를 닫고, 교체 뒤 다시 연다. POSIX에서도 reader가 옛 inode를 계속 읽지 않도록 같은 절차를 따른다. 이를 위해 Table이 열린 reader 목록을 갖는다.
- 비용은 O(행 수)이고, 서버에서는 그동안 배타 gate를 잡는다. 단, `WHERE id = N`은 먼저 ID 인덱스를 확인해 없으면 파일을 읽지 않고 0행으로 끝낸다([후속: ID 미스 생략](#후속-where-id-미스-시-파일-읽기-생략-2026-11-18)).
- 교체 전에 임시 파일을 `fsync`하고, 교체 뒤 폴더를 `fsync`한다([후속: 교체 fsync](#후속-파일-교체-fsync-2026-11-18)). INSERT는 `fsync`하지 않는다.

### ID 재사용

`last_id`는 줄이지 않으므로 같은 실행 안에서는 지운 최대 ID를 다시 쓰지 않는다. 재시작하면 기존 규칙(파일의 최대 ID + 1)으로 다시 계산하므로 끝쪽 ID를 지운 뒤 재시작하면 그 ID가 다시 쓰인다. 최대 ID를 따로 저장하려면 파일 형식이나 보조 파일이 필요해 이번 범위에서 제외했다. 테스트(`test_executor_update_delete`)가 이 동작을 고정한다. **2026-11-18 [13번](13-persist-max-id.md)에서 `users.meta`로 해결했다.** 이제 재시작 뒤에도 다시 쓰지 않는다.

## 변경 파일

| 파일 | 변경 |
|---|---|
| `src/parser.h`, `src/parser.c` | `STMT_UPDATE`·`STMT_DELETE`, `Statement`의 `set_name`·`set_age`·`has_set_age`(기존 위치 초기화를 깨지 않도록 끝에 추가), `parse_set`/`parse_update`/`parse_delete`, `parser_peek_kind` 확장. 알 수 없는 첫 키워드 메시지는 "INSERT, SELECT, UPDATE 또는 DELETE" |
| `src/storage.h`, `src/storage.c` | `table_rewrite`(RewriteFn, RowEdit), reader 목록, 파일 닫기·교체·재열기 |
| `src/executor.h`, `src/executor.c` | `ResultSink.on_change`(끝에 추가), `run_modify`, name·age 검증 분리 |
| `src/main.c` | `UPDATE N`/`DELETE N` 출력 |
| `server/engine.hpp`, `server/engine.cpp` | `QueryKind`, `QueryResult.affected_count`, UPDATE 값 검증, 쓰기 경로(배타 gate·만료 검사) 공유, `successful_updates/deletes` |
| `server/main.cpp` | 쓰기 큐 분류(`INSERT`·`UPDATE`·`DELETE`), 응답 `type`·`affected_rows`, `request_kinds.update/delete`, 지표 |
| 테스트 | `tests/test_unit.c`, `tests/cli_test.py`, `tests/benchmark.c`(초기화자), `server/tests/engine_test.cpp`, `server/tests/api_test.py` |
| 문서 | `docs/spec.md`, `README.md`, `server/README.md`, 이 문서, 계획 README |

## 검증 기록 (2026-11-18)

환경: Windows 10 호스트, Docker Desktop 29.7.2, 이미지 `mini-dbms-phase3-review:3b19667`(Debian 12, GCC 12.2, CMake 3.25, Python 3.11). 호스트에는 TDM-GCC가 없어 Windows 바이너리는 컨테이너의 MinGW-w64(`gcc-mingw-w64-x86-64`, Debian 12 패키지)로 교차 컴파일한 뒤 호스트에서 실행했다.

| 검사 | 명령(컨테이너 안) | 결과 |
|---|---|---|
| C 빌드 경고 | `make /tmp/b/minidb /tmp/b/test_unit` (`-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes`) | 경고 0건 |
| C 단위 테스트 | `test_unit` | 34733개 검사, 실패 0 |
| CLI 테스트 | `python3 tests/cli_test.py` | 33개 PASS |
| ASan/UBSan/누수 | `setarch -R make sanitize` (`--security-opt seccomp=unconfined`) | 단위 실패 0, CLI 33개 PASS |
| 서버 Release `ctest` | `cmake --build /build && ctest` | 6/6 PASS |
| TSan | `-DMINIDB_ENABLE_TSAN=ON -DMINIDB_BUILD_HTTP=OFF`, `setarch -R` engine·engine_gate·pool·gate | 4개 exit 0, 경고 0건 |
| 프로젝트 코드 C++ 경고 | Release 빌드 | 0건(Drogon 내부 `-Wshadow` 경고만) |
| Windows 빌드 경고 | `x86_64-w64-mingw32-gcc` 같은 C 플래그 + `-D__USE_MINGW_ANSI_STDIO=1` | 경고 0건 |
| Windows 단위·CLI | `-static` 빌드를 호스트에서 `test_unit.exe`, `python tests/cli_test.py minidb.exe` | 34675개 검사 실패 0(POSIX 전용 테스트 제외), CLI 33개 PASS. `MoveFileExA` 교체 경로 포함 |

`make sanitize`를 ASLR 완화 없이 실행하면 CLI 테스트가 매번 다른 항목에서 30초 시간 초과로 실패했다. 같은 바이너리를 직접 6회 실행하면 모두 정상 종료했고, 컨테이너의 `vm.mmap_rnd_bits`가 32였다. [05 최소 검증](05-minimal-verification.md)에 기록된 ASan·높은 ASLR 엔트로피 문제와 같은 증상으로 보고 `setarch -R`로 다시 실행했다. 이 완화는 이번 실행에만 썼다.

추가한 테스트:

- 단위: 파싱 성공·오류 25종, 여러 행 UPDATE·이름 길이 변경 뒤 인덱스 위치, 0행일 때 파일 불변, 값 오류 불변, DELETE 뒤 id 조회, 실행 중 최대 ID 미재사용·재시작 뒤 재계산, 전체 DELETE, 콜백 실패·저장 불가 값·임시 파일 쓰기 실패(`RLIMIT_FSIZE`) 시 원본 불변과 Table 계속 사용, reader 3개 재열기(가운데 reader 닫은 뒤 포함), 읽기 핸들의 UPDATE·DELETE 거부.
- CLI: UPDATE·DELETE 흐름과 재실행 뒤 id 조회, 전체 DELETE 뒤 INSERT, 두 번째 UPDATE 값 오류 시 앞 문장 유지.
- 엔진: 결과 필드·지표·만료·검증, reader 3개가 스캔·id 조회하는 동안 UPDATE 40회(스캔이 한 버전만 보는지).
- HTTP: 응답 형식, 다른 worker에서 즉시 보임, 0행 성공, 422/400, 임시 파일 미잔존, 재시작 뒤 유지, `/metrics`의 `update`·`delete`.

## 남은 한계

- Windows는 MinGW-w64 교차 빌드로만 확인했다. README의 TDM-GCC `mingw32-make test` 경로는 실행하지 않았다.
- UPDATE·DELETE는 O(행 수)이며 실행 중 다른 SQL을 막는다(`WHERE id = N`의 ID가 없는 경우 제외). 100만 행에서의 지연은 재지 않았다.
- ~~재시작 뒤 끝쪽 ID 재사용~~ → [13번](13-persist-max-id.md)에서 해결.
- 교체 직전 열린 파일을 닫는 동안 다른 프로세스가 같은 파일을 열면(Windows) 교체가 실패할 수 있다. 이 경우 원본은 그대로다.

## 후속: WHERE id 미스 시 파일 읽기 생략 (2026-11-18)

사용자 지시: "WHERE id = N일 때 인덱스로 대상 행이 있는지 먼저 확인해, 없으면 파일을 아예 읽지 않게 하는 것 … 일단 이거 작업 진행해"

- 변경: `src/executor.c`의 `run_modify`가 `WHERE id = N`이면 값 검증 뒤 `table_find_id`로 행을 확인한다. 없으면 `table_rewrite`를 부르지 않고 0행 성공이다. 있으면 기존대로 파일 전체를 다시 쓴다. 저장소 API는 바꾸지 않았다.
- 비용: 없는 ID는 B+ 트리 조회 한 번(O(log n))이다. 있는 ID는 그 행 하나를 더 읽는다(seek 1회, 이어서 O(행 수) 재작성).
- 동작 차이: 없는 ID에 대한 UPDATE·DELETE는 더 이상 파일 전체를 검증하지 않는다. 시작 뒤 외부에서 파일이 손상돼도 0행 성공을 돌려준다. `SELECT … WHERE id = N`과 같은 규칙이고, 파일 변경은 이 프로세스만 한다는 전제 안의 차이다.
- 테스트: `test_modify_id_miss_skips_scan` — 연 뒤 파일 끝에 손상된 줄을 붙이고, 없는 ID의 UPDATE·DELETE는 0행 성공, 값 오류는 여전히 오류, 있는 ID와 `age` 조건은 손상 오류, 파일 불변을 확인한다.
- 검증: 컨테이너(`mini-dbms-phase3-review:3b19667`)에서 `make test` — 경고 0건, 단위 34742개 검사 실패 0, CLI 33개 PASS. 서버 `ctest`·sanitizer·TSan은 다시 돌리지 않았다(서버 코드 변경 없음, 잠금 범위 동일).

## 후속: 파일 교체 fsync (2026-11-18)

사용자 지시: 다음 개선 후보 중 "1번 진행"(파일 교체 전에 디스크에 확실히 쓰기).

- 문제: `fsync` 없이 `rename`하면 전원 장애 때 임시 파일 내용이 디스크에 닿기 전에 이름 바꾸기만 남아, `users.tsv`가 비거나 일부만 남을 수 있었다. 추가 쓰기만 있을 때는 끝의 몇 줄만 잃었지만, 교체는 테이블 전체를 잃을 수 있다.
- 변경(`src/storage.c`):
  - `sync_file`: 임시 파일을 `fflush` 뒤 `fsync`(Windows `_commit` = `FlushFileBuffers`). 실패하면 교체 전이므로 아무것도 바뀌지 않는다.
  - `sync_parent_dir`: 교체·다시 열기 뒤 데이터 폴더를 `fsync`한다(POSIX). 폴더를 열 권한이 없거나(`EACCES`) 폴더 `fsync`를 지원하지 않으면(`EINVAL`·`EBADF`) 성공으로 본다(PostgreSQL과 같은 규칙). 그 밖의 실패는 변경이 적용된 채 `write_failed`로 Table을 막는다(`*changed > 0` 실패 계약 그대로). Windows는 `MOVEFILE_WRITE_THROUGH`가 이동을 디스크에 기록한 뒤 돌아오므로 하지 않는다.
  - INSERT 경로와 `WHERE id` 미스 경로는 바꾸지 않았다.
- 비용: UPDATE·DELETE마다 `fsync` 2회가 늘어난다(저장 장치에 따라 수 ms~수십 ms). 측정하지 않았다.
- 검증:
  - 컨테이너 `make test`: 경고 0건, 단위 34742개 검사 실패 0, CLI 33개 PASS.
  - 호출 순서: `fsync`·`rename`을 가로채는 `LD_PRELOAD` 도구로 CLI 실행. UPDATE와 DELETE마다 `fsync users.tsv.tmp` → `rename` → `fsync <폴더>` 순서, INSERT와 없는 ID의 DELETE는 `fsync` 없음.
  - Windows: MinGW-w64 `-static` 교차 빌드 경고 0건, 호스트에서 단위 34684개 검사 실패 0, CLI 33개 PASS.
  - 미실행: 실제 전원 차단 시험, 서버 `ctest`·sanitizer·TSan(서버 코드·잠금 변경 없음). `fsync` 실패 경로는 [15-A](15-small-followups.md#a-fsync-실패-경로-테스트)에서 테스트로 고정했다.
