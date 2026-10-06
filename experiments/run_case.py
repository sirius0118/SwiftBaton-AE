#!/usr/bin/env python3
"""Run one experiment directory on Node 2; keep every trial independently auditable."""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

EXPERIMENTS = Path(__file__).resolve().parent
ROOT = EXPERIMENTS.parent
WORK = Path(os.environ.get('SB_BENCH_RESULTS', ROOT.parent / (ROOT.name + '-benchmark-results'))).resolve()
COMMON = ['--image-rdma', '--u-precopy', '--fast-cutover', '--buffered-cutover',
          '--network-lock', 'nftables', '--runtime-snapshot', '--numa-node', '0',
          '--poststeady-seconds', '30', '--vma-cache']
MODE_ARGS = {
    'k': ['--kernel-transfer', '--kernel-ps-arm', '--kernel-ps-mr',
          '--kernel-export-workers', '16', '--kernel-catalog-workers', '16',
          '--validation-workers', '16', '--kernel-export-chunk-mb', '64',
          '--precopy-limit-mb', '64', '--fault-workers', '2',
          '--prefetch-workers', '1', '--install-workers', '4'],
    'u': ['--parent-stage', '--parallel-transfer',
          '--precopy-workers', '4', '--validation-workers', '8',
          '--precopy-limit-mb', '8192', '--stage-max-mb', '64',
          '--prefetch-window', '4', '--fault-workers', '1',
          '--prefetch-workers', '1', '--copy-workers', '4',
          '--install-workers', '4', '--batch-pages', '16', '--compact-bg-wire'],
}


def run(argv, **kwargs):
    return subprocess.run(argv, text=True, check=True, **kwargs)


def remote(host, argv, check=True, timeout=120):
    command = argv if host == 'node2' else ['ssh', '-oBatchMode=yes', host, shlex.join(argv)]
    p = subprocess.run(command, text=True, capture_output=True, timeout=timeout)
    if check and p.returncode:
        raise RuntimeError(f'{host}: {shlex.join(argv)}: {p.stderr or p.stdout}')
    return p.stdout.strip()


def ensure_build_path(mode, kind):
    if kind in ('mysql', 'voltdb'):
        binary = ROOT / 'build' / ('criu-' + mode.upper() + '-filelocks') / 'criu/criu'
        target_ready = remote('node3', ['sh', '-c',
                                       'test -x ' + shlex.quote(str(binary)) + ' && echo yes'], check=False)
        if not binary.is_file() or target_ready != 'yes':
            run([sys.executable, str(EXPERIMENTS / 'build_filelocks_criu.py'), mode], timeout=1800)
        hashes = {h: remote(h, ['sha256sum', str(binary)]).split()[0] for h in ('node2', 'node3')}
        if len(set(hashes.values())) != 1:
            raise RuntimeError('Database CRIU binaries differ across hosts')
        return str(binary), hashes['node2']
    binary = ROOT / 'build' / ('criu-' + mode.upper()) / 'criu/criu'
    if not binary.is_file():
        raise RuntimeError('Build the repository source first: bash scripts/build.sh ' + mode.upper())
    local_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
    target_hash = remote('node3', ['sha256sum', str(binary)], check=False)
    if not target_hash or target_hash.split()[0] != local_hash:
        raise RuntimeError('Stage the matching CRIU binary on node3: python3 scripts/deploy.py --execute')
    return str(binary), local_hash


def swap_criu_link(host, binary):
    temporary = '/usr/bin/criu.ae-tmp-' + str(os.getpid())
    remote(host, ['sudo', '-n', 'ln', '-sfn', binary, temporary])
    try:
        remote(host, ['sudo', '-n', 'mv', '-Tf', temporary, '/usr/bin/criu'])
    finally:
        remote(host, ['sudo', '-n', 'rm', '-f', temporary], check=False)


