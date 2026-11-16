# 반복 측정 집계

각 셀의 RPS와 지연은 반복 3회의 중앙값이다. RPS 범위도 함께 보관한다. 지연은 성공 응답만 대상으로 하므로 오류 수를 함께 확인한다.

| 데이터 | 부하 | W | 도착률 | 서버 | RPS 중앙값 [최소–최대] | 오류 합계 | 미제출 합계 | client CPU 코어 중앙값 |
|---|---|---:|---|---|---:|---|---:|---:|
| fair | write90 | 8 | closed | parallel | 13,089.633 [12,964.100–13,474.433] | {} | 0 | 2.029 |
| fair | write99 | 8 | closed | parallel | 13,170.100 [12,862.900–13,280.900] | {} | 0 | 1.993 |
| scan | scan_write | 2 | closed | parallel | 6.667 [6.500–6.833] | {'QUEUE_TIMEOUT': 3} | 0 | 0.003 |
| scan | scan_write | 2 | closed | serial | 3.767 [3.600–3.800] | {'QUEUE_TIMEOUT': 222} | 0 | 0.002 |
| scan | scan_write | 4 | closed | parallel | 11.733 [11.600–11.767] | {} | 0 | 0.004 |
| scan | scan_write | 4 | closed | serial | 3.700 [3.400–3.867] | {'QUEUE_TIMEOUT': 197} | 0 | 0.002 |
| scan | scan_write | 8 | closed | parallel | 18.133 [17.767–18.500] | {} | 0 | 0.008 |
| scan | scan_write | 8 | closed | serial | 3.667 [3.467–3.767] | {'QUEUE_TIMEOUT': 120} | 0 | 0.002 |
| scan-w1 | scan_write | 1 | closed | parallel | 3.733 [3.700–3.800] | {'QUEUE_TIMEOUT': 242} | 0 | 0.002 |

요청 종류별 반복 결과는 [classes.csv](classes.csv), 공정성 구간 분석은 [fairness.csv](fairness.csv)에 있다. 각 case 디렉터리의 `summary.json`에는 서버 지표와 표본 수가, `requests.jsonl.gz`에는 원시 요청 기록이 있다.
