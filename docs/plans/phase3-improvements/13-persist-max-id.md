# 13. 재시작 뒤 지운 ID 재사용 막기

작성일: 2026-11-18  
기준 커밋: `f89f37c` (브랜치 `feat/phase3-cpp`)  
상태: 완료(2026-11-18). 결과는 [7절](#7-결과-2026-11-18).

## 1. 문제

- 다음 ID는 `Table.last_id + 1`이다. `last_id`는 시작할 때 파일의 마지막 행 ID로 정한다([storage.c](../../../src/storage.c) `scan_rows`의 `build_index`).
- UPDATE·DELETE는 `last_id`를 줄이지 않으므로, 실행 중에는 지운 ID를 다시 쓰지 않는다.
- 재시작하면 파일에 남은 최대 ID로 다시 계산한다. 그래서 끝쪽 행을 지운 뒤 재시작하면 그 ID가 새 행에 다시 붙는다.

예:

```text
INSERT → id 5
DELETE … WHERE id = 5
재시작
INSERT → 다시 id 5
```

예전 id 5를 저장해 둔 클라이언트는 이제 다른 행을 보게 된다. 지금은 이 동작을 테스트(`test_executor_update_delete`)로 고정하고 문서에 한계로 적어 두었다.

## 2. 범위

포함:

- 부여한 최대 ID를 보조 파일 `users.meta`에 저장하고, 시작할 때 반영한다.
- CLI와 서버 모두(둘 다 `table_open`을 쓴다).
- `Table.last_id` 주석 정정. 지금은 "파일에 있는 최대 ID"라고 적혀 있지만 실제로는 "부여한 최대 ID"다.
- 문서: spec, server README의 ID 설명, 10번 문서의 한계 항목.

제외:

- `users.tsv` 형식 변경.
- INSERT 경로 변경. INSERT는 항상 파일 끝에 가장 큰 ID를 쓰므로 보조 파일이 필요 없다.
- 기존 데이터 이전 도구. 보조 파일이 없으면 지금과 같은 규칙으로 동작한다.

## 3. 설계

### 파일

`users.meta`는 한 줄짜리 텍스트다.

```text
max_id\t<정수>\n
```

### 언제 쓰나

`table_rewrite`가 파일을 교체하기 **전에**, 다음 두 조건이 모두 맞을 때만 쓴다.

- 새 파일의 마지막 ID가 `t->last_id`보다 작다. 즉 최대 ID 행을 지웠다.
- 보조 파일에 이미 같은 값이 있지 않다(같은 실행에서 이미 쓴 값은 다시 쓰지 않는다).

대부분의 UPDATE와 끝쪽이 아닌 DELETE는 보조 파일을 건드리지 않으므로 `fsync`가 늘지 않는다.

쓰는 방법은 10번 후속과 같다: `users.meta.tmp`에 쓰기 → `fsync` → 교체 → 폴더 `fsync`.

### 순서와 장애

| 장애 시점 | 결과 | 문제 여부 |
|---|---|---|
| 보조 파일 교체 전 | 둘 다 옛 상태 | 없음 |
| 보조 파일 교체 후, `users.tsv` 교체 전 | 보조 파일 값 ≥ 파일 최대 ID | 없음. ID가 하나 건너뛸 뿐 재사용되지 않는다 |
| 둘 다 교체 후 | 새 상태 | 없음 |

`users.tsv`를 먼저 바꾸면 그 사이 장애가 났을 때 재사용이 생긴다. 그래서 보조 파일을 먼저 쓴다.

### 시작할 때

- `last_id = max(파일 마지막 ID, 보조 파일 값)`.
- 보조 파일이 없으면 지금과 같다(이전 버전 데이터와 호환).
- 보조 파일 형식이 틀리면 `table_open`이 "데이터 파일 손상"과 같은 종류의 오류로 실패한다. 조용히 무시하면 재사용이 다시 생길 수 있다.
- 보조 파일 값이 파일 최대 ID보다 작은 경우(구버전으로 INSERT한 뒤 등)는 파일 값을 쓴다. 오류로 보지 않는다.

### 검토 후 제외한 대안

| 대안 | 제외 이유 |
|---|---|
| 지운 마지막 행 자리에 표시 줄을 남김 | `users.tsv` 형식과 기존 검증·CLI 호환이 깨진다 |
| INSERT마다 보조 파일 갱신 | INSERT마다 쓰기·`fsync`가 늘어난다. 파일 자체가 최대 ID를 담고 있으므로 불필요하다 |
| 문서에 한계로만 둠 (현 상태) | 클라이언트가 ID를 저장하는 API에서는 잘못된 행을 가리키는 위험이 남는다 |

## 4. 작업 단계

1. `storage.c`
   - `META_FILE`, `read_meta`, `write_meta`를 추가한다. `write_meta`는 `sync_file`·`replace_file`·`sync_parent_dir`를 재사용한다.
   - `table_open`에서 `read_meta`를 반영한다.
   - `table_rewrite`의 교체 직전에 조건부로 `write_meta`를 호출한다. 실패하면 교체 전이므로 아무것도 바뀌지 않는다.
2. `last_id` 주석을 정정한다.
3. 테스트(5절)와 문서를 갱신한다.

## 5. 테스트 (최소)

| 테스트 | 왜 필요한가 |
|---|---|
| 기존 `test_executor_update_delete`의 재시작 기대값 변경: 마지막 ID를 지우고 다시 연 뒤 INSERT하면 지운 ID + 1 | 이 작업의 목적 자체 |
| 단위 1개(`test_meta_file`), 4가지 검사 | 시작 규칙의 분기를 각각 한 번씩 확인 |

`test_meta_file`의 4가지 검사:

1. 보조 파일이 없으면 기존 규칙대로 동작한다.
2. 보조 파일 값이 파일 최대 ID보다 작으면 파일 값을 쓴다.
3. 보조 파일 형식이 틀리면 `table_open`이 실패한다.
4. 끝쪽이 아닌 행을 DELETE하면 보조 파일이 생기지 않는다.

그 밖의 검증:

- 기존 `make test` 회귀.
- Windows 단위·CLI 1회. 새 파일을 교체하는 경로가 `MoveFileExA`를 쓰므로 필요하다.

실행하지 않음:

- 서버 `ctest`·TSan. 서버 코드와 잠금은 바뀌지 않는다.
- sanitizer. 새 코드는 고정 크기 버퍼로 한 줄만 읽는다.
- 장애 주입 테스트. 3절의 순서 논증으로 대신한다.

## 6. 완료 기준

- 위 테스트가 통과한다.
- spec·server README·10번 문서에서 "재시작 뒤 ID 재사용" 한계를 지우고, 새 규칙과 `users.meta`를 설명한다.

## 7. 결과 (2026-11-18)

계획대로 구현했다. 계획과 다른 점은 없다.

- `src/storage.c`
  - `META_FILE`·`read_meta`·`write_meta`를 추가했다.
  - `Table`에 `meta_path`·`meta_id`를 추가했다.
  - `table_open`이 `read_meta`를 호출한다.
  - `table_rewrite`는 교체 직전에 조건부로 `write_meta`를 호출한다. 조건은 `last_kept < last_id`이고 `meta_id < last_id`일 때다.
  - `last_id` 주석을 "부여한 최대 ID"로 바로잡았다.
- `src/storage.h`: `table_open`·`table_rewrite` 설명을 갱신했다.
- `tests/test_unit.c`
  - `fresh_dir`가 `users.meta`도 지운다.
  - `test_executor_update_delete`의 재시작 기대값을 바꿨다(4 → 6, 5 → 7). 전체 DELETE 뒤 재시작 확인을 추가했다.
  - `test_meta_file`을 추가했다. 확인 항목: meta 없음, 끝쪽이 아닌 DELETE는 meta 없음, meta가 파일 최대 ID보다 작음·큼, 형식 오류 9종.
- 문서: spec(ID 규칙, UPDATE·DELETE 절), server README, 10번 문서의 한계 항목.

검증:

| 검사 | 결과 |
|---|---|
| 컨테이너 `make test` | 경고 0건, 단위 34764개 검사 실패 0(15-A 전 기준), CLI 33개 PASS |
| Windows(MinGW-w64 `-static`, 호스트 실행) | 단위 34721개 검사 실패 0(15-A 포함 빌드, POSIX 전용 제외), CLI 33개 PASS |

실행하지 않음:

- 서버 `ctest`·TSan: 서버 코드와 잠금은 그대로다.
- sanitizer: 고정 버퍼 64바이트에 `fread` 한 번만 한다.
- 장애 주입: 순서 논증(3절)으로 대신했다.