def select_criu(binary, expected):
    """Select a reviewed binary on both hosts and return exact old symlink targets."""
    before = {}
    for host in ('node2', 'node3'):
        current = remote(host, ['readlink', '/usr/bin/criu'])
        if not current: raise RuntimeError(host + ' CRIU is not a symlink')
        before[host] = current
        pids = remote(host, ['pgrep', '-x', 'criu'], check=False)
        if pids: raise RuntimeError(host + ' already has active CRIU processes: ' + pids)
        if remote(host, ['sha256sum', binary]).split()[0] != expected:
            raise RuntimeError(host + ' binary changed during preflight')
    changed = []
    try:
        for host in before:
            swap_criu_link(host, binary)
            changed.append(host)
            if remote(host, ['sha256sum', '/usr/bin/criu']).split()[0] != expected:
                raise RuntimeError(host + ' installed CRIU hash differs')
    except BaseException:
        restore_criu(before, changed)
        raise
    return before


def restore_criu(before, hosts=None):
    for host in reversed(hosts or list(before)):
        swap_criu_link(host, before[host])


def ensure_image(kind, dry_run=False):
    from common.workload import image
    ref = image(kind)
    if dry_run: return ref
    if not remote('node2', ['docker', 'image', 'inspect', '--format', '{{.Id}}', ref], check=False):
        dockerfile = EXPERIMENTS / 'real_world' / kind / 'Dockerfile'
        if not dockerfile.exists(): raise RuntimeError(f'No image {ref} and no Dockerfile {dockerfile}')
        if kind == 'largecontainer':
            run(['gcc', '-O3', '-std=gnu11', '-Wall', '-Wextra', '-static', '-pthread',
                 str(dockerfile.parent / 'largecontainer.c'), '-lm',
                 '-o', str(dockerfile.parent / 'largecontainer-static')], timeout=120)
        run(['docker', 'build', '-t', ref, '-f', str(dockerfile), str(dockerfile.parent)], timeout=1800)
    source_id = remote('node2', ['docker', 'image', 'inspect', '--format', '{{.Id}}', ref])
    target_id = remote('node3', ['docker', 'image', 'inspect', '--format', '{{.Id}}', ref], check=False)
    if target_id != source_id:
        # Stream directly to avoid a large intermediate image file on Node 3.
        producer = subprocess.Popen(['docker', 'save', ref], stdout=subprocess.PIPE)
        consumer = subprocess.Popen(['ssh', '-oBatchMode=yes', 'node3', 'docker load'], stdin=producer.stdout)
        producer.stdout.close()
        if consumer.wait() or producer.wait(): raise RuntimeError('Image transfer failed: ' + ref)
    if remote('node3', ['docker', 'image', 'inspect', '--format', '{{.Id}}', ref]) != source_id:
        raise RuntimeError('Source/target image ID mismatch: ' + ref)
    return ref


def stage_client(kind):
    if kind != 'largecontainer': return
    source = EXPERIMENTS / 'real_world' / kind / 'client.py'
    dest = str(source)
    remote('node1', ['mkdir', '-p', str(source.parent)])
    run(['rsync', '-az', str(source), 'node1:' + dest])


def ensure_client_binding(kind):
    if kind == 'largecontainer':
        stage_client(kind)
        return
    if kind == 'redis': return
    from common.workload import BINDINGS
    module, binding = BINDINGS[kind]
    destination = ROOT / 'build/benchmark-YCSB'
    marker = destination / module / 'target/classes' / Path(*binding.split('.')).with_suffix('.class')
    source = ROOT / 'YCSB'
    source_hash = None
    version_file = destination / module / '.ae-source-sha256'
    if kind in ('voltdb', 'mysql'):
        java = (source / 'voltdb/src/main/java/site/ycsb/db/voltdb/VoltClient4.java'
                if kind == 'voltdb' else
                source / 'jdbc/src/main/java/site/ycsb/db/JdbcDBClient.java')
        source_hash = hashlib.sha256(java.read_bytes()).hexdigest()
    rebuild = not marker.exists() or (source_hash is not None and
                                      (not version_file.exists() or version_file.read_text().strip() != source_hash))
    if rebuild:
        run(['mvn', '-q', '-pl', module, '-am', 'install', '-DskipTests', '-Dcheckstyle.skip=true'], cwd=source, timeout=1800)
        for part in ('core', module):
            run(['mvn', '-q', '-pl', part, 'dependency:copy-dependencies', '-DincludeScope=runtime'], cwd=source, timeout=1200)
            for child in ('classes', 'dependency'):
                origin = source / part / 'target' / child
                if origin.exists():
                    target = destination / part / 'target' / child
                    target.mkdir(parents=True, exist_ok=True)
                    run(['rsync', '-a', str(origin) + '/', str(target) + '/'])
        if kind == 'mysql':
            driver = Path.home() / '.m2/repository/mysql/mysql-connector-java/8.0.30/mysql-connector-java-8.0.30.jar'
            if not driver.exists(): raise RuntimeError('Missing MySQL JDBC driver: ' + str(driver))
            run(['cp', str(driver), str(destination / 'mysql-connector-java.jar')])
        if source_hash is not None:
            version_file.parent.mkdir(parents=True, exist_ok=True)
            version_file.write_text(source_hash + '\n')
    if not marker.exists(): raise RuntimeError('YCSB binding build did not produce ' + str(marker))
    remote('node1', ['mkdir', '-p', str(destination)])
    run(['rsync', '-az', str(destination / 'core'), str(destination / module),
         *([str(destination / 'mysql-connector-java.jar')] if kind == 'mysql' else []),
         'node1:' + str(destination) + '/'])


