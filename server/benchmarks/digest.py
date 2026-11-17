"""Print one line per case: throughput, per-class latency/errors, per-kind queue share."""
import glob, json, sys

for root in sys.argv[1:]:
    for path in sorted(glob.glob(f'{root}/*/summary.json')):
        s = json.load(open(path))
        d = s['metrics_delta']
        out = {'case': s['name'], 'rps': round(s['successful_rps'], 1),
               'offered': round(s['offered_rps']), 'lag_p99_ms': round(s['generator_lag_p99_ms'], 1),
               'errors': s['errors']}
        for kind, c in s['classes'].items():
            out[kind] = {'n': c['successful'], 'p50': round(c['p50_ms'] or 0, 2), 'p99': round(c['p99_ms'] or 0, 2),
                         'sched_p99': round(c['scheduled_p99_ms'] or 0, 1), 'err': c['errors']}
        for kind, k in d.get('request_kinds', {}).items():
            if k['count']:
                out['srv_' + kind] = {'n': k['count'], 'qwait_ms': round(k['queue_wait_us'] / k['count'] / 1000, 3),
                                      'total_ms': round(k['total_us'] / k['count'] / 1000, 3),
                                      'q_share': round(k['queue_wait_us'] / max(1, k['total_us']), 3),
                                      'qmax_ms': round(s['metrics_after']['request_kinds'][kind]['max_queue_wait_us'] / 1000, 1)}
        lw = d['engine'].get('lock_wait', {})
        out['gate'] = {n: round(v['total_us'] / max(1, v['count']) / 1000, 3) for n, v in lw.items()}
        out['queued_max'] = max((x['metrics']['pool']['queued'] for x in s['samples']), default=None)
        out['expired'] = d['pool'].get('expired')
        print(json.dumps(out))
