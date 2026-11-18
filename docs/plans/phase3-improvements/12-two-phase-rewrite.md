# 12. 두 단계 재작성 — 파일을 다시 쓰는 동안 SELECT 허용

작성일: 2026-11-18  
기준 커밋: `f89f37c` (브랜치 `feat/phase3-cpp`)  
상태: 보류(2026-11-18). 사용자가 임시 파일 최적화([16번](16-rewrite-copy.md))를 먼저 골랐고, 그 뒤 100만 행 한 행 변경이 컨테이너 85 ms(기준 100 ms 이하)·Windows 146 ms가 됐다. 배타 gate 보유 시간이 다시 문제가 되면 이 계획을 그대로 쓴다.

## 1. 문제

서버는 UPDATE·DELETE의 `executor_run` 전체를 배타 gate 안에서 실행한다([engine.cpp](../../../server/engine.cpp) `run`). 그래서 다음 작업이 모두 끝날 때까지 모든 SELECT가 기다린다.

- 파일 전체 읽기
- 임시 파일 쓰기
- 임시 파일 `fsync`
- 교체
- 다시 열기
- 폴더 `fsync`

이 중 SELECT와 실제로 충돌하는 것은 핸들을 닫고 교체하고 인덱스를 바꾸는 순간뿐이다. 앞부분은 기존 파일을 읽기만 하고 새 파일은 아무도 보지 않는다.

## 2. 지켜야 할 불변식

| 번호 | 불변식 | 이유 |
|---|---|---|
| I1 | 임시 파일을 만드는 동안 다른 쓰기(INSERT·UPDATE·DELETE)가 끼어들지 않는다 | 끼어든 INSERT 행은 임시 파일에 없으므로 교체하면 사라진다 |
| I2 | 파일 교체, 인덱스 교체, 핸들 다시 열기는 어떤 SELECT와도 겹치지 않는다 | SELECT가 닫힌 핸들이나 옛 인덱스와 새 파일 조합을 보면 안 된다 |
| I3 | SELECT는 문장 하나 동안 한 버전만 본다 | 지금과 같은 보장. 준비 단계 중 SELECT는 옛 버전, 교체 뒤는 새 버전을 본다 |
| I4 | 실패 의미는 10번과 같다 | 교체 전 실패는 아무것도 바꾸지 않고, 교체 뒤 실패는 "적용됨" + Table 차단 |

## 3. 범위

포함:

- C 저장소: `table_rewrite`를 준비(prepare)와 확정(commit) 두 함수로 나눈다. 기존 `table_rewrite`는 두 함수를 차례로 부르는 함수로 남겨 CLI와 기존 테스트를 그대로 둔다.
- C 실행기: UPDATE·DELETE를 준비·확정으로 나눠 부를 수 있는 함수 한 쌍.
- 서버 엔진: 쓰기끼리만 막는 `write_mutex` 추가, UPDATE·DELETE 경로 변경, 잠금 대기 지표 1종 추가.
- 문서: 서버 README의 동시성 절, spec, 이 문서.

제외:

- INSERT 경로 변경. INSERT는 짧으므로 지금처럼 배타 gate 안에서 실행한다(다만 `write_mutex`도 먼저 잡는다, 4절).
- gate 정책(쓰기 우선) 변경, MVCC, 여러 UPDATE의 동시 실행.
- CLI 동작 변경. CLI는 단일 스레드라 이득이 없다.

## 4. 설계

### C API (`src/storage.h`)

```c
typedef struct RewritePlan RewritePlan;

/* 1~3단계: 기존 파일을 읽어 임시 파일을 쓰고 fsync·close, 새 인덱스를 만든다.
 * Table의 파일·인덱스는 읽기만 한다. 바뀐 행이 없으면 *out = NULL, *changed = 0. */
int table_rewrite_prepare(Table *t, RewriteFn fn, void *ctx, RewritePlan **out,
                          size_t *changed, DbError *err);
/* 4단계: 핸들 닫기·교체·인덱스 교체·다시 열기·폴더 fsync. plan은 항상 해제한다. */
int table_rewrite_commit(Table *t, RewritePlan *plan, DbError *err);
/* 확정하지 않고 버린다. 임시 파일을 지운다. NULL이면 아무것도 안 한다. */
void table_rewrite_abort(RewritePlan *plan);
```

- 준비 단계는 `t->fp`(쓰기 쪽 핸들)로 읽는다. 이 핸들은 쓰기 스레드만 쓰고 SELECT는 각자 `TableReader`를 쓰므로, `write_mutex` 아래라면 SELECT와 같은 파일을 동시에 읽어도 된다.
- 기존 `table_rewrite`는 `prepare` → (`plan != NULL`이면) `commit`으로 구현한다.

### 실행기 (`src/executor.h`)

```c
/* UPDATE·DELETE만. 검증·WHERE id 미스 확인·준비까지. */
int executor_modify_prepare(Table *t, const Statement *st, RewritePlan **plan,
                            size_t *changed, DbError *err);
/* 확정 후 sink->on_change를 부른다. plan이 NULL이면 0행 성공만 알린다. */
int executor_modify_commit(Table *t, RewritePlan *plan, size_t changed,
                           const ResultSink *sink, DbError *err);
```

