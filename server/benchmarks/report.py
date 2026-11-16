#!/usr/bin/env python3
"""Summarize completed runs; keep repeat results and examine pending-reader gaps."""
import argparse
import collections
import csv
import gzip
import json
from pathlib import Path
import statistics


def number(value):
    return '—' if value is None else f'{value:,.3f}'


def mean_wait(summary, kind):
    metric = summary['metrics_delta']['engine'].get('lock_wait', {}).get(kind, {})
    return metric.get('total_us', 0) / metric['count'] if metric.get('count') else None


def pending_reader_gaps(raw_path, duration):
    # Intervals start when an actual submitted read is outstanding. A successful
    # read resets progress; failed reads leave the pending set but are not progress.
    events = []
    censored = 0
    with gzip.open(raw_path, 'rt', encoding='utf-8') as source:
        for line in source:
            row = json.loads(line)
            if row['kind'] == 'write':
                if row['valid'] and row['completed_s'] <= duration:
                    events.append((row['completed_s'], 'write'))
            elif row['submitted_s'] < duration:
                events.append((max(0, row['submitted_s']), 'start'))
                if row['completed_s'] <= duration:
                    events.append((row['completed_s'], 'success' if row['valid'] else 'error'))
                if row['censored']:
                    censored += 1
    events.sort()
    pending, writers, start = 0, 0, None
    maximum = {'seconds': 0, 'writes_completed': 0, 'start_s': None, 'end_s': None}

    def observe(end):
        nonlocal maximum
        if start is not None and writers and end-start > maximum['seconds']:
            maximum = {'seconds': end-start, 'writes_completed': writers,
                       'start_s': start, 'end_s': end}

    for at, kind in events:
        if kind == 'start':
            if not pending:
                start, writers = at, 0
            pending += 1
        elif kind == 'write':
            if pending:
                writers += 1
        else:
            pending -= 1
            if kind == 'success' or not pending:
                observe(at)
                start, writers = (at if pending else None), 0
    observe(duration)
    maximum.update({'readers_pending_at_window_end': pending, 'censored_readers': censored})
    return maximum


FULL_MATRIX = {'closed-v2': 168, 'open': 18, 'fallback': 12, 'scan-extended': 6}


