# Phase 3 HTTP 성능 비교

운영 서버와 동일한 W개의 reader를 유지하면서 DB 실행만 직렬화한 비교 타깃을 제공한다. 운영 실행 파일의 기본 정책은 쓰기 우선 공유 gate다. `minidb_server_fair`는 도착 순서(티켓) gate 후보를 쓰는 비교 타깃이다.

```sh
cmake -S server -B build/server -G Ninja -DCMAKE_BUILD_TYPE=Release -DMINIDB_BUILD_BENCHMARKS=ON
cmake --build build/server --parallel 4
python3 server/benchmarks/run.py --binary build/server/minidb_server \
  --serial-binary build/server/minidb_server_serial \
  --output /tmp/minidb-estimate --source-id '<commit-or-snapshot>' --estimate-only
```

정식 기본 matrix는 기존 ID, 신규 ID, 없는 ID, 스캔/쓰기 혼합, 쓰기 50/90/99%의 7개 부하 × worker 1/2/4/8 × 두 서버 × 3회다. 5초 워밍업과 30초 측정만 합쳐 5,880초이며 시작·데이터 준비·정리 시간은 별도다. 임시 DB는 실행 환경의 임시 파일시스템에 만들고 매 조건마다 새 fixture를 복사한다. 부하 생성은 기본 4개 프로세스(`--processes`)에 총 32개 연결(`--clients`)을 나누고 공통 시작 시각으로 동기화한다.

```sh
# 짧은 동작 확인: 성능/백분위 결론에 사용하지 않는다.
python3 server/benchmarks/run.py --binary build/server/minidb_server \
  --serial-binary build/server/minidb_server_serial --rows 1000 --workers 2 \
  --duration .2 --warmup .1 --repeats 1 --output /tmp/minidb-smoke --source-id '<snapshot>'

# 기본 정식 matrix: 고정 동시성, 연결 32개 재사용
python3 server/benchmarks/run.py --binary build/server/minidb_server \
  --serial-binary build/server/minidb_server_serial \
  --output docs/phase3-benchmarks/<run-id>/closed --source-id '<snapshot>'

# pilot 결과에 맞춰 낮은 부하/포화 근처/과부하의 동일 도착률을 선택한다.
python3 server/benchmarks/run.py --binary build/server/minidb_server \
  --serial-binary build/server/minidb_server_serial --workers 4 --scenarios write90 \
  --rates <low-rps> <near-rps> <overload-rps> \
  --output docs/phase3-benchmarks/<run-id>/open --source-id '<snapshot>'
```

`--old-binary`로 다른 커밋에서 빌드한 서버를 같은 실행 안에서 번갈아 측정할 수 있다. 예를 들어 인덱스 복제 구조(`1477e83`)와 단일 인덱스 구조를 `--scenarios scan_write --workers 1 8`로 비교했다. 결과는 `minimal-report.md` B4 절에 있다. 인덱스 복제 구조의 결과에만 `engine.fallbacks` 계열 지표가 있으며, `report.py`는 그 지표가 있을 때만 검사한다.

`--variant NAME=ARGS`는 `--binary` 서버를 서버 옵션만 바꿔 한 번 더 측정한다(여러 번 줄 수 있음). 예: `--variant "ww2=--write-workers 2"`. 다른 변형과 번갈아 실행된다.

`bulk90` 부하는 요청의 90%가 100행 INSERT, 10%가 ID 단건 조회다. 단일 행 INSERT로는 이 부하 생성기가 서버보다 먼저 한계에 닿아 큐가 쌓이지 않으므로, 쓰기 과부하 조건을 만들 때 쓴다. 초당 약 100만 행이 쌓이고 클라이언트가 확인용 ID를 모두 메모리에 들고 있으므로 측정 시간을 짧게(5초 정도) 둔다.

`digest.py <결과 디렉터리>...`는 조건마다 처리량, 종류별 지연·오류, 서버의 종류별 큐 대기 비중(`request_kinds`), gate 평균 대기를 한 줄 JSON으로 출력한다.

## 결과 해석

- 기본 fixture는 ID가 짝수인 100만 행이다. 없는 ID는 gap/음수/0/미래 ID로 나누고, 스캔은 파일 전체를 읽되 마지막 행에만 일치하는 age 조건을 사용한다.
- 요청별 결과를 검사하고 모든 응답 성공 INSERT가 실제 파일에 남았는지 종료 후 확인한다. 부하 생성 중 검증·계측 비용이 포함되므로 클라이언트가 최대 처리량을 제한할 수 있다.
- 성공 지연 p50/p95/p99는 nearest-rank 방식이다. 성공 지연에서 제외된 오류/거절/timeout 수를 반드시 함께 본다. 클래스별 표본이 적으면 특히 p99 결론을 유보한다.
- `successful_rps`와 `completed_rps`는 측정 구간 안에 끝난 요청 기준이다. drain 이후 성공 수와 전체 경과 시간은 따로 기록한다.
- 고정 도착률에서는 예정/실제 제출/완료 시각, 예정 시각 기준 지연과 generator lag를 기록한다. 최대 동시 outstanding은 연결 수의 두 배다. 측정 종료 후 10초까지 제출하지 못한 요청은 `not_submitted`에 남기며 서버 거절로 세지 않는다.
- 클라이언트 요청 timeout은 10초다. timeout 표본은 완료 시각을 알 수 없는 관측이며 성공 지연에 섞지 않는다. 연결 실패 뒤 다음 요청은 재연결하지만 실패한 SQL을 자동 재시도하지 않는다.
- 원시 응답 기록은 측정 중 클라이언트 메모리에 보관하고 모든 client가 측정을 끝낸 뒤 gzip JSONL로 쓴다. 각 프로세스의 원시 표본을 합쳐 백분위를 계산하며 프로세스별 p99를 평균내지 않는다. 저장 시 gzip 압축 수준 1과 여러 gzip member의 연결을 사용해 중복 압축을 피한다. Python `gzip.open`으로 전체를 읽을 수 있다. 메모리 여유가 필요하다. 각 case의 server log, summary JSON, 원시 요청 기록과 전체 summary CSV/environment JSON을 저장한다.
- `client_cpu_fraction`은 client CPU초의 합을 측정 경과 시간으로 나눈 값이며 프로세스 수만큼 커질 수 있다. 단일 프로세스 파일럿에서 이 값이 약 1로 관찰되어 다중 프로세스 경로를 추가했다. 이 변경만으로 generator 병목이 완전히 없어졌다고 간주하지 않는다.
- `/metrics`와 `/proc/<pid>/status`는 1초 간격으로 관찰한다. snapshot의 max 값과 VmHWM은 준비/워밍업/시작을 포함한 실행 전체 최대값이다. `metrics_delta`는 가산 카운터만 차분하며 최대값·현재 gauge를 차분하지 않는다.
- `--source-id`, 실제 환경/명령/이미지 정보와 실행하지 않은 조건을 보고서에 남긴다. 컨테이너에서 측정하면 호스트 CPU·메모리 제한과 데이터 파일시스템도 기록한다.

서버 내부 지표는 지연의 원인을 설명하는 용도이며, `peak_executions`만으로 속도 향상을 결론내리지 않는다. 무응답 reader가 있는 실행은 완료된 reader만 골라 공정성이 개선됐다고 해석하지 않는다.