def stage_helpers(mode):
    source = ROOT / 'scripts/ae' / mode
    for host in ('node1', 'node3'):
        remote(host, ['mkdir', '-p', str(source)])
        run(['rsync', '-az', '--exclude=__pycache__', str(source) + '/', host + ':' + str(source) + '/'])


def client_result(path):
    """Read the YCSB footer without loading multi-gigabyte failure logs."""
    with path.open('rb') as stream:
        stream.seek(max(0, path.stat().st_size - 1024 * 1024))
        if stream.tell(): stream.readline()
        footer = stream.read().decode(errors='replace')
    counts = {}
    for kind, metric, number in re.findall(r'^\[(READ|UPDATE|READ-FAILED|UPDATE-FAILED)\], '
                                           r'(Operations|Return=ERROR), (\d+)\s*$', footer, re.M):
        counts[(kind, metric)] = int(number)
    successes = sum(counts.get((kind, 'Operations'), 0) for kind in ('READ', 'UPDATE'))
    failures = sum(counts.get((kind, 'Return=ERROR'),
                              counts.get((kind + '-FAILED', 'Operations'), 0))
                   for kind in ('READ', 'UPDATE'))
    if successes + failures == 0:
        raise RuntimeError('YCSB footer has no read/update operation counts: ' + str(path))
    return {'successes': successes, 'failures': failures,
            'failure_fraction': failures / (successes + failures)}


def replace_flags(args, overrides):
    """Replace a single-value option, or append a boolean option."""
    out = list(args)
    for key, value in overrides.items():
        flag = '--' + key.replace('_', '-')
        if flag in out:
            at = out.index(flag)
            if isinstance(value, bool):
                if not value: out.pop(at)
            else: out[at + 1] = str(value)
        elif value is True: out.append(flag)
        elif value is not False and value is not None: out += [flag, str(value)]
    return out


def qos_rate(host):
    output = remote(host, ['sudo', '-n', 'mlnx_qos', '-i', 'ens4f1', '-a'])
    match = re.search(r'tc: 1 ratelimit: (unlimited|[\d.]+ Gbps)', output)
    if not match or output.count('priority:') < 8:
        raise RuntimeError('Cannot read TC1 QoS on ' + host)
    return 0 if match[1] == 'unlimited' else float(match[1].split()[0])


def set_qos(host, gbps):
    value = 0 if gbps >= 100 else gbps
    remote(host, ['sudo', '-n', 'mlnx_qos', '-i', 'ens4f1',
                  '--prio_tc=1,1,1,1,1,1,1,1',
                  '--ratelimit=0,' + format(value, 'g') + ',0,0,0,0,0,0'])
    actual = qos_rate(host)
    if abs(actual - value) > .1: raise RuntimeError(f'{host} QoS expected {value} Gbps, got {actual}')


