#!/usr/bin/env python3
"""Temporary CRIU CPU affinity and observation on one migration host."""
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

CONTROL = Path('/run/swiftbaton-ae/criu-cpus')
AUDIT = CONTROL.with_name('criu-cpu-audit.jsonl')


def cpulist(text):
    values = set()
    for part in text.strip().split(','):
        first, _, last = part.partition('-')
        values.update(range(int(first), int(last or first) + 1))
    return values


def choose(count, numa):
    allowed = cpulist(Path('/sys/devices/system/cpu/online').read_text())
    allowed &= os.sched_getaffinity(0)
    if numa is not None:
        allowed &= cpulist(Path(f'/sys/devices/system/node/node{numa}/cpulist').read_text())
    rows = subprocess.check_output(['lscpu', '-p=CPU,CORE,SOCKET,NODE'], text=True)
    seen, result, topology = set(), [], []
    for row in rows.splitlines():
        if row.startswith('#'): continue
        cpu, core, socket, node = map(int, row.split(','))
        if cpu not in allowed or (socket, core) in seen: continue
        seen.add((socket, core))
        result.append(cpu)
        topology.append(dict(cpu=cpu, core=core, socket=socket, numa=node))
        if len(result) == count: break
    if len(result) != count: raise RuntimeError('Not enough distinct available physical cores')
    return {'cpus': result, 'topology': topology, 'online_allowed': sorted(allowed)}


def criu_pids():
    result = []
    for p in Path('/proc').iterdir():
        if not p.name.isdigit(): continue
        try:
            name = (p / 'comm').read_text().strip()
            if name == 'criu' or name.startswith('criu:'): result.append(int(p.name))
        except (OSError, ProcessLookupError): pass
    return result


def install(cpus, binary):
    if not cpus: raise RuntimeError('CPU mask must be nonempty')
    if criu_pids(): raise RuntimeError('CRIU already running')
    if CONTROL.exists() or AUDIT.exists(): raise RuntimeError('Existing CPU control: recover its owner first')
    CONTROL.parent.mkdir(mode=0o700, exist_ok=True)
    st = CONTROL.parent.stat()
    if st.st_uid or st.st_mode & 0o022: raise RuntimeError('Unsafe control directory')
    text = ','.join(map(str, cpus)) + '\n'
    try:
        with AUDIT.open('x') as f: os.fchmod(f.fileno(), 0o600)
        with CONTROL.open('x') as f:
            os.fchmod(f.fileno(), 0o600)
            f.write(text)
        p = subprocess.run([binary, '--version'], text=True, capture_output=True, timeout=10)
        rows = [json.loads(line) for line in AUDIT.read_text().splitlines()]
        if p.returncode or len(rows) != 1 or rows[0]['cpus'] != text.strip():
            raise RuntimeError('CRIU lacks startup CPU-limit support: ' + p.stderr)
        # Preserve the probe in the audit; the observer identifies it as a probe.
        return {'probe': rows[0], 'binary': binary}
    except BaseException:
        if CONTROL.exists() and CONTROL.read_text() == text: CONTROL.unlink()
        if AUDIT.exists(): AUDIT.unlink()
        raise


def snapshot():
    result = []
    for p in Path('/proc').iterdir():
        if not p.name.isdigit(): continue
        try:
            name = (p / 'comm').read_text().strip()
            is_criu = name == 'criu' or name.startswith('criu:')
            is_worker = name.startswith(('sbk_bg/', 'sbk_ft/', 'sbk_export/'))
            if not (is_criu or is_worker): continue
            for task in (p / 'task').iterdir():
                try:
                    status = (task / 'status').read_text()
                    mask = next(line.split(':', 1)[1].strip() for line in status.splitlines()
                                if line.startswith('Cpus_allowed_list:'))
                    result.append({'pid': int(p.name), 'tid': int(task.name), 'name': name,
                                   'kind': 'criu' if is_criu else 'kernel_worker',
                                   'cpus': sorted(cpulist(mask))})
                except (OSError, StopIteration, ProcessLookupError): pass
        except (OSError, ProcessLookupError): pass
    return result