def main(root, expected=None):
    expected = expected or FULL_MATRIX
    datasets = tuple(expected)
    all_summaries, groups, rows, fairness = [], collections.defaultdict(list), [], []
    for dataset in datasets:
        for path in sorted((root/dataset).glob('*/summary.json')):
            summary = json.loads(path.read_text())
            # Rerun both variants of first pair after interruption of an older run.
            if dataset == 'closed-v2' and summary['workers'] == 1 and summary['scenario'] == 'old' and summary['repeat'] == 0:
                replacement = root/'replacement'/path.parent.name/'summary.json'
                if not replacement.exists():
                    raise RuntimeError('missing replacement for first comparison pair')
                path = replacement
                summary = json.loads(path.read_text())
            summary['_path'] = str(path.parent.relative_to(root))
            summary['_dataset'] = dataset
            summary['_unacknowledged_persisted'] = summary['persisted_new_rows'] - sum(
                phase['classes'].get('write', {}).get('successful', 0)
                for phase in (summary, summary['warmup_summary']))
            engine = summary['metrics_delta']['engine']
            # Fallback counters exist only in results from the replicated-index
            # design (before plan 06); the single shared index has no fallback.
            if 'fallbacks' not in engine:
                pass
            elif summary['variant'] != 'old' and summary['scenario'] in ('missing', 'missing_future'):
                if engine['fallbacks'] != 0 or engine['fallbacks_skipped'] != summary['success_after_drain']:
                    raise RuntimeError(f"unexpected missing-ID fallback counters: {summary['name']}")
            elif summary['variant'] == 'old' and summary['scenario'] == 'missing_future':
                if engine['fallbacks'] != summary['success_after_drain']:
                    raise RuntimeError(f"legacy baseline did not exercise fallback: {summary['name']}")
            all_summaries.append(summary)
            groups[(dataset, summary['scenario'], summary['workers'], summary['rate'], summary['variant'])].append(summary)
            for kind, values in summary['classes'].items():
                rows.append({'dataset': dataset, 'case': summary['name'], 'kind': kind,
                    'successful_rps': summary['successful_rps'], **values,
                    'read_gate_mean_us': mean_wait(summary, 'read_gate'),
                    'write_gate_mean_us': mean_wait(summary, 'write_gate'),
                    'client_cpu_cores': summary['client_cpu_fraction'], 'raw_directory': summary['_path']})
            if summary['rate'] is None and summary['variant'] == 'parallel' and summary['scenario'] in ('write50', 'write90', 'write99'):
                gap = pending_reader_gaps(path.parent/'requests.jsonl.gz', summary['duration_s'])
                fairness.append({'case': summary['name'], 'workers': summary['workers'],
                    'scenario': summary['scenario'], 'repeat': summary['repeat'], **gap,
                    'read_p95_ms': summary['classes']['old']['p95_ms'],
                    'read_p99_ms': summary['classes']['old']['p99_ms'],
                    'write_p95_ms': summary['classes']['write']['p95_ms'],
                    'write_p99_ms': summary['classes']['write']['p99_ms'],
                    'read_gate_mean_us': mean_wait(summary, 'read_gate'),
                    'write_gate_mean_us': mean_wait(summary, 'write_gate'),
                    'errors': summary['errors'], 'raw_directory': summary['_path']})
    actual = collections.Counter(s['_dataset'] for s in all_summaries)
    if actual != expected:
        raise RuntimeError(f'incomplete matrix: expected {expected}, observed {dict(actual)}')
    if any(len(values) != 3 for values in groups.values()):
        raise RuntimeError('every formal condition must have three independent repetitions')
    for name, records in (('classes.csv', rows), ('fairness.csv', fairness)):
        with (root/name).open('w', newline='', encoding='utf-8') as output:
            writer = csv.DictWriter(output, fieldnames=list(records[0]))
            writer.writeheader()
            writer.writerows(records)
    (root/'fairness.json').write_text(json.dumps(fairness, indent=2), encoding='utf-8')
    lines = ['# 반복 측정 집계', '', '각 셀의 RPS와 지연은 반복 3회의 중앙값이다. RPS 범위도 함께 보관한다. 지연은 성공 응답만 대상으로 하므로 오류 수를 함께 확인한다.', '',
             '| 데이터 | 부하 | W | 도착률 | 서버 | RPS 중앙값 [최소–최대] | 오류 합계 | 미제출 합계 | client CPU 코어 중앙값 |',
             '|---|---|---:|---|---|---:|---|---:|---:|']
    medians = []
    for key, values in sorted(groups.items(), key=lambda item: str(item[0])):
        dataset, scenario, workers, rate, variant = key
        rps = [v['successful_rps'] for v in values]
        errors = collections.Counter()
        for value in values:
            errors.update(value['errors'])
        item = {'dataset': dataset, 'scenario': scenario, 'workers': workers, 'rate': rate,
            'variant': variant, 'rps_median': statistics.median(rps), 'rps_min': min(rps), 'rps_max': max(rps),
            'errors': dict(errors), 'not_submitted': sum(v['not_submitted'] for v in values),
            'unacknowledged_persisted_rows': sum(v['_unacknowledged_persisted'] for v in values),
            'client_cpu_median': statistics.median(v['client_cpu_fraction'] for v in values),
            'lag_p99_median_ms': statistics.median(v['generator_lag_p99_ms'] for v in values),
            'startup_median_s': statistics.median(v['startup_s'] for v in values),
            'rss_median_bytes': statistics.median(v['memory']['VmHWM'] for v in values), 'classes': {}}
        item['lock_mean_us'] = {kind: statistics.median(mean_wait(v, kind) for v in values if mean_wait(v, kind) is not None)
            if any(mean_wait(v, kind) is not None for v in values) else None
            for kind in ('reader_mutex', 'read_gate', 'write_gate', 'insert_writer_mutex', 'fallback_mutex')}
        item['queue_mean_us'] = statistics.median(
            v['metrics_delta']['pool']['queue_wait_us']/v['metrics_delta']['pool']['completed']
            for v in values if v['metrics_delta']['pool']['completed'])
        for kind in values[0]['classes']:
            subsets = [v['classes'][kind] for v in values]
            item['classes'][kind] = {field: statistics.median(s[field] for s in subsets if s[field] is not None)
                if any(s[field] is not None for s in subsets) else None for field in ('p50_ms', 'p95_ms', 'p99_ms', 'scheduled_p99_ms')}
            item['classes'][kind]['successful_min'] = min(s['successful'] for s in subsets)
        medians.append(item)
        lines.append(f"| {dataset} | {scenario} | {workers} | {rate or 'closed'} | {variant} | {number(item['rps_median'])} [{number(min(rps))}–{number(max(rps))}] | {dict(errors)} | {item['not_submitted']} | {number(item['client_cpu_median'])} |")
    lines += ['', '요청 종류별 반복 결과는 [classes.csv](classes.csv), 공정성 구간 분석은 [fairness.csv](fairness.csv)에 있다. 각 case 디렉터리의 `summary.json`에는 서버 지표와 표본 수가, `requests.jsonl.gz`에는 원시 요청 기록이 있다.', '']
    (root/'AGGREGATES.md').write_text('\n'.join(lines), encoding='utf-8')
    (root/'medians.json').write_text(json.dumps(medians, indent=2), encoding='utf-8')
    print(json.dumps({'formal_cases': len(all_summaries), 'fairness_cases': len(fairness),
                      'max_pending_reader_gap_s': max(v['seconds'] for v in fairness)}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--dataset', action='append', metavar='NAME=CASES',
                        help='subdirectory and expected case count; repeatable (default: full matrix)')
    arguments = parser.parse_args()
    expected = None
    if arguments.dataset:
        expected = {}
        for item in arguments.dataset:
            name, _, count = item.partition('=')
            if not name or not count.isdigit():
                parser.error(f'--dataset expects NAME=CASES, got {item!r}')
            expected[name] = int(count)
    main(arguments.root, expected)
