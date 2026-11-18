# 11. UPDATE·DELETE 비용 측정

작성일: 2026-11-18  
기준 커밋: `f89f37c` (브랜치 `feat/phase3-cpp`)  
상태: 측정 완료(2026-11-18). 결과는 [8절](#8-결과-2026-11-18).

## 1. 문제

UPDATE·DELETE는 문장마다 `users.tsv` 전체를 읽어 임시 파일에 다시 쓰고, `fsync` 2회 뒤 교체한다([10번](10-update-delete.md)). 서버에서는 이 동안 배타 gate를 잡으므로 모든 SELECT와 INSERT가 멈춘다.

이 시간이 얼마인지 아직 한 번도 재지 않았다. 다음 작업이 필요한지는 이 값으로 정한다.

- [12번 두 단계 재작성](12-two-phase-rewrite.md): SELECT가 멈추는 시간을 줄인다.
- [14번 변경 기록·정리](14-change-log-compaction.md): 한 행 변경 비용 자체를 줄인다.

## 2. 범위

포함:

- C 계층에서 UPDATE·DELETE 한 문장이 걸리는 시간. 서버의 배타 gate 보유 시간은 이 값과 같다고 본다(gate 안에서는 `executor_run`만 실행한다, [engine.cpp](../../../server/engine.cpp) `run`).
- 행 수 1만, 10만, 100만.
- 문장 4종:
  - `UPDATE users SET age = 1 WHERE id = <가운데 ID>`: 한 행 변경. 가장 흔한 경우.
  - `UPDATE users SET age = 1 WHERE age = <없는 값>`: 일치 0행이어도 전체를 읽는 경우. 파일은 다시 쓰지 않는다.
  - `DELETE FROM users WHERE id = <없는 ID>`: 인덱스에서 끝나는 경우([10번 후속](10-update-delete.md#후속-where-id-미스-시-파일-읽기-생략-2026-11-18)).
  - `DELETE FROM users WHERE id = <가운데 ID>`: 한 행 삭제.
- 비교 기준: 같은 실행에서 `SELECT * FROM users WHERE age = <없는 값>`(전체 스캔 한 번) 시간.

제외:

- HTTP 부하 측정과 서버 벤치마크(`server/benchmarks/run.py`). gate 보유 시간은 C 실행 시간으로 충분히 알 수 있고, 2026-11-16 사용자 지시(최소 검증)에 따라 전체 matrix는 돌리지 않는다.
- `fsync` 유무 비교. 결정에 필요한 값은 "SELECT가 멈추는 총 시간"이지 그 안의 비중이 아니다.
- 반복 통계(p99 등). 크기·문장마다 3회 실행해 최솟값·중앙값만 적는다.

## 3. 방법

`tests/benchmark.c`에 측정 모드 하나를 추가한다. 새 실행 파일은 만들지 않는다.

```text
build/benchmark <빈 임시 디렉터리> modify <행 수>
```

1. 기존 `run_multi_insert`처럼 묶음 INSERT로 `<행 수>`만큼 채운다. 채우는 시간은 재지 않는다.
2. 위 문장 4종과 비교용 SELECT를 3회씩 실행해 각 시간(ms)을 출력한다.
   - 한 행 변경: 매번 같은 행을 바꾼다.
   - 한 행 삭제: 매번 다른 ID를 지운다. 파일 크기는 거의 같다.
3. 결과 형식: `<문장> rows=<N>: min <ms> median <ms>`.

실행 환경: 기존 검증과 같은 컨테이너(`mini-dbms-phase3-review:3b19667`). 데이터는 컨테이너 안 `/tmp`에 둔다.

- Docker Desktop VM의 overlay 파일시스템은 실제 디스크와 `fsync` 비용이 다를 수 있다. 결과에 환경을 적고, 같은 측정을 Windows 호스트 실행 파일(MinGW 교차 빌드)로 100만 행 한 번만 더 재서 차이를 확인한다.

## 4. 판단 기준

100만 행 "한 행 변경" 중앙값 T로 다음 작업을 정한다.

| T | 판단 |
|---|---|
| 100 ms 이하 | 12·14번 보류. 한계로만 기록한다. |
| 100 ms 초과 ~ 1 s | 12번 진행 여부를 사용자에게 묻는다. |
| 1 s 초과 | 12번 진행을 제안한다. 12번 뒤에도 commit 구간이 100 ms를 넘으면 14번을 검토한다. |

100 ms는 현재 측정된 읽기 p99(약 1.4 ms, [README](../../../README.md))의 약 70배로, "UPDATE 한 번에 모든 SELECT가 눈에 띄게 멈춘다"고 볼 수 있는 경계로 정했다. 사용자가 다른 기준을 주면 그것을 따른다.

## 5. 작업 단계

1. `tests/benchmark.c`에 `modify` 모드 추가(기존 모드 동작은 그대로).
2. 컨테이너에서 `make build/benchmark` 빌드 경고 0건 확인(`make bench`는 기존 100만 행 INSERT 측정을 실행하므로 쓰지 않는다).
3. 1만·10만·100만 행 측정, Windows 호스트 100만 행 1회.
4. 결과와 판단을 이 문서 "결과" 절에 기록하고 원시 출력은 `docs/phase3-benchmarks/20261118-modify/`에 텍스트로 저장한다.

## 6. 테스트

- 실행: `make build/benchmark` 빌드(경고 0건). 측정 자체가 `modify` 모드의 동작 확인이다(각 문장의 `affected` 행 수를 출력해 기대값과 다르면 0이 아닌 종료 코드).
- 실행하지 않음: `make test`, sanitizer, 서버 테스트. 제품 코드를 바꾸지 않는다.

## 7. 완료 기준

- 측정값 표(행 수 × 문장 × min/median)와 환경이 기록돼 있다.
- 4절 기준에 따른 12·14번 진행 판단이 기록돼 있다.

## 8. 결과 (2026-11-18)

원시 출력: [results.txt](../../phase3-benchmarks/20261118-modify/results.txt). 각 3회, 값은 중앙값(ms).

컨테이너(`mini-dbms-phase3-review:3b19667`, `/tmp` overlay):

| 문장 | 1만 행 | 10만 행 | 100만 행 |
|---|---:|---:|---:|
| `update_id_hit` (한 행 변경) | 8.8 | 30.1 | **242.3** |
| `update_age_miss` (0행, 전체 읽기) | 1.7 | 15.7 | 174.5 |
| `delete_id_miss` (인덱스에서 끝남) | 0.000 | 0.000 | 0.000 |
| `delete_id_hit` (한 행 삭제) | 12.2 | 28.8 | 228.7 |
| `select_scan` (비교: 전체 스캔) | 0.4 | 4.6 | 46.0 |

Windows 10 호스트(NTFS, MinGW-w64 `-static` 빌드), 100만 행 1회 실행:

| 문장 | 100만 행 |
|---|---:|
| `update_id_hit` | **536.2** |
| `update_age_miss` | 443.7 |
| `delete_id_miss` | 0.000 |
| `delete_id_hit` | 518.3 |
| `select_scan` | 51.7 |

빌드: `make build/benchmark` 경고 0건, MinGW 교차 빌드 경고 0건. 모든 문장의 결과 행 수가 기대값과 같았다(다르면 종료 코드 1).

### 해석

- **비용은 행 수에 거의 비례한다.** 100만 행 한 행 변경은 컨테이너 242 ms, Windows 536 ms다. 서버에서는 이 시간 동안 모든 SELECT·INSERT가 멈춘다.
- **가장 큰 몫은 `fsync`가 아니라 임시 파일 쓰기다.** 근거:
  - `update_age_miss`는 일치하는 행이 0개라 교체도 `fsync`도 하지 않는다. 그런데도 같은 파일을 읽기만 하는 `select_scan`보다 컨테이너에서 128 ms, Windows에서 392 ms 더 걸린다.
  - 이 차이는 행마다 `fprintf`로 임시 파일을 쓰고 새 인덱스를 만드는 비용이다. 결국 버리는 작업이다.
  - 교체·`fsync`·다시 열기는 `update_id_hit − update_age_miss`로 컨테이너 약 68 ms, Windows 약 93 ms다.
- `WHERE id` 미스는 측정 하한(1 µs) 미만이다. 10번 후속이 의도대로 동작한다.

### 판단

4절 기준을 적용하면 T = 242 ms(컨테이너) / 536 ms(Windows)로 **"100 ms 초과 ~ 1 s: 12번 진행 여부를 사용자에게 묻는다"**에 해당한다.

측정으로 12번보다 싼 후보도 드러났다. 임시 파일 쓰기 비용을 줄이는 것이다.

- 일치하는 첫 행 전까지는 임시 파일을 쓰지 않는다.
- 바뀌지 않은 행은 형식화(`fprintf`) 없이 원래 바이트를 블록 단위로 복사한다.
- 기대 효과:
  - `update_age_miss`는 `select_scan` 수준(약 46 ms)으로 줄어든다.
  - 한 행 변경도 형식화 비용(위 128 ms의 대부분)이 줄어든다.
- 12번과 달리 잠금 구조를 바꾸지 않는다.

사용자 결정이 필요한 선택지:

| 선택지 | 줄이는 것 | 크기 |
|---|---|---|
| (a) 임시 파일 쓰기 최적화 (위 후보, 새 계획) | UPDATE·DELETE 자체 시간. 따라서 SELECT가 멈추는 시간도 줄어든다 | 작음~중간. 저장소만 |
| (b) 12번 두 단계 재작성 | SELECT가 멈추는 시간만 줄인다(확정 단계 약 70~90 ms로). INSERT는 그대로 기다린다 | 중간. 저장소·실행기·서버 잠금 |
| (c) 둘 다 보류 | — | — |

### 후속 (2026-11-18)

사용자가 선택지 (a)를 골라 [16번](16-rewrite-copy.md)으로 진행했다. 100만 행 한 행 변경이 컨테이너 242 → 85 ms, Windows 536 → 146 ms로 줄었다. 4절 기준(컨테이너)으로 12·14번은 보류다.