def invocation(case, variant, mode, smoke):
    kind = case['workload']
    base = dict(case.get('parameters', {}))
    base.update(variant.get('parameters', {}))
    if smoke:
        base.update(records=10000 if kind != 'largecontainer' else 100000,
                    duration=50, warmup=10,
                    large_mib=128, large_workers=4, threads=4)
    args = COMMON + MODE_ARGS[mode]
    args = replace_flags(args, base)
    args = replace_flags(args, {k:v for k,v in variant.get('options', {}).items() if k not in ('qos_gbps', 'criu_cpu_cores')})
    args += ['--ae-workload', kind]
    return [sys.executable, str(ROOT / 'scripts/ae' / mode / 'run_ae.py')] + args


def start_cpu_trial(case, variant, command, binary, dest, hosts, record):
    if not case.get('observe_criu_cpus'): return
    helper = ROOT / 'experiments/common/cpu_limit.py'
    remote('node3', ['mkdir', '-p', str(helper.parent)])
    run(['rsync', '-az', str(helper), 'node3:' + str(helper)])
    cores = int(variant.get('options', {}).get('criu_cpu_cores', 0))
    numa = int(command[command.index('--numa-node') + 1]) if '--numa-node' in command else None
    for host in ('node2', 'node3'):
        topology = json.loads(remote(host, ['sudo', '-n', 'python3', str(helper), 'choose',
                              '--cores', str(cores or 2)] + ([] if numa is None else ['--numa', str(numa)])))
        cpus = topology['cpus'] if cores else []
        out = dest / ('cpu-' + host)
        remote(host, ['mkdir', '-p', str(out)])
        cpus_text = ','.join(map(str, cpus))
        hosts[host] = dict(cpus=cpus, topology=topology, out=str(out))
        # Persist the selected mask before installing the host control.
        (dest / 'experiment.json').write_text(json.dumps(record, indent=2) + '\n')
        if cores:
            hosts[host]['installation'] = json.loads(remote(host,
                ['sudo', '-n', 'python3', str(helper), 'install', '--cpus', cpus_text, '--binary', binary]))
        hosts[host]['observer'] = json.loads(remote(host,
            ['sudo', '-n', 'python3', str(helper), 'start', '--cpus', cpus_text, '--out', str(out)]))
        (dest / 'experiment.json').write_text(json.dumps(record, indent=2) + '\n')


def finish_cpu_trial(hosts, dest):
    helper = ROOT / 'experiments/common/cpu_limit.py'
    errors = []
    for host, info in hosts.items():
        try:
            info['result'] = json.loads(remote(host,
                ['sudo', '-n', 'python3', str(helper), 'finish', '--cpus', ','.join(map(str, info['cpus'])),
                 '--out', info['out']]))
            if host != 'node2':
                run(['rsync', '-az', host + ':' + info['out'] + '/', info['out'] + '/'])
        except Exception as error:
            info['finish_error'] = str(error)
            errors.append(host)
    return not errors and all(info['result']['ok'] for info in hosts.values())


