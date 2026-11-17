#!/usr/bin/env python3
"""HTTP benchmark; Python stdlib only. See README.md for interpretation limits."""
import argparse
import asyncio
import collections
import csv
import gzip
import http.client
import json
import math
import multiprocessing
import traceback
import os
from pathlib import Path
import platform
import random
import shutil
import socket
import subprocess
import tempfile
import time


SCENARIOS = ('old', 'new', 'missing', 'scan_write', 'write50', 'write90', 'write99', 'bulk90')
# Rows per INSERT statement in the bulk90 scenario.
BULK_ROWS = 100


def percentile(values, fraction):
    if not values:
        return None
    values = sorted(values)
    return values[max(0, math.ceil(len(values) * fraction) - 1)]


def fixture(path, rows):
    with path.open('w', encoding='utf-8', newline='\n') as output:
        for n in range(1, rows + 1):
            output.write(f'{2*n}\tu-{2*n}\t{150 if n == rows else n % 150}\n')


class Connection:
    def __init__(self, port):
        self.port = port
        self.reader = self.writer = None

    async def close(self):
        if self.writer:
            self.writer.close()
            try:
                await self.writer.wait_closed()
            except (OSError, ConnectionError):
                pass
        self.reader = self.writer = None

    async def exchange(self, method, path, body=b''):
        if self.writer is None:
            self.reader, self.writer = await asyncio.open_connection('127.0.0.1', self.port)
        header = (f'{method} {path} HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n'
                  f'Content-Type: application/json\r\nContent-Length: {len(body)}\r\n\r\n').encode()
        self.writer.write(header + body)
        await self.writer.drain()
        raw = await self.reader.readuntil(b'\r\n\r\n')
        lines = raw.decode('latin1').split('\r\n')
        status = int(lines[0].split()[1])
        headers = dict(line.split(':', 1) for line in lines[1:] if ':' in line)
        headers = {k.lower(): v.strip() for k, v in headers.items()}
        if 'content-length' not in headers:
            raise RuntimeError('benchmark expects a fixed-length JSON response')
        value = json.loads(await self.reader.readexactly(int(headers['content-length'])))
        if headers.get('connection', '').lower() == 'close':
            await self.close()
        return status, value


async def get_json(port, path):
    connection = Connection(port)
    try:
        status, value = await asyncio.wait_for(connection.exchange('GET', path), 5)
        if status != 200:
            raise RuntimeError(f'{path}: HTTP {status}')
        return value
    finally:
        await connection.close()


