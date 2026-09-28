#!/usr/bin/env python3
"""Run stock CRIU Redis/YCSB migration using only rsocket for image transfer.

Run on Node2. This driver owns only containers with its unique name and
restores both hosts' original /usr/bin/criu symlinks in its finally block.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

BASE = Path(__file__).resolve().parents[2]
COMMON = BASE / 'baseline/common'
STOCK = Path(os.environ.get('SB_STOCK_CRIU', str(BASE / 'baseline/native-criu/.build/upstream-criu/criu/criu')))
WORK = Path(os.environ.get('SB_AE_WORK_ROOT', str(BASE.parent / (BASE.name + '-work'))))
IMAGE = os.environ.get('SB_REDIS_IMAGE', json.loads((BASE / 'configs/lab.json').read_text())['redis_image'])
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--records', type=int, default=100000)
parser.add_argument('--field-length', type=int, default=1024)
parser.add_argument('--threads', type=int, default=16)
parser.add_argument('--duration', type=int, default=60)
parser.add_argument('--warmup', type=int, default=10)
parser.add_argument('--port', type=int, default=6398)
parser.add_argument('--poststeady-seconds', type=int, default=30)
parser.add_argument('--canary-mib', type=int, default=8)
args = parser.parse_args()
if args.records < 1 or not 1 <= args.field_length <= 1048576 or not 1 <= args.threads <= 128:
    parser.error('invalid workload size')
if args.duration < args.warmup + 20 or not 0 <= args.poststeady_seconds <= 120:
    parser.error('duration must exceed warmup by 20 seconds; poststeady must be 0..120')
if not 1024 <= args.port <= 65535:
    parser.error('invalid port')
if not STOCK.is_file() or not COMMON.joinpath('rsocket_relay').is_file():
    parser.error('build stock CRIU and baseline/common/rsocket_relay first')

WORK.mkdir(parents=True, exist_ok=True)
lock = (WORK / 'run.lock').open('w')
fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
name = 'sb_native_' + time.strftime('%Y%m%d_%H%M%S')
out = WORK / name
out.mkdir()
state = {'name': name, 'baseline': 'native-criu-rsocket', 'parameters': vars(args),
         'source_criu': str(STOCK), 'out': str(out), 'success': False}
rdma_port = 23000 + os.getpid() % 20000
switch = COMMON / 'select_criu.py'
source_relays = []
target_relay_pids = []
nat_rule = None
selection = {}
changed_selection = []
created = []
ycsb_pid = None


def save():
    (out / 'state.json').write_text(json.dumps(state, indent=2) + '\n')


def event(kind, **details):
    row = dict(time_ns=time.time_ns(), event=kind, **details)
    with (out / 'events.jsonl').open('a') as log:
        log.write(json.dumps(row) + '\n')
    print(json.dumps(row), flush=True)


def run(host, argv, *, timeout=60, check=True, input_bytes=None):
    command = list(map(str, argv))
    if host != 'knode2':
        command = ['ssh', '-oBatchMode=yes', '-oConnectTimeout=8', host,
                   shlex.join(command)]
    result = subprocess.run(command, input=input_bytes, capture_output=True,
                            timeout=timeout)
    if check and result.returncode:
        raise RuntimeError('%s: %s: exit=%d\n%s' %
                           (host, shlex.join(list(map(str, argv))), result.returncode,
                            result.stderr.decode(errors='replace')[-2000:]))
    return result.stdout.decode(errors='replace').strip()


def script(host, source, argv=(), *, timeout=60):
    return run(host, ['python3', '-c', source, *map(str, argv)], timeout=timeout)


def launch(host, label, argv):
    log = out / (label + '.log')
    status = out / (label + '.status')
    pidfile = out / (label + '.pid')
    child = ('import subprocess,pathlib; f=open(%r,"w"); '
             'p=subprocess.Popen(%r,stdout=f,stderr=subprocess.STDOUT,start_new_session=True); '
             'pathlib.Path(%r).write_text(str(p.pid)); '
             'pathlib.Path(%r).write_text(str(p.wait()))' %
             (str(log), list(map(str, argv)), str(pidfile), str(status)))
    launcher = ('import subprocess; p=subprocess.Popen(["python3","-c",%r],'
                'stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True); '
                'print(p.pid)' % child)
    return int(script(host, launcher))


def await_job(host, label, timeout):
    status = out / (label + '.status')
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = script(host, 'from pathlib import Path;import sys; p=Path(sys.argv[1]);'
                       'print(p.read_text() if p.exists() else "running")', [status])
        if value != 'running':
            data = run(host, ['cat', out / (label + '.log')], timeout=30, check=False)
            (out / (label + '.log')).write_text(data + '\n')
            if value != '0':
                raise RuntimeError(label + ' exited ' + value)
            event(label + '_completed')
            return
        time.sleep(.2)
    raise TimeoutError(label + ' did not complete')


def stage_to_target(local, target):
    with Path(local).open('rb') as source:
        result = subprocess.run(['ssh', '-oBatchMode=yes', 'knode3',
                                 'cat > ' + shlex.quote(str(target))],
                                stdin=source, capture_output=True, timeout=60)
    if result.returncode:
        raise RuntimeError('stage file failed: ' + result.stderr.decode(errors='replace'))


def cleanup():
    cleanup_errors = []
    if ycsb_pid is not None:
        stop_owned = '''import os, signal, sys
from pathlib import Path
root, wrapper = Path(sys.argv[1]), int(sys.argv[2])
pidfile = root / 'run.pid'
targets = []
if pidfile.exists():
    targets.append((int(pidfile.read_text()), (b'site.ycsb.Client', str(root).encode())))
targets.append((wrapper, (str(root).encode(),)))
for pid, markers in targets:
    try:
        command = Path('/proc/%d/cmdline' % pid).read_bytes()
        if all(marker in command for marker in markers):
            os.kill(pid, signal.SIGTERM)
    except (FileNotFoundError, ProcessLookupError):
        pass
'''
        try: script('knode1', stop_owned, [out, ycsb_pid])
        except Exception as error: cleanup_errors.append('client workload: ' + str(error))
    for source_relay in source_relays:
        source_relay.terminate()
        try: source_relay.wait(timeout=2)
        except subprocess.TimeoutExpired:
            source_relay.kill()
            source_relay.wait(timeout=2)
    for target_relay_pid in target_relay_pids:
        try: run('knode3', ['kill', '-TERM', target_relay_pid], check=False)
        except Exception as error: cleanup_errors.append('target relay: ' + str(error))
    if nat_rule:
        try: run('knode1', ['sudo', '-n', 'iptables', '-w', '10', '-t', 'nat', '-D', 'OUTPUT', *nat_rule])
        except Exception as error: cleanup_errors.append('client NAT: ' + str(error))
    for host in reversed(created):
        try: run(host, ['docker', 'rm', '-f', name])
        except Exception as error: cleanup_errors.append(host + ' container: ' + str(error))
    for host in reversed(changed_selection):
        expected = str(STOCK) if host == 'knode2' else str(out / 'criu')
        program = switch if host == 'knode2' else out / 'select_criu.py'
        try:
            run(host, ['sudo', '-n', 'python3', program, expected, selection[host]])
            state.setdefault('selection_rollback', {})[host] = True
        except Exception as error:
            state.setdefault('selection_rollback', {})[host] = str(error)
            cleanup_errors.append(host + ' CRIU selection: ' + str(error))
    if cleanup_errors:
        state['success'] = False
        state['cleanup_errors'] = cleanup_errors
    save()


save()
try:
    for host in ('knode2', 'knode3'):
        if run(host, ['pgrep', '-x', 'criu'], check=False):
            raise RuntimeError('another CRIU is active on ' + host)
        if run(host, ['docker', 'ps', '-aq', '--filter', 'label=swiftbaton.ae=true']):
            raise RuntimeError('an AE container is still present on ' + host)
        selection[host] = run(host, ['readlink', '/usr/bin/criu'])
        existing = run(host, ['docker', 'network', 'ls', '-q', '--filter', 'name=^sb-ae-net$'])
        if not existing:
            run(host, ['docker', 'network', 'create', '--subnet', '172.30.52.0/24', 'sb-ae-net'])
    ids = [run(h, ['docker', 'image', 'inspect', IMAGE, '--format', '{{.Id}}'])
           for h in ('knode2', 'knode3')]
    if ids[0] != ids[1]:
        raise RuntimeError('Redis image mismatch')
    for host in ('knode1', 'knode3'):
        run(host, ['sudo', '-n', 'mkdir', '-p', out])
        run(host, ['sudo', '-n', 'chown', 'k8s:k8s', out])
    for source, target in [(STOCK, out / 'criu'),
                           (COMMON / 'rsocket_relay', out / 'rsocket_relay'),
                           (COMMON / 'tree_stream.py', out / 'tree_stream.py'),
                           (switch, out / 'select_criu.py')]:
        stage_to_target(source, target)
    run('knode3', ['chmod', '+x', out / 'criu', out / 'rsocket_relay'])
    source_hash = run('knode2', ['sha256sum', STOCK]).split()[0]
    target_hash = run('knode3', ['sha256sum', out / 'criu']).split()[0]
    if source_hash != target_hash:
        raise RuntimeError('stock CRIU binary mismatch')
    state['criu_sha256'] = {'knode2': source_hash, 'knode3': target_hash}
    save()
    for host in ('knode2', 'knode3'):
        command = ['docker', 'create', '--name', name,
                   '--label', 'swiftbaton.baseline.native=true', '--network', 'sb-ae-net',
                   '--ip', '172.30.52.3', '--security-opt', 'seccomp=unconfined',
                   '-p', str(args.port) + ':6379', IMAGE,
                   'redis-server', '--save', '', '--appendonly', 'no']
        state[host + '_cid'] = run(host, command)
        created.append(host)
    run('knode2', ['docker', 'start', name])
    for _ in range(50):
        if run('knode2', ['redis-cli', '-p', args.port, 'SET', 'ae:sentinel', name],
               check=False) == 'OK':
            break
        time.sleep(.2)
    else:
        raise RuntimeError('source Redis did not become ready')
    if run('knode1', ['redis-cli', '-h', '10.0.0.62', '-p', args.port, 'GET', 'ae:sentinel']) != name:
        raise RuntimeError('Node1 cannot reach source Redis')

    properties = '\n'.join([
        'workload=site.ycsb.workloads.CoreWorkload', 'recordcount=' + str(args.records),
        'operationcount=1000000000', 'fieldcount=1',
        'fieldlength=' + str(args.field_length), 'fieldlengthdistribution=constant',
        'zeropadding=32', 'readproportion=0.5', 'updateproportion=0.5',
        'scanproportion=0', 'insertproportion=0',
        'requestdistribution=zipfian', 'zipfzeta=0.99', 'readallfields=true',
        'redis.host=10.0.0.62', 'redis.port=' + str(args.port), 'redis.timeout=100',
        'threadcount=' + str(args.threads), 'status.interval=10',
        'measurementtype=hdrhistogram', 'maxexecutiontime=' + str(args.duration), ''])
    (out / 'workload.properties').write_text(properties)
    script('knode1', 'from pathlib import Path;import sys; p=Path(sys.argv[1]);'
           'p.parent.mkdir(parents=True,exist_ok=True);p.write_text(sys.argv[2])',
           [out / 'workload.properties', properties])
    ycsb = BASE / 'build/YCSB'
    cp = ':'.join(str(ycsb / p) for p in ('core/target/classes', 'redis/target/classes',
                     'redis/target/dependency/*', 'core/target/dependency/*'))
    base = ['java', '-Xms1g', '-Xmx4g', '-cp', cp, 'site.ycsb.Client',
            '-db', 'site.ycsb.db.RedisClient', '-s', '-P', out / 'workload.properties']
    load = run('knode1', base + ['-load', '-p', 'status.interval=1000',
                                '-p', 'maxexecutiontime=600'], timeout=650)
    (out / 'load.log').write_text(load + '\n')
    event('load_completed')
    if args.canary_mib:
        canary = (BASE / 'scripts/ae/u/canary_redis.py').read_bytes()
        command = ['python3', '-', 'load', '--host', '10.0.0.62', '--port', str(args.port),
                   '--mib', str(args.canary_mib), '--seed', name]
        (out / 'canary-load.json').write_text(run('knode1', command, input_bytes=canary, timeout=180) + '\n')
    expected = args.records + 2 + bool(args.canary_mib)
    if int(run('knode2', ['redis-cli', '-p', args.port, 'DBSIZE'])) != expected:
        raise RuntimeError('source key count mismatch')
    ycsb_pid = launch('knode1', 'run', base + ['-t', '-p',
                      'swiftbaton.success.gaps.dir=' + str(out / 'success-gaps')])
    event('workload_started')
    time.sleep(args.warmup)

    for host in ('knode2', 'knode3'):
        selected = STOCK if host == 'knode2' else out / 'criu'
        program = switch if host == 'knode2' else out / 'select_criu.py'
        run(host, ['sudo', '-n', 'python3', program, selection[host], selected])
        changed_selection.append(host)
    event('checkpoint_start')
    run('knode2', ['docker', 'checkpoint', 'create', name, 'checkpoint'], timeout=120)
    event('source_checkpoint_finished')
    event('source_retired')  # conservative: command returned after source stopped
    src = Path('/var/lib/docker/containers') / state['knode2_cid'] / 'checkpoints/checkpoint'
    dst = Path('/var/lib/docker/containers') / state['knode3_cid'] / 'checkpoints/checkpoint'
    run('knode3', ['sudo', '-n', 'mkdir', '-p', dst.parent])
    lanes = 8
    target_ports = [rdma_port + 3 + 3 * i for i in range(lanes)]
    source_ports = [port + 2 for port in target_ports]
    relays = [('control', rdma_port, rdma_port + 1, rdma_port + 2)] + [
        ('data-%d' % i, target_ports[i], target_ports[i] + 1, source_ports[i])
        for i in range(lanes)]
    for label, target_port, network_port, source_port in relays:
        target_relay_pids.append(run('knode3', ['sh', '-c',
            'nohup %s listen 10.0.0.63 %d 127.0.0.1 %d >%s 2>&1 </dev/null & echo $!' %
            (shlex.quote(str(out / 'rsocket_relay')), network_port, target_port,
             shlex.quote(str(out / ('relay-target-' + label + '.log'))))]))
    run('knode3', ['sh', '-c',
        'nohup sudo -n python3 %s receive %s --port %d --data-ports %s >%s 2>&1 </dev/null &' %
        (shlex.quote(str(out / 'tree_stream.py')), shlex.quote(str(dst)), rdma_port,
         shlex.quote(','.join(map(str, target_ports))),
         shlex.quote(str(out / 'transfer-target.log')))])
    for label, target_port, network_port, source_port in relays:
        with (out / ('relay-source-' + label + '.log')).open('w') as log:
            source_relays.append(subprocess.Popen([str(COMMON / 'rsocket_relay'), 'connect',
                '127.0.0.1', str(source_port), '10.0.0.63', str(network_port)],
                stdout=log, stderr=subprocess.STDOUT))
    time.sleep(.3)
    transfer = run('knode2', ['sudo', '-n', 'python3', COMMON / 'tree_stream.py',
                              'send', src, '--port', rdma_port + 2,
                              '--data-ports', ','.join(map(str, source_ports))], timeout=600)
    (out / 'transfer-source.log').write_text(transfer + '\n')
    event('rdma_images_received', transfer=transfer)
    if script('knode1', 'from pathlib import Path;import sys;print(Path(sys.argv[1]).exists())',
              [out / 'run.status']) == 'True':
        raise RuntimeError('YCSB ended before restore; increase --duration')
    run('knode3', ['docker', 'start', '--checkpoint', 'checkpoint', name], timeout=120)
    nat_rule = ['-d', '10.0.0.62/32', '-p', 'tcp', '--dport', str(args.port),
                '-m', 'comment', '--comment', name, '-j', 'DNAT',
                '--to-destination', '10.0.0.63:' + str(args.port)]
    run('knode1', ['sudo', '-n', 'iptables', '-w', '10', '-t', 'nat', '-I', 'OUTPUT', '1', *nat_rule])
    run('knode1', ['sudo', '-n', 'conntrack', '-D', '-p', 'tcp', '--dst', '10.0.0.62',
                  '--dport', args.port], check=False)
    event('network_cutover')
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if run('knode1', ['timeout', '3', 'redis-cli', '-h', '10.0.0.62', '-p', args.port,
                         'GET', 'ae:sentinel'], check=False) == name:
            event('sentinel_verified')
            break
        time.sleep(.2)
    else:
        raise TimeoutError('target Redis not reachable through the client endpoint')
    await_job('knode1', 'run', args.duration + 60)
    subprocess.run(['rsync', '-az', 'knode1:' + str(out / 'success-gaps') + '/',
                    str(out / 'success-gaps') + '/'], check=True, timeout=30)
    if args.poststeady_seconds:
        steady = run('knode1', base + ['-t', '-p', 'maxexecutiontime=' +
                    str(args.poststeady_seconds)], timeout=args.poststeady_seconds + 60)
        (out / 'poststeady.log').write_text(steady + '\n')
    if int(run('knode3', ['redis-cli', '-p', args.port, 'DBSIZE'])) != expected:
        raise RuntimeError('destination key count mismatch')
    event('key_count_verified', count=expected)
    if args.canary_mib:
        command = ['python3', '-', 'verify', '--host', '10.0.0.62', '--port', str(args.port),
                   '--mib', str(args.canary_mib), '--seed', name]
        (out / 'canary-verify.json').write_text(run('knode1', command, input_bytes=canary,
                                                  timeout=180) + '\n')
    verifier = (BASE / 'scripts/ae/u/verify_redis.py').read_bytes()
    command = ['python3', '-', '--host', '10.0.0.62', '--port', str(args.port),
               '--records', str(args.records), '--field-length', str(args.field_length),
               '--sentinel', name, '--extra-keys', str(int(bool(args.canary_mib)))]
    validation = run('knode1', command, input_bytes=verifier, timeout=180)
    (out / 'validation.json').write_text(validation + '\n')
    event('all_records_verified', checked=args.records)
    state['success'] = True
    save()
    for analysis in ('analyze_run.py', 'analyze_recovery.py', 'analyze_success_gaps.py'):
        with (out / (analysis + '.log')).open('w') as log:
            subprocess.run([sys.executable, str(BASE / 'scripts' / analysis), str(out)],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
except Exception as error:
    state['success'] = False
    state['error'] = str(error)
    save()
    event('failed', error=str(error))
    raise
finally:
    cleanup()
    print('STATE=' + str(out / 'state.json'), flush=True)
if state['success']:
    print('NATIVE_CRIU_RDMA_PASS state=' + str(out / 'state.json'), flush=True)
else:
    raise SystemExit(1)