def validate_application_cpus(raw):
    from common.cpu_limit import cpulist
    result = {}
    for phase in ('source-before-workload', 'target-after-workload'):
        data = json.loads((raw / ('runtime-' + phase + '.json')).read_text())
        expected = cpulist(data['host_config']['CpusetCpus'])
        masks = {tid: sorted(row['affinity']) for tid, row in data['tasks'].items()}
        if not masks or any(set(mask) != expected for mask in masks.values()):
            raise RuntimeError('Redis inherited a migration CPU limit: ' + phase + ' ' + str(masks))
        result[phase] = {'container_cpus': sorted(expected), 'task_affinities': masks}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', type=Path, required=True)
    parser.add_argument('--mode', choices=['k','u','both'], default='both')
    parser.add_argument('--trials', type=int, default=5)
    parser.add_argument('--smoke', action='store_true')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--resume', action='store_true',
                        help='Keep matching validated trials and run only missing repetitions')
    parser.add_argument('--variant', action='append', help='Run a named variant; repeat to select several')
    args = parser.parse_args()
    case_file = args.case.resolve() / 'case.json'
    case = json.loads(case_file.read_text())
    if args.trials < 1: parser.error('--trials must be positive')
    modes = ['k','u'] if args.mode == 'both' else [args.mode]
    unknown = set(args.variant or []) - {v['name'] for v in case['variants']}
    if unknown: parser.error('Unknown variant(s): ' + ', '.join(sorted(unknown)))
    variants = [v for v in case['variants'] if not args.variant or v['name'] in args.variant]
    if not variants: parser.error('No matching variant')
    commands = [(v, m, invocation(case, v, m, args.smoke)) for v in variants for m in modes]
    if args.dry_run:
        print(json.dumps({'case': case['name'], 'image': ensure_image(case['workload'], True),
                          'commands': [c for _,_,c in commands]}, indent=2)); return
    expected_host = os.environ.get('SB_AE_NODE2_HOSTNAME')
    if expected_host and os.uname().nodename.split('.')[0] != expected_host:
        raise SystemExit('This command must run on Node 2 (source/coordinator)')
    WORK.mkdir(parents=True, exist_ok=True)
    lock = (WORK / 'cluster.lock').open('w')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    image_ref = ensure_image(case['workload'])
    image_id = remote('node2', ['docker', 'image', 'inspect', '--format', '{{.Id}}', image_ref])
    case_sha256 = hashlib.sha256(case_file.read_bytes()).hexdigest()
    source_revision = run(['git', 'rev-parse', 'HEAD'], cwd=ROOT, capture_output=True).stdout.strip()
    source_dirty = bool(run(['git', 'status', '--porcelain'], cwd=ROOT,
                            capture_output=True).stdout.strip())
    ensure_client_binding(case['workload'])
    if case.get('observe_criu_cpus'):
        schedule = [(v, m, c, trial) for trial in range(1, args.trials + 1)
                    for v, m, c in (commands if trial % 2 else list(reversed(commands)))]
    else:
        schedule = [(v, m, c, trial) for v, m, c in commands for trial in range(1, args.trials + 1)]
    if args.resume:
        planned = {(m, v['name'], trial): c for v, m, c, trial in schedule}
        hashes = {m: ensure_build_path(m, case['workload'])[1] for m in modes}
        completed = set()
        name = re.sub(r'[^a-z0-9_-]+', '-', case['name'].lower())
        for path in sorted((WORK / name).glob('*/experiment.json')):
            saved = json.loads(path.read_text())
            if not saved.get('success'): continue
            key = (saved.get('mode'), saved.get('variant'), saved.get('trial'))
            if key not in planned: continue
            required = {'case_sha256': case_sha256, 'image_id': image_id,
                        'binary_sha256': hashes[key[0]], 'command': planned[key],
                        'smoke': args.smoke, 'driver_rc': 0, 'cleanup_rc': 0,
                        'criu_restored': True, 'qos_restored': True, 'source_dirty': False}
            if case.get('observe_criu_cpus'):
                required.update(cpu_validation_ok=True, application_cpu_validation_ok=True)
            if any(saved.get(field) != value for field, value in required.items()):
                raise SystemExit('Cannot resume with incompatible or unvalidated trial: ' + str(path))
            if key in completed:
                raise SystemExit('Duplicate successful repetition: ' + str(path))
            completed.add(key)
        schedule = [(v, m, c, trial) for v, m, c, trial in schedule
                    if (m, v['name'], trial) not in completed]
        print('BENCH_RESUME=' + json.dumps({'completed': len(completed), 'remaining': len(schedule)}), flush=True)
    for variant, mode, command, trial in schedule:
        stage_helpers(mode)
        binary, digest = ensure_build_path(mode, case['workload'])
        name = re.sub(r'[^a-z0-9_-]+', '-', case['name'].lower())
        tag = time.strftime('%Y%m%d_%H%M%S') + f'-{mode}-{variant["name"]}-{trial}'
        dest = WORK / name / tag
        dest.mkdir(parents=True)
        record = {'case': case['name'], 'workload': case['workload'], 'variant': variant['name'],
                  'mode': mode, 'trial': trial, 'smoke': args.smoke,
                  'command': command, 'binary_sha256': digest,
                  'image': image_ref, 'image_id': image_id,
                  'case_sha256': case_sha256, 'source_revision': source_revision,
                  'source_dirty': source_dirty,
                  'success': False}
        (dest / 'experiment.json').write_text(json.dumps(record, indent=2) + '\n')
        before = None
        prior_qos = None
        state = None
        cpu_hosts = {}
        try:
            requested_qos = float(variant.get('options', {}).get('qos_gbps', 25))
            prior_qos = {host: qos_rate(host) for host in ('node2', 'node3')}
            record['qos_before_gbps'] = prior_qos
            record['qos_requested_gbps'] = requested_qos
            for host in prior_qos: set_qos(host, requested_qos)
            before = select_criu(binary, digest)
            record['cpu_restriction'] = cpu_hosts
            start_cpu_trial(case, variant, command, binary, dest, cpu_hosts, record)
            env = dict(os.environ, SB_AE_WORK_ROOT=str(WORK / '_driver'),
                       SB_AE_IMAGE=ensure_image(case['workload']),
                       SB_AE_CRIU_ROOT=str(Path(binary).parent.parent))
            with (dest / 'driver.log').open('w') as output:
                proc = subprocess.Popen(command, env=env, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, text=True)
                for line in proc.stdout:
                    output.write(line); output.flush(); print(line, end='', flush=True)
                    if line.startswith('STATE='): state = Path(line.strip().split('=',1)[1])
                rc = proc.wait()
            record['driver_rc'] = rc
            if state:
                record['state'] = str(state)
                driver_state = json.loads(state.read_text())
                if rc or not driver_state.get('success'):
                    raise RuntimeError('Migration failed; see ' + str(dest / 'driver.log'))
                if case.get('observe_criu_cpus'):
                    record['application_cpu_validation'] = validate_application_cpus(state.parent)
                    record['application_cpu_validation_ok'] = True
                if case['workload'] != 'largecontainer':
                    record['client_result'] = client_result(state.parent / 'run.log')
                    record['poststeady_result'] = client_result(state.parent / 'poststeady.log')
                    if record['client_result']['failure_fraction'] > .01:
                        raise RuntimeError('YCSB client operation failure rate exceeds 1%')
                    if record['poststeady_result']['failure_fraction'] > .001:
                        raise RuntimeError('YCSB fresh-client failure rate exceeds 0.1%')
                scripts = ['analyze_run.py', 'analyze_recovery.py']
                if case['workload'] != 'largecontainer': scripts.append('analyze_success_gaps.py')
                for script in scripts:
                    out = dest / (script + '.log')
                    with out.open('w') as f:
                        p = subprocess.run([sys.executable, str(ROOT / 'scripts' / script), str(state.parent)],
                                           stdout=f, stderr=subprocess.STDOUT)
                    if p.returncode: raise RuntimeError(script + ' failed; see ' + str(out))
                record['raw_result'] = str(state.parent)
            else: raise RuntimeError('Driver did not emit STATE path')
            record['success'] = True
        except BaseException as exc:
            record['error'] = str(exc)
            raise
        finally:
            if state:
                cleanup = ROOT / 'scripts/ae' / mode / 'cleanup_ae.py'
                with (dest / 'cleanup.log').open('w') as out:
                    p = subprocess.run([sys.executable, str(cleanup), str(state)], stdout=out, stderr=subprocess.STDOUT)
                record['cleanup_rc'] = p.returncode
                if p.returncode: record['success'] = False
            if cpu_hosts:
                try:
                    record['cpu_validation_ok'] = finish_cpu_trial(cpu_hosts, dest)
                    if not record['cpu_validation_ok']: record['success'] = False
                except Exception as e:
                    record['cpu_validation_error'] = str(e)
                    record['success'] = False
            if before:
                try: restore_criu(before); record['criu_restored'] = True
                except Exception as e: record['criu_restored'] = str(e); record['success'] = False
            if prior_qos:
                try:
                    for host, limit in prior_qos.items(): set_qos(host, limit)
                    record['qos_restored'] = True
                except Exception as e: record['qos_restored'] = str(e); record['success'] = False
            (dest / 'experiment.json').write_text(json.dumps(record, indent=2) + '\n')
            print('BENCH_RESULT=' + str(dest / 'experiment.json'), flush=True)
        if not record['success']: raise RuntimeError('Trial failed or cleanup incomplete: ' + str(dest))


if __name__ == '__main__': main()