def rss(pid):
    try:
        lines = Path(f'/proc/{pid}/status').read_text().splitlines()
        result = {line.split(':')[0]: int(line.split()[1]) * 1024
                  for line in lines if line.startswith(('VmRSS:', 'VmHWM:'))}
        fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
        result['cpu_seconds'] = (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')
        return result
    except (OSError, ValueError):
        return {}


def difference(after, before):
    if isinstance(after, dict):
        # Maxima/gauges are not additive counters; keep them only in snapshots.
        excluded = {'queued', 'queued_bytes', 'active', 'workers', 'peak_active', 'peak_executions',
                    'current_executions', 'max_queue_wait_us', 'max_us'}
        return {key: difference(value, before.get(key, 0)) for key, value in after.items()
                if key not in excluded and isinstance(value, (dict, int)) and not isinstance(value, bool)}
    if isinstance(before, dict):
        before = 0
    return after - before


async def load(port, scenario, rows, duration, concurrency, rate, seed, prefix, raw_path, sync=None):
    rng = random.Random(seed)
    free = asyncio.Queue()
    connections = [Connection(port) for _ in range(concurrency)]
    for connection in connections:
        # Establish the connection before the measured submission timestamp.
        await connection.exchange('GET', '/health')
        free.put_nowait(connection)
    if sync is not None:
        ready, start_event, start_clock, finish_barrier = sync
        ready.put(True)
        if not await asyncio.to_thread(start_event.wait, 15):
            raise RuntimeError('client start handshake timed out')
        started_phase = start_clock.value
        await asyncio.sleep(max(0, started_phase-time.perf_counter()))
    else:
        started_phase = time.perf_counter()
    client_cpu_start = time.process_time()
    end = started_phase + duration
    records = []
    acknowledged = {}
    serial = 0
    pending = set()
    submit_slots = asyncio.Semaphore(concurrency * 2)

    def action(number):
        write_ratio = {'scan_write': .1, 'write50': .5, 'write90': .9, 'write99': .99, 'bulk90': .9}.get(scenario, 0)
        if rng.random() < write_ratio:
            if scenario == 'bulk90':
                names = [f'{prefix}-{number}-{i}' for i in range(BULK_ROWS)]
                values = ', '.join(f"('{name}', 31)" for name in names)
                return 'write', f'INSERT INTO users (name, age) VALUES {values};', names
            name = f'{prefix}-{number}'
            return 'write', f"INSERT INTO users (name, age) VALUES ('{name}', 31);", name
        if scenario == 'scan_write':
            return 'scan', 'SELECT * FROM users WHERE age = 150;', {'id': str(rows*2), 'name': f'u-{rows*2}', 'age': 150}
        if scenario == 'new':
            n = rng.randrange(100)
            return 'new', f'SELECT * FROM users WHERE id = {rows*2+n+1};', {'id': str(rows*2+n+1), 'name': f'prepared-{n}', 'age': 42}
        if scenario in ('missing', 'missing_future'):
            kind, row_id = ('future', 9223372036854775807) if scenario == 'missing_future' else (
                ('gap', 1), ('negative', -1), ('zero', 0), ('future', 9223372036854775807))[number % 4]
            return kind, f'SELECT * FROM users WHERE id = {row_id};', None
        n = rng.randint(1, rows)
        return 'old', f'SELECT * FROM users WHERE id = {2*n};', {'id': str(2*n), 'name': f'u-{2*n}', 'age': 150 if n == rows else n % 150}

    async def request(number, scheduled, spec):
        kind, sql, expected = spec
        connection = await free.get()
        submitted = time.perf_counter()
        status, code, valid, censored = 0, 'CLIENT_ERROR', False, False
        try:
            status, body = await asyncio.wait_for(connection.exchange(
                'POST', '/query', json.dumps({'sql': sql}).encode()), 10)
            code = 'OK' if status == 200 else body.get('error', {}).get('code', 'HTTP_ERROR')
            if status == 200:
                if kind == 'write':
                    names = expected if isinstance(expected, list) else [expected]
                    row_id = int(body.get('inserted_id', 0))
                    last_id = int(body.get('last_inserted_id', row_id))
                    ids = range(row_id, last_id+1)
                    valid = (body.get('write_outcome') == 'applied' and row_id > rows*2+100 and
                             len(ids) == len(names) and not any(i in acknowledged for i in ids))
                    for i, name in zip(ids, names):
                        acknowledged[i] = name
                else:
                    valid = body.get('rows') == ([] if expected is None else [expected])
                if not valid:
                    code = 'WRONG_RESULT'
        except asyncio.TimeoutError:
            code, censored = 'CLIENT_TIMEOUT', True
            await connection.close()
        except (OSError, ValueError, RuntimeError, asyncio.IncompleteReadError, asyncio.LimitOverrunError):
            await connection.close()
        finally:
            completed = time.perf_counter()
            records.append({'n': number, 'client': prefix, 'kind': kind, 'scheduled_s': scheduled-started_phase,
                            'submitted_s': submitted-started_phase, 'completed_s': completed-started_phase,
                            'latency_ms': (completed-submitted)*1000, 'scheduled_latency_ms': (completed-scheduled)*1000,
                            'generator_lag_ms': max(0, (submitted-scheduled)*1000), 'status': status,
                            'code': code, 'valid': valid, 'censored': censored})
            free.put_nowait(connection)
            submit_slots.release()

    async def closed_worker():
        nonlocal serial
        while time.perf_counter() < end:
            number = serial
            serial += 1
            await submit_slots.acquire()
            await request(number, time.perf_counter(), action(number))

    planned = None
    unsent = 0
    if rate is None:
        await asyncio.gather(*(closed_worker() for _ in range(concurrency)))
    else:
        planned = math.ceil(rate * duration)
        for number in range(planned):
            due = started_phase + number / rate
            await asyncio.sleep(max(0, due-time.perf_counter()))
            try:
                await asyncio.wait_for(submit_slots.acquire(), max(.001, end+10-time.perf_counter()))
            except asyncio.TimeoutError:
                unsent = planned-number
                break
            if time.perf_counter() >= end+10:
                submit_slots.release()
                unsent = planned-number
                break
            task = asyncio.create_task(request(number, due, action(number)))
            pending.add(task)
            task.add_done_callback(pending.discard)
        if pending:
            await asyncio.gather(*pending)
    finished = time.perf_counter()
    client_cpu = time.process_time() - client_cpu_start
    if sync is not None:
        # All clients finish measuring before any process compresses raw data.
        await asyncio.to_thread(finish_barrier.wait, 30)
    for connection in connections:
        await connection.close()
    if raw_path:
        with gzip.open(raw_path, 'wt', encoding='utf-8', compresslevel=1) as output:
            for record in records:
                output.write(json.dumps(record, separators=(',', ':')) + '\n')
    return summarize(records, duration, finished-started_phase, client_cpu, planned, unsent), acknowledged


def summarize(records, duration, elapsed, client_cpu, planned, unsent):
    groups = {}
    for kind in sorted({r['kind'] for r in records}):
        subset = [r for r in records if r['kind'] == kind]
        successful = [r for r in subset if r['valid']]
        groups[kind] = {'submitted': len(subset), 'successful': len(successful),
                        'completed_during_window': sum(r['completed_s'] <= duration for r in subset),
                        'p50_ms': percentile([r['latency_ms'] for r in successful], .5),
                        'p95_ms': percentile([r['latency_ms'] for r in successful], .95),
                        'p99_ms': percentile([r['latency_ms'] for r in successful], .99),
                        'scheduled_p99_ms': percentile([r['scheduled_latency_ms'] for r in successful], .99),
                        'errors': dict(collections.Counter(r['code'] for r in subset if not r['valid']))}
    return {'offered': planned if planned is not None else len(records), 'submitted': len(records),
            'not_submitted': unsent, 'elapsed_with_drain_s': elapsed,
            'client_cpu_s': client_cpu, 'client_cpu_fraction': client_cpu/(elapsed),
            'offered_rps': (planned if planned is not None else len(records))/duration,
            'completed_rps': sum(r['completed_s'] <= duration for r in records)/duration,
            'successful_rps': sum(r['valid'] and r['completed_s'] <= duration for r in records)/duration,
            'success_after_drain': sum(r['valid'] for r in records),
            'generator_lag_p99_ms': percentile([r['generator_lag_ms'] for r in records], .99),
            'errors': dict(collections.Counter(r['code'] for r in records if not r['valid'])),
            'classes': groups}


def client_process(arguments, sync, result_queue):
    try:
        result_queue.put(('ok', asyncio.run(load(*arguments, sync=sync))))
    except BaseException:
        result_queue.put(('error', traceback.format_exc()))


async def distributed_load(port, scenario, args, rate, seed, prefix, raw_path):
    if args.processes == 1:
        return await load(port, scenario, args.rows, args.active_duration, args.clients,
                          rate, seed, prefix, raw_path)
    ctx = multiprocessing.get_context('spawn')
    ready, results = ctx.Queue(), ctx.Queue()
    start_event, start_clock = ctx.Event(), ctx.Value('d', 0.0)
    barrier = ctx.Barrier(args.processes)
    processes, outputs = [], []
    with tempfile.TemporaryDirectory(prefix='minidb-client-') as directory:
        try:
            for n in range(args.processes):
                path = Path(directory) / f'{n}.jsonl.gz'
                outputs.append(path)
                clients = args.clients // args.processes + (n < args.clients % args.processes)
                arguments = (port, scenario, args.rows, args.active_duration, clients,
                             None if rate is None else rate/args.processes,
                             seed+1009*n, f'{prefix}-p{n}', path)
                process = ctx.Process(target=client_process,
                    args=(arguments, (ready, start_event, start_clock, barrier), results))
                process.start()
                processes.append(process)
            for _ in processes:
                await asyncio.to_thread(ready.get, True, 20)
            start_clock.value = time.perf_counter()+.05
            start_event.set()
            summaries, acknowledged = [], {}
            for _ in processes:
                status, value = await asyncio.to_thread(results.get, True, args.active_duration+75)
                if status != 'ok':
                    raise RuntimeError(value)
                summary, inserts = value
                if acknowledged.keys() & inserts.keys():
                    raise AssertionError('duplicate INSERT ID across clients')
                acknowledged.update(inserts)
                summaries.append(summary)
            for process in processes:
                await asyncio.to_thread(process.join, 10)
                if process.exitcode != 0:
                    raise RuntimeError('client failed to exit cleanly')
            records = []
            for path in outputs:
                with gzip.open(path, 'rt', encoding='utf-8') as source:
                    records.extend(json.loads(line) for line in source)
            summary = summarize(records, args.active_duration,
                max(s['elapsed_with_drain_s'] for s in summaries),
                sum(s['client_cpu_s'] for s in summaries),
                None if rate is None else sum(s['offered'] for s in summaries),
                sum(s['not_submitted'] for s in summaries))
            if raw_path:
                # RFC 1952 permits concatenated members; Python gzip reads all.
                # Reuse client output instead of serializing/compressing it twice.
                with raw_path.open('wb') as output:
                    for path in outputs:
                        with path.open('rb') as source:
                            shutil.copyfileobj(source, output)
            return summary, acknowledged
        finally:
            for process in processes:
                if process.is_alive():
                    process.terminate()
                process.join(timeout=5)
                if process.is_alive():
                    process.kill()
                    process.join()
            ready.close()
            results.close()


async def one_case(binary, variant, workers, scenario, repeat, args, seed_file, rate=None):
    name = f'{variant}-w{workers}-{scenario}-r{repeat}-' + ('closed' if rate is None else f'rate{rate:g}')
    output = args.output / name
    output.mkdir(parents=True, exist_ok=False)
    with tempfile.TemporaryDirectory(prefix='minidb-bench-') as directory:
        data_file = Path(directory)/'users.tsv'
        shutil.copyfile(seed_file, data_file)
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        command = [str(binary), '--data-dir', directory, '--port', str(port), '--workers', str(workers),
                   '--io-threads', '2', '--queue-capacity', '256', '--queue-timeout-ms', '5000']
        log = (output/'server.log').open('wb')
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        samples = []
        monitor_running = True
        async def monitor():
            while monitor_running:
                try:
                    metrics = await get_json(port, '/metrics')
                    samples.append({'time': time.perf_counter(), 'memory': rss(process.pid), 'metrics': metrics})
                except (OSError, RuntimeError, asyncio.TimeoutError, asyncio.IncompleteReadError):
                    pass
                await asyncio.sleep(1)
        monitoring = None
        start = time.perf_counter()
        try:
            deadline = start+30
            while True:
                if process.poll() is not None:
                    raise RuntimeError(f'server startup failed: {output}/server.log')
                try:
                    await get_json(port, '/health')
                    break
                except (OSError, RuntimeError, asyncio.TimeoutError, asyncio.IncompleteReadError):
                    if time.perf_counter() > deadline:
                        raise RuntimeError('server startup deadline exceeded')
                    await asyncio.sleep(.02)
            startup = time.perf_counter()-start
            connection = Connection(port)
            for n in range(100):
                status, value = await connection.exchange('POST', '/query', json.dumps(
                    {'sql': f"INSERT INTO users (name, age) VALUES ('prepared-{n}', 42);"}).encode())
                assert status == 200 and value['inserted_id'] == str(args.rows*2+n+1)
            await connection.close()
            args.active_duration = args.warmup
            warm_summary, warm_ids = await distributed_load(port, scenario, args, None, args.seed+repeat, 'warm', None)
            before = await get_json(port, '/metrics')
            monitoring = asyncio.create_task(monitor())
            args.active_duration = args.duration
            summary, ids = await distributed_load(port, scenario, args, rate, args.seed+repeat, 'measure', output/'requests.jsonl.gz')
            assert not summary['errors'].get('WRONG_RESULT') and not warm_summary['errors'].get('WRONG_RESULT'), 'incorrect query result'
            after = await get_json(port, '/metrics')
            memory = rss(process.pid)
            monitor_running = False
            await monitoring
            monitoring = None
            process.terminate()
            await asyncio.to_thread(process.wait, 15)
            if process.returncode != 0:
                raise RuntimeError('server did not shut down cleanly')
            # Check every acknowledged INSERT against persisted data after shutdown.
            acknowledged = {**warm_ids, **ids}
            persisted_new = 0
            with data_file.open(encoding='utf-8') as data:
                for line in data:
                    row_id, row_name, _ = line.rstrip('\n').split('\t')
                    row_id = int(row_id)
                    if row_id > args.rows*2+100:
                        persisted_new += 1
                    if row_id in acknowledged:
                        assert acknowledged.pop(row_id) == row_name, 'persisted name mismatch'
            assert not acknowledged, 'acknowledged INSERT was lost'
            summary.update({'name': name, 'variant': variant, 'workers': workers, 'scenario': scenario,
                            'repeat': repeat, 'rate': rate, 'rows': args.rows, 'duration_s': args.duration,
                            'warmup_s': args.warmup, 'clients': args.clients, 'processes': args.processes, 'startup_s': startup,
                            'memory': memory, 'metrics_before': before, 'metrics_after': after,
                            'metrics_delta': difference(after, before), 'samples': samples,
                            'warmup_summary': warm_summary, 'persisted_new_rows': persisted_new,
                            'command': command})
            (output/'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
            print(json.dumps({'case': name, 'rps': summary['successful_rps'], 'errors': summary['errors'],
                              'lag_p99_ms': summary['generator_lag_p99_ms']}), flush=True)
            return summary
        finally:
            monitor_running = False
            if monitoring:
                await monitoring
            if process.poll() is None:
                process.terminate()
                try:
                    await asyncio.to_thread(process.wait, 15)
                except subprocess.TimeoutExpired:
                    process.kill()
                    await asyncio.to_thread(process.wait)
            log.close()


async def main(args):
    args.output.mkdir(parents=True, exist_ok=True)
    metadata = {'arguments': {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
                'platform': platform.platform(), 'python': platform.python_version(), 'cpu_count': os.cpu_count(),
                'clock': str(time.get_clock_info('perf_counter')), 'source_id': args.source_id,
                'percentile': 'nearest rank of successful responses; errors/censored reported separately',
                'rps_window': 'only completions within measurement duration, excludes drain',
                'raw_write': 'raw JSONL gzip is written after measured load; client holds records in memory'}
    metadata['cgroup'] = {name: Path('/sys/fs/cgroup', name).read_text().strip()
                          for name in ('cpu.max', 'memory.max') if Path('/sys/fs/cgroup', name).exists()}
    (args.output/'environment.json').write_text(json.dumps(metadata, indent=2), encoding='utf-8')
    variants = [('parallel', args.binary)]
    if args.serial_binary:
        variants.append(('serial', args.serial_binary))
    if args.old_binary:
        variants.append(('old', args.old_binary))
    cases = [(w, scenario, repeat, rate) for repeat in range(args.repeats)
             for w in args.workers for scenario in args.scenarios for rate in args.rates]
    estimate = len(cases)*len(variants)*(args.duration+args.warmup)
    print(json.dumps({'cases': len(cases)*len(variants), 'minimum_load_seconds': estimate,
                      'note': 'startup/preparation/drain/result writing are additional'}), flush=True)
    if args.estimate_only:
        return
    summaries = []
    with tempfile.TemporaryDirectory(prefix='minidb-fixture-') as directory:
        seed = Path(directory)/'fixture.tsv'
        fixture(seed, args.rows)
        for index, (workers, scenario, repeat, rate) in enumerate(cases):
            order = variants if index % 2 == 0 else list(reversed(variants))
            for variant, binary in order:
                summaries.append(await one_case(binary, variant, workers, scenario, repeat, args, seed, rate))
                with (args.output/'summary.csv').open('w', newline='', encoding='utf-8') as out:
                    fields = ('name', 'variant', 'workers', 'scenario', 'repeat', 'rate', 'successful_rps',
                              'completed_rps', 'offered_rps', 'not_submitted', 'generator_lag_p99_ms', 'startup_s')
                    writer = csv.DictWriter(out, fieldnames=fields, extrasaction='ignore')
                    writer.writeheader()
                    writer.writerows(summaries)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--serial-binary', type=Path)
    parser.add_argument('--old-binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--source-id', required=True)
    parser.add_argument('--rows', type=int, default=1000000)
    parser.add_argument('--workers', type=int, nargs='+', default=[1, 2, 4, 8])
    parser.add_argument('--scenarios', nargs='+', choices=(*SCENARIOS, 'missing_future'), default=list(SCENARIOS))
    parser.add_argument('--duration', type=float, default=30)
    parser.add_argument('--warmup', type=float, default=5)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--clients', type=int, default=32)
    parser.add_argument('--processes', type=int, default=4)
    parser.add_argument('--seed', type=int, default=7410)
    parser.add_argument('--rates', type=str, nargs='+', default=['closed'], help='closed or fixed requests/second')
    parser.add_argument('--estimate-only', action='store_true')
    args = parser.parse_args()
    args.rates = [None if v == 'closed' else float(v) for v in args.rates]
    if min(args.rows, args.repeats, args.clients, *args.workers) <= 0 or args.duration <= 0 or args.warmup <= 0:
        parser.error('counts and durations must be positive')
    if args.processes < 1 or args.clients < args.processes:
        parser.error('clients must be >= processes >= 1')
    if any(v is not None and v <= 0 for v in args.rates):
        parser.error('rates must be positive')
    args.binary = args.binary.resolve()
    if args.serial_binary:
        args.serial_binary = args.serial_binary.resolve()
    if args.old_binary:
        args.old_binary = args.old_binary.resolve()
    asyncio.run(main(args))
