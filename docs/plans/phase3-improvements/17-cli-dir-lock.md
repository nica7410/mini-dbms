# 17. CLI 데이터 디렉터리 잠금 — 서버가 쓰는 폴더에서 CLI 거부

작성일: 2026-11-18  
기준 커밋: `f5738cd` (브랜치 `feat/phase3-cpp`)  
상태: 완료(2026-11-18). 결과는 [6절](#6-결과-2026-11-18).

## 1. 문제와 근거

서버는 시작할 때 데이터 디렉터리의 `.minidb-server.lock`을 잡는다(`server/main.cpp`의 `DataDirectoryLock`).

- POSIX: `flock(LOCK_EX | LOCK_NB)`
- Windows: `CreateFileW` 공유 모드 0

이 잠금은 서버 두 개가 같은 폴더를 쓰는 것만 막는다. CLI(`minidb`)는 이 잠금을 확인하지 않는다. README는 "같은 데이터 디렉터리에 기존 CLI와 서버를 동시에 실행하지 않는다"고 규칙만 적어 두었다.

INSERT만 있던 때는 CLI가 끼어들어도 서버의 인덱스만 어긋났다. 10번 뒤에는 CLI의 UPDATE·DELETE가 `users.tsv`를 새 파일로 교체한다. 그래서 서버가 성공이라고 응답한 데이터가 사라진다.

2026-11-18 컨테이너에서 실제 서버와 CLI로 재현했다(`f5738cd`).

| 순서 | 동작 | 결과 |
|---|---|---|
| 1 | 서버 INSERT a | id 1 |
| 2 | 같은 폴더에서 CLI `UPDATE users SET age = 2 WHERE id = 1;` | 종료 코드 0, `UPDATE 1`. 거부되지 않음 |
| 3 | 서버 INSERT b | id 2, 성공 응답 |
| 4 | 서버 SELECT | a의 age가 1(옛 값), b가 보임 |
| 5 | 서버 종료 뒤 `users.tsv` | `1\ta\t2`만 남음. **성공 응답한 b가 없다** |

원인: 서버는 CLI가 교체하기 전의 옛 파일을 계속 열어 두고 쓴다. Linux에서는 이름이 지워진 그 옛 파일에 b를 기록했고, 서버가 멈추면서 옛 파일과 함께 b가 사라졌다.

## 2. 범위

포함(`src/main.c`, CLI 진입점만):

- CLI가 `table_open` **전에** 같은 잠금 파일을 서버와 같은 방식으로 잡는다.
  - POSIX: `open(O_CREAT | O_RDWR | O_CLOEXEC)` + `flock(LOCK_EX | LOCK_NB)`
  - Windows: `CreateFileA` 공유 모드 0
- 잠금은 `table_close` 뒤까지 유지한다.
- 잠금을 못 잡으면 오류를 출력하고 종료 코드 1로 즉시 끝낸다. 기다리지 않는다.
  - 다른 프로세스가 잡고 있음: "데이터 디렉터리를 다른 프로세스(서버 또는 다른 minidb)가 사용 중입니다: <폴더>"
  - 잠금 파일을 열 수 없음: "데이터 디렉터리 잠금 파일을 열 수 없습니다: <경로> (<원인>)"
- 데이터 디렉터리가 없으면 지금처럼 만든다. `table_open`과 같은 한 단계 `mkdir`이다.
- CLI 두 개가 같은 폴더에서 동시에 실행되는 경우도 같이 막힌다.

제외:

- 잠금 파일 이름 변경. 이름을 바꾸면 서버도 바꿔야 하고, 옛 서버와 새 CLI가 서로를 못 본다.
- 저장 계층(`storage.c`)에 잠금 넣기. 서버는 한 프로세스에서 Table을 여러 개(읽기 worker마다) 연다. 같은 프로세스 안에서 다시 잡으면 Windows 공유 모드 0이 자기 자신을 막는다.
- 잠금 대기·재시도 옵션, 서버 코드 변경(주석 1줄만 고친다).

## 3. 바뀌는 동작

- 서버가 실행 중인 폴더에서 CLI는 아무것도 읽거나 쓰지 않고 실패한다. 지금까지는 실행됐다.
- 데이터 디렉터리 자리에 일반 파일이 있으면, 오류 문구가 "데이터 파일을 열 수 없습니다"에서 "데이터 디렉터리 잠금 파일을 열 수 없습니다"로 바뀐다. 기존 CLI 테스트 `a7_data_dir_unusable`은 공통 부분인 "열 수 없습니다"로 확인하도록 고친다.
- CLI도 데이터 폴더에 `.minidb-server.lock`(빈 파일)을 남긴다. 서버와 같다.

## 4. 테스트 (최소)

| 테스트 | 왜 필요한가 |
|---|---|
| CLI 테스트 1개 추가: 테스트가 잠금을 잡은 채로 CLI INSERT 실행 → 실패, "사용 중", `users.tsv`가 생기지 않음. 잠금을 놓은 뒤 같은 INSERT 성공 | 이 작업의 핵심 동작. POSIX는 `fcntl.flock`, Windows는 파일을 열어 두기만 하면 공유 모드 0이 실패한다 |
| 기존 `make test`(컨테이너) | CLI 경로 회귀 |
| 1절 재현 스크립트 1회(컨테이너, 실제 서버) | 실제 서버 잠금과 맞물리는지. 2단계에서 CLI가 거부되고, 5단계 파일에 a·b가 모두 남아야 한다 |
| Windows 단위·CLI 1회 | `CreateFileA` 경로는 Windows에서만 실행된다 |

실행하지 않음: 서버 `ctest`·TSan(서버 코드 변경 없음), sanitizer(새 코드는 파일 핸들 하나 열고 닫기).

## 5. 완료 기준

- 위 테스트가 통과한다.
- README·server/README·`server/main.cpp` 주석의 "CLI는 잠금에 참여하지 않는다"는 설명을 새 동작으로 고친다.

## 6. 결과 (2026-11-18)

계획대로 구현했다.

- `src/main.c`
  - `lock_data_dir`·`unlock_data_dir`를 추가했다.
  - `table_open` 직전에 잠그고, `table_close` 뒤에 놓는다.
  - POSIX는 `flock`을 쓰려고 `_DEFAULT_SOURCE`를 켠다.
- `tests/cli_test.py`
  - `a7_data_dir_locked`를 추가했다.
  - `a7_data_dir_unusable`은 "열 수 없습니다"로 확인한다.
- 문서: `docs/spec.md` 입출력 계약 표에 "잠금" 행, `README.md`, `server/README.md`, `server/main.cpp` 주석.

### 검증

| 검사 | 결과 |
|---|---|
| 컨테이너 `make test` | 경고 0건, 단위 34793개 검사 실패 0, CLI 34개 PASS(새 테스트 포함) |
| 1절 재현 스크립트(컨테이너, 실제 `minidb_server`) | 2단계 CLI가 종료 코드 1, "데이터 디렉터리를 다른 프로세스(서버 또는 다른 minidb)가 사용 중입니다"로 거부됐다. 4단계 SELECT는 a(age 1)·b, 서버 종료 뒤 파일은 `1\ta\t1`, `2\tb\t1`로 두 행이 모두 남았다 |
| Windows(MinGW-w64 `-static`, 호스트 실행) | `minidb.exe` 빌드 경고 0건, CLI 34개 PASS. `a7_data_dir_locked`가 공유 위반 경로("사용 중")를 확인했다 |

실행하지 않음:

- Windows 단위 테스트: `src/main.c`는 단위 테스트 바이너리에 들어가지 않는다.
- 서버 `ctest`·TSan: 서버는 주석 한 줄만 바뀌었다.
- sanitizer.
- Windows에서 실제 서버와 함께 확인하기: 서버 빌드는 Linux/WSL·Docker 기준이다.

### 남은 한계

- 잠금은 이 프로그램끼리의 약속이다. 다른 프로그램이 `users.tsv`를 직접 고치는 것은 막지 못한다.
- POSIX `flock`은 일부 네트워크 파일 시스템(NFS 등)에서 동작이 다를 수 있다. 서버 잠금과 같은 한계다.