def monitor(out, cpus):
    # Sampling verifies sustained masks; the startup audit covers short-lived
    # CRIU executions that cannot reliably be caught by polling.
    out.mkdir(parents=True, exist_ok=True)
    stop = out / 'cpu-observer.stop'
    events = out / 'cpu-observer.jsonl'
    expected = set(cpus)
    seen, violations = {}, []
    maximum = {'criu': 0, 'kernel_worker': 0}
    started = time.time_ns()
    (out / 'cpu-observer.ready').write_text(str(os.getpid()))
    with events.open('w') as f:
        while not stop.exists():
            counts = {'criu': 0, 'kernel_worker': 0}
            for row in snapshot():
                counts[row['kind']] += 1
                key = (row['tid'], tuple(row['cpus']))
                if key not in seen:
                    seen[key] = row
                    row['time_ns'] = time.time_ns()
                    f.write(json.dumps(row) + '\n')
                    if expected and not set(row['cpus']) <= expected:
                        violations.append(row)
            for kind in counts: maximum[kind] = max(maximum[kind], counts[kind])
            f.flush()
            time.sleep(.05)
    result = dict(start_ns=started, finish_ns=time.time_ns(), expected_cpus=cpus,
                  sampling_interval_ms=50, maximum_concurrent_threads=maximum,
                  observed_threads=list(seen.values()), violations=violations,
                  note='Only dedicated K dispatch/export workers are included; Redis fault callbacks and NIC IRQs are not CPU-capped.')
    (out / 'cpu-observer-result.json').write_text(json.dumps(result, indent=2) + '\n')


def clear(out, cpus):
    if criu_pids(): raise RuntimeError('CRIU still running; keep CPU control in place')
    rows = [json.loads(line) for line in AUDIT.read_text().splitlines()] if cpus and AUDIT.exists() else []
    if cpus and CONTROL.exists():
        expected = ','.join(map(str, cpus)) + '\n'
        if CONTROL.read_text() != expected: raise RuntimeError('CPU control changed concurrently')
        out.mkdir(parents=True, exist_ok=True)
        (out / 'criu-cpu-startup-audit.json').write_text(json.dumps(rows, indent=2) + '\n')
        CONTROL.unlink()
        if AUDIT.exists(): AUDIT.unlink()
    return rows


def finish(out, cpus):
    out.mkdir(parents=True, exist_ok=True)
    (out / 'cpu-observer.stop').touch()
    result = out / 'cpu-observer-result.json'
    if (out / 'cpu-observer.ready').exists():
        deadline = time.monotonic() + 10
        while not result.exists() and time.monotonic() < deadline: time.sleep(.1)
    rows = clear(out, cpus)
    if not result.exists(): raise RuntimeError('CPU observer did not finish; CPU control removed')
    d = json.loads(result.read_text())
    d['startup_audit'] = rows
    d['control_removed'] = not CONTROL.exists() and not AUDIT.exists()
    d['ok'] = not d['violations'] and d['control_removed'] and bool(d['maximum_concurrent_threads']['criu'])
    if cpus:
        expected = ','.join(map(str, cpus))
        d['ok'] &= len(rows) > 1 and all(row['cpus'] == expected for row in rows)
    result.write_text(json.dumps(d, indent=2) + '\n')
    return d


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=['choose', 'install', 'start', 'monitor', 'finish', 'clear'])
    p.add_argument('--cores', type=int, default=2)
    p.add_argument('--numa', type=int)
    p.add_argument('--cpus', default='')
    p.add_argument('--binary')
    p.add_argument('--out', type=Path)
    a = p.parse_args()
    cpus = sorted(cpulist(a.cpus)) if a.cpus else []
    if a.action == 'choose': result = choose(a.cores, a.numa)
    elif a.action == 'install': result = install(cpus, a.binary)
    elif a.action == 'monitor': monitor(a.out, cpus); return
    elif a.action == 'start':
        if not cpus and (CONTROL.exists() or AUDIT.exists()): raise RuntimeError('Unrestricted run has an existing CPU control')
        a.out.mkdir(parents=True, exist_ok=True)
        with (a.out / 'cpu-observer.log').open('w') as f:
            child = subprocess.Popen([sys.executable, __file__, 'monitor', '--out', str(a.out),
                                      '--cpus', a.cpus], stdout=f, stderr=subprocess.STDOUT,
                                     start_new_session=True)
        deadline = time.monotonic() + 10
        while not (a.out / 'cpu-observer.ready').exists() and time.monotonic() < deadline:
            if child.poll() is not None: raise RuntimeError('CPU observer failed')
            time.sleep(.1)
        if not (a.out / 'cpu-observer.ready').exists(): raise RuntimeError('CPU observer startup timeout')
        result = {'pid': child.pid}
    elif a.action == 'clear': result = {'audit': clear(a.out, cpus)}
    else: result = finish(a.out, cpus)
    print(json.dumps(result))


if __name__ == '__main__': main()