`run_modify`는 이 두 함수로 다시 구현한다. 오류 문구("변경 N행은 적용됨: …")는 그대로다.

### 서버 엔진 잠금 순서

```text
INSERT        : write_mutex → 배타 gate → executor_run
UPDATE·DELETE : write_mutex → 공유 gate → executor_modify_prepare
                            → 공유 gate 풀기 → 배타 gate → executor_modify_commit
SELECT        : reader mutex → 공유 gate (변경 없음)
```

- 공유 gate를 푼 뒤 배타 gate를 다시 잡을 때까지 다른 쓰기는 `write_mutex`에 막혀 있으므로 파일이 바뀌지 않는다(I1).
- 이 틈에 들어온 SELECT는 옛 버전을 끝까지 읽고, 배타 gate는 그 SELECT가 끝난 뒤 잡힌다(I2, I3).
- 준비 단계에서 공유 gate를 잡는 이유: 준비 중 실패로 `write_failed`가 될 수는 없지만, 인덱스(`table_find_id`)를 읽으므로 기존 규칙("인덱스는 gate 아래서 읽는다")을 지킨다.
- 쓰기 우선 gate와의 상호작용: INSERT·UPDATE는 gate가 아니라 `write_mutex`에서 기다리므로, 대기 중인 쓰기가 새 SELECT를 막는 일은 "확정 단계 직전" 한 번뿐이다. 지금보다 SELECT가 덜 막힌다.
- 만료 검사: 지금처럼 `write_mutex`를 잡은 직후 한 번 한다. 확정 직전에는 검사하지 않는다(준비를 끝낸 문장을 버리면 일만 낭비한다).
- 지표: `engine.lock_wait`에 `write_mutex`를 추가한다. 기존 `write_gate`는 배타 gate 대기만 계속 뜻한다. 기존 필드는 없애지 않는다.

### 검토 후 제외한 대안

| 대안 | 제외 이유 |
|---|---|
| 준비 단계를 gate 밖에서, `write_mutex` 없이 실행 | INSERT가 끼어들면 그 행이 사라진다(I1 위반) |
| 준비 단계를 gate 없이 `write_mutex`만으로 실행 | 동작은 맞지만 "인덱스는 gate 아래서 읽는다"는 기존 규칙과 TSan 근거가 달라진다. 이득이 없다 |
| 공유 gate를 배타로 승격(upgrade) | `WriterPreferredGate`에 승격 기능이 없고, 추가하면 gate를 다시 검증해야 한다. `write_mutex`로 같은 효과를 얻는다 |

## 5. 작업 단계

1. 11번 결과로 진행이 정해졌는지 확인한다.
2. `storage.c`: `table_rewrite`의 본문을 prepare·commit·abort로 나누고 `table_rewrite`는 둘을 부르게 한다. 동작 변경 없음.
3. `executor.c`: `executor_modify_prepare`·`executor_modify_commit` 추가, `run_modify`를 이것으로 재구현.
4. `engine.cpp`: `write_mutex`, UPDATE·DELETE 경로, 지표. 테스트용 훅(5절)을 `detail` 이름공간에 추가한다.
5. 문서 갱신, 11번 `modify` 측정으로 100만 행의 준비·확정 시간을 나눠 한 번 잰다.

## 6. 테스트 (최소)

| 테스트 | 왜 필요한가 |
|---|---|
| 단위 1개: prepare 뒤 abort하면 파일·인덱스 그대로이고 임시 파일이 없다. prepare 뒤 commit은 기존 결과와 같다 | 새 API의 유일한 새 경로(abort) |
| 엔진 1개(`engine_gate_test`): UPDATE가 준비를 마친 시점에 테스트 훅으로 멈추게 하고, 그 사이 SELECT가 끝나며 옛 값을 보는지, 그 사이 INSERT는 끝나지 않는지 확인한 뒤 훅을 풀어 UPDATE가 끝나고 새 값이 보이는지 확인 | I1·I3의 직접 증거. sleep·시간이 아니라 훅으로 순서를 맞춘다(계획 README 검증 규칙) |
| 기존 `make test`, Release `ctest` | 회귀 |
| TSan: `engine`·`engine_gate` 테스트 | 잠금 순서를 바꾸는 변경이므로 계획 README 규칙상 필수 |

실행하지 않음: sanitizer(ASan) 전체, HTTP 벤치마크, Windows 실행(C 변경은 순수 리팩터링이고 서버는 Linux 전용 검증 경로).

## 7. 완료 기준

- 위 테스트가 통과하고 TSan 경고 0건.
- 100만 행에서 확정 단계(배타 gate 보유) 시간이 기록돼 있다.
- 서버 README 동시성 절이 새 잠금 순서를 설명한다.

## 8. 위험

- 준비 단계 동안 INSERT는 지금처럼 기다린다. 쓰기 처리량은 늘지 않는다.
- 확정 단계는 여전히 핸들 다시 열기와 폴더 `fsync`를 포함한다. 저장 장치에 따라 수십 ms가 남을 수 있다. 이를 더 줄이는 것은 범위 밖이다.
