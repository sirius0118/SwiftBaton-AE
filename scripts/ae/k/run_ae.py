#!/usr/bin/env python3
"""Run on knode2. Reproduce and validate one SwiftBaton Redis migration."""
import argparse
import os
import json
import re
import secrets
import shlex
import subprocess
import time
import sys
from k_mode import KernelSettings, validate_preflight, validate_container, validate_completion, validate_ps_config, validate_export_config, validate_dispatch
from pathlib import Path

BASE = Path(__file__).resolve().parents[3]
RUN_ROOT = Path(os.environ.get('SB_AE_WORK_ROOT', str(BASE.parent / (BASE.name + '-work')))).resolve()
if BASE == RUN_ROOT or BASE in RUN_ROOT.parents:
    raise SystemExit('SB_AE_WORK_ROOT must be outside the source repository')
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--kernel-transfer', action='store_true', help='Use the already deployed K CRIU and anonymous-PTE module; never installs or loads them')
parser.add_argument('--kernel-device', default='mlx5_1')
parser.add_argument('--kernel-gid', type=int, default=3)
parser.add_argument('--kernel-timeout-ms', type=int, default=2000)
parser.add_argument('--kernel-dense', action='store_true')
parser.add_argument('--kernel-export-workers', type=int, default=1)
parser.add_argument('--kernel-export-chunk-mb', type=int, default=0)
parser.add_argument('--kernel-ps-chunk-mb', type=int, default=64, help='K: independently published PS span in MiB, default 64; 0 retains legacy spans')
parser.add_argument('--preflight-only', action='store_true', help='K: inspect both hosts without creating containers or changing network/kernel state')
parser.add_argument('--records', type=int, default=1000000)
parser.add_argument('--field-length', type=int, default=1024, help='Bytes per YCSB field0 value; record count is configured separately')
parser.add_argument('--duration', type=int, default=90)
parser.add_argument('--warmup', type=int, default=15)
parser.add_argument('--threads', type=int, default=32)
parser.add_argument('--defer-fault-credits', action='store_true', help='Accept source PF requests independently of SQ credits and coalesce request ring acknowledgements')
parser.add_argument('--serial-ps-prepare', action='store_true', help='Ablation: single-thread full memset of PS receive buffer before MR registration')
parser.add_argument('--numa-node', type=int, choices=[0, 1], help='Bind both containers CPU/memory and destination pageclient to this NUMA node; U also binds anonymous PS staging. K worker page placement is verified by runtime observations, not a kernel allocation-policy guarantee.')
parser.add_argument('--runtime-snapshot', action='store_true', help='Read owned container CPU/NUMA/page layout before workload and after it; no timed-window sampling')
parser.add_argument('--poststeady-seconds', type=int, default=0, help='Optional fresh-client target throughput run after the migration workload, 10..120 seconds')
parser.add_argument('--port', type=int, default=6390)
parser.add_argument('--image-rdma', action='store_true', help='Keep CRIU images in tmpfs and transfer over RDMA')
parser.add_argument('--fast-cutover', action='store_true', help='Arm a direct control connection before checkpoint')
parser.add_argument('--u-precopy', action='store_true', help='Use real PS snapshots with final soft-dirty/PFN validation')
parser.add_argument('--parent-stage', action='store_true', help='Prepare anonymous pages in restore parent during PS, inherit and remap')
parser.add_argument('--parallel-transfer', action='store_true', help='Use independent demand, adjacent prefetch and background RDMA lanes')
parser.add_argument('--install-workers', type=int, default=4)
parser.add_argument('--copy-workers', type=int, default=4)
parser.add_argument('--fault-read-batch', type=int, default=1, help='Max translated missing pages extracted per lifecycle lock acquisition, 1..64')
parser.add_argument('--fault-install-workers', type=int, default=0, help='Experimental target per-PID demand installers, 1..32; default zero keeps inline installation')
parser.add_argument('--fault-workers', type=int, default=2)
parser.add_argument('--prefetch-workers', type=int, default=1)
parser.add_argument('--no-prefetch', action='store_true', help='Ablation: disable adjacent-page enqueue, retaining channel resources')
parser.add_argument('--no-hot-first', action='store_true', help='Ablation: use address-order background instead of sampled heat order')
parser.add_argument('--no-pretransfer', action='store_true', help='Ablation: send an empty PS control snapshot, no application page payload')
parser.add_argument('--serial-precopy-ack', action='store_true', help='Ablation: scan PS installation acknowledgements on the source demand thread')
parser.add_argument('--sync-fault-transport', action='store_true', help='Ablation: synchronous PF transport with the old shared client lock')
parser.add_argument('--rdma-mtu', type=int, choices=[256,512,1024,2048,4096], help='Optional endpoint MTU ceiling; default negotiates active MTUs')
parser.add_argument('--fault-trace', action='store_true', help='Buffer per-request transport timestamps and emit after AS; no disk I/O on the fault path')
parser.add_argument('--spin-lifecycle', action='store_true', help='Experimental busy-wait lifecycle gate with pending-writer priority')
parser.add_argument('--reader-preferred-lock', action='store_true', help='Ablation: restore original installer-preferring lifecycle rwlock')
parser.add_argument('--serial-prefetch-install', action='store_true', help='Ablation: one target FT installer shared by every process')
parser.add_argument('--serial-background-install', action='store_true', help='Ablation: finish each complete target batch before dispatching the next')
parser.add_argument('--no-bg-fault-assist', action='store_true', help='Ablation: pipeline target BG batches without demand workers claiming arrived pages')
parser.add_argument('--install-trace', action='store_true', help='Diagnostic target installation lock/COPY/retry wall-time attribution; buffered until AS ends')
parser.add_argument('--bg-round-robin', action='store_true', help='Ablation: rotate normal installers across arrived batches instead of preserving source heat order')
parser.add_argument('--fixed-ready-scan', action='store_true', help='Ablation: restart ready-slot scans at zero')
parser.add_argument('--prefetch-window', type=int, default=16, help='Maximum claimed but not install-ACKed prefetch pages; 0 retains the unbounded ablation')
parser.add_argument('--page-trace', action='store_true', help='Diagnostic target page installation timestamps; writes CSV after AS workers finish')
parser.add_argument('--kernel-trace', action='store_true', help='Diagnostic only: trace kernel page fault, scheduling and long syscall events for the actual-read probe')
parser.add_argument('--page-probe', action='store_true', help='Diagnostic memory child: real adjacent-page reads with mincore and timestamps after resume')
parser.add_argument('--batch-pages', type=int, default=None)
parser.add_argument('--compact-bg-wire', action='store_true', help='Send only used background metadata fields and page descriptors')
parser.add_argument('--bg-segment-pages', type=int, default=0, help='Experimental background RDMA write limit in pages, 8..256; zero sends each whole batch')
parser.add_argument('--precopy-workers', type=int, default=4)
parser.add_argument('--validation-workers', type=int, default=1)
parser.add_argument('--vma-cache', action='store_true', help='Cache PS smaps with eBPF mutation monitoring and final maps check')
parser.add_argument('--precopy-limit-mb', type=int, default=2048)
parser.add_argument('--dynamic-memory', action='store_true', help='Exercise discard, unmap/reuse, remap and fork during target AS')
parser.add_argument('--memory-children', type=int, default=0)
parser.add_argument('--cow-descendants', action='store_true', help='Each memory child forks one same-executable COW descendant before migration')
parser.add_argument('--fd-adversarial', action='store_true', help='Also test EFD_SEMAPHORE and preexisting UDP queues; known unsupported on current baseline')
parser.add_argument('--fd-placeholder', action='store_true', help='Prepare empty FD/socket objects during PS with final-context validation')
parser.add_argument('--fd-groups', type=int, default=0, help='Per memory process: eventfds, epoll, pipes, UNIX/TCP/UDP queues, file aliases, timers and churn')
parser.add_argument('--child-mib', type=int, default=64)
parser.add_argument('--child-workers', type=int, default=4)
parser.add_argument('--canary-mib', type=int, default=0, help='Immutable data for bytewise migration integrity checks')
opts = parser.parse_args()
SCRIPT_ROOT = Path(__file__).resolve().parent
CRIU_ROOT = BASE / ('build/criu-K' if opts.kernel_transfer else 'build/criu-U')
if opts.batch_pages is None:
    opts.batch_pages = 32 if opts.kernel_transfer else 64
kernel_settings = None
if opts.kernel_transfer:
    if not (opts.image_rdma and opts.u_precopy):
        parser.error('K requires --image-rdma and --u-precopy')
    if opts.batch_pages != 32:
        parser.error('Current K uses 32-page RDMA batches; --batch-pages must be 32')
    unsupported = {'parent-stage','parallel-transfer','vma-cache','defer-fault-credits',
        'serial-ps-prepare','dynamic-memory','page-trace','page-probe','kernel-trace',
        'fault-read-batch','fault-install-workers','copy-workers','serial-precopy-ack',
        'sync-fault-transport','rdma-mtu','fault-trace','spin-lifecycle','reader-preferred-lock',
        'serial-prefetch-install','serial-background-install','no-bg-fault-assist','install-trace',
        'bg-round-robin','fixed-ready-scan','prefetch-window','compact-bg-wire','bg-segment-pages'}
    specified = {arg[2:].split('=',1)[0] for arg in sys.argv[1:] if arg.startswith('--')}
    if specified & unsupported:
        parser.error('U-specific options are not K paths: ' + ', '.join(sorted(specified & unsupported)))
    kernel_settings = KernelSettings(device=opts.kernel_device, gid=opts.kernel_gid,
        timeout_ms=opts.kernel_timeout_ms, fault_workers=opts.fault_workers,
        prefetch_workers=opts.prefetch_workers, install_workers=opts.install_workers,
        precopy_workers=opts.precopy_workers, no_pretransfer=opts.no_pretransfer,
        no_prefetch=opts.no_prefetch, no_hot_first=opts.no_hot_first, dense=opts.kernel_dense,
        ps_chunk_mb=opts.kernel_ps_chunk_mb, export_workers=opts.kernel_export_workers,
        export_chunk_mb=opts.kernel_export_chunk_mb)
    try:
        kernel_settings.values()
    except ValueError as error:
        parser.error(str(error))
elif opts.preflight_only or opts.kernel_dense or any(a.split('=', 1)[0] in ('--kernel-ps-chunk-mb','--kernel-export-workers','--kernel-export-chunk-mb') for a in sys.argv[1:]):
    parser.error('--preflight-only/--kernel-dense require --kernel-transfer')
if opts.defer_fault_credits and (not opts.parallel_transfer or opts.sync_fault_transport):
    parser.error('--defer-fault-credits requires asynchronous --parallel-transfer')
if opts.serial_ps_prepare and not opts.parent_stage:
    parser.error('--serial-ps-prepare requires --parent-stage')
if opts.numa_node is not None and not (opts.runtime_snapshot and (opts.parent_stage or opts.kernel_transfer)):
    parser.error('--numa-node requires --runtime-snapshot and either K or U --parent-stage')
if opts.poststeady_seconds and not 10 <= opts.poststeady_seconds <= 120:
    parser.error('poststeady-seconds must be zero or 10..120')
if opts.records < 1 or not 1 <= opts.field_length <= 1048576:
    parser.error('records must be positive and field-length must be 1..1048576 bytes')
if not 0 <= opts.memory_children <= 8 or not 1 <= opts.child_mib <= 1024 or not 1 <= opts.child_workers <= 16:
    parser.error('memory-children 0..8, child-mib 1..1024, child-workers 1..16 required')
if opts.dynamic_memory and not opts.memory_children:
    parser.error('--dynamic-memory requires --memory-children')
if opts.cow_descendants and not opts.memory_children:
    parser.error('--cow-descendants requires --memory-children')
if opts.vma_cache and not opts.parent_stage:
    parser.error('--vma-cache requires --parent-stage')
if opts.fd_placeholder and not opts.u_precopy:
    parser.error('--fd-placeholder requires --u-precopy')
if opts.fd_adversarial and not opts.fd_groups:
    parser.error('--fd-adversarial requires --fd-groups')
if not 0 <= opts.fd_groups <= 512 or (opts.fd_groups and not opts.memory_children):
    parser.error('--fd-groups 0..512 requires --memory-children')
memory_processes = opts.memory_children * (2 if opts.cow_descendants else 1)
if opts.page_trace and not opts.parallel_transfer:
    parser.error('--page-trace requires --parallel-transfer')
if opts.page_probe and (not opts.page_trace or opts.memory_children != 1 or opts.child_workers != 1 or opts.cow_descendants or opts.dynamic_memory or opts.fd_groups):
    parser.error('--page-probe requires --page-trace, one memory child/worker, no COW/dynamic/FD fixture')
if opts.spin_lifecycle and (not opts.parallel_transfer or opts.reader_preferred_lock):
    parser.error('--spin-lifecycle requires parallel transfer and cannot combine with reader-preferred-lock')
if opts.kernel_trace and (not opts.page_probe or not opts.fault_trace):
    parser.error('--kernel-trace requires --page-probe and --fault-trace')
if opts.memory_children and not (opts.parallel_transfer or opts.kernel_transfer):
    parser.error('multi-process validation requires --parallel-transfer')
if not opts.kernel_transfer and (opts.no_prefetch or opts.no_hot_first or opts.no_pretransfer or opts.serial_precopy_ack or opts.sync_fault_transport or opts.fault_trace or opts.reader_preferred_lock or opts.fixed_ready_scan or opts.serial_prefetch_install or opts.serial_background_install or opts.no_bg_fault_assist or opts.bg_round_robin or opts.install_trace or opts.compact_bg_wire) and not opts.parallel_transfer:
    parser.error('Path ablations require --parallel-transfer')
if opts.parallel_transfer and not opts.u_precopy:
    parser.error('--parallel-transfer requires --u-precopy')
if opts.bg_segment_pages and (not opts.parallel_transfer or not 8 <= opts.bg_segment_pages <= 256):
    parser.error('bg-segment-pages requires parallel-transfer and 0 or 8..256')
if not 1 <= opts.fault_read_batch <= 64:
    parser.error('fault-read-batch must be 1..64')
if not 0 <= opts.fault_install_workers <= 32:
    parser.error('fault-install-workers must be 0..32')
if not 1 <= opts.fault_workers <= 32 or not 1 <= opts.prefetch_workers <= 32:
    parser.error('fault-workers and prefetch-workers must be 1..32')
if not 1 <= opts.copy_workers <= 32 or not 1 <= opts.install_workers <= 32 or not 1 <= opts.batch_pages <= 256:
    parser.error('copy-workers and install-workers must be 1..32 and batch-pages must be 1..256')
if opts.parent_stage and not opts.u_precopy:
    parser.error('--parent-stage requires --u-precopy')
if (opts.u_precopy or opts.fast_cutover) and not opts.image_rdma:
    parser.error('--u-precopy and --fast-cutover require --image-rdma')
if not 1 <= opts.precopy_workers <= 32 or not 1 <= opts.precopy_limit_mb <= 65536:
    parser.error('precopy-workers must be 1..32 and precopy-limit-mb must be 1..65536')
if not 1 <= opts.validation_workers <= 32:
    parser.error('validation-workers must be 1..32')
if not 0 <= opts.canary_mib <= 1024:
    parser.error('canary-mib must be 0..1024')
if not 0 <= opts.prefetch_window <= 4096:
    parser.error('prefetch-window must be 0..4096')
precopy_config = (f'u-precopy=yes\nprecopy-workers={opts.precopy_workers}\n'
                  f'precopy-limit-mb={opts.precopy_limit_mb}\nvalidation-workers={opts.validation_workers}\n') if opts.u_precopy else ''
if opts.numa_node is not None and not opts.kernel_transfer:
    precopy_config += f'stage-numa-node={opts.numa_node}\n'
if opts.vma_cache:
    precopy_config += 'vma-cache=yes\n'
if opts.fd_placeholder:
    precopy_config += 'fd-placeholder=yes\n'
if opts.parent_stage:
    precopy_config += 'parent-stage=yes\n'
if opts.parallel_transfer:
    precopy_config += f'prefetch-window={opts.prefetch_window}\n'
    precopy_config += f'parallel-transfer=yes\ninstall-workers={opts.install_workers}\nbatch-pages={opts.batch_pages}\nbg-segment-pages={opts.bg_segment_pages}\ncopy-workers={opts.copy_workers}\nfault-read-batch={opts.fault_read_batch}\nfault-install-workers={opts.fault_install_workers}\nfault-workers={opts.fault_workers}\nprefetch-workers={opts.prefetch_workers}\n'
if opts.rdma_mtu:
    precopy_config += f'rdma-mtu={opts.rdma_mtu}\n'
for option in ['no_prefetch', 'no_hot_first', 'no_pretransfer', 'serial_precopy_ack', 'sync_fault_transport','fault_trace','reader_preferred_lock','spin_lifecycle','fixed_ready_scan','serial_prefetch_install','serial_background_install','no_bg_fault_assist','bg_round_robin','install_trace','compact_bg_wire','serial_ps_prepare','defer_fault_credits']:
    if getattr(opts, option):
        precopy_config += option.replace('_', '-') + '=yes\n'
if kernel_settings:
    # Use exactly the same K options for checkpoint, restore and page-client.
    precopy_config = kernel_settings.config() + f'precopy-limit-mb={opts.precopy_limit_mb}\n'
    if opts.fd_placeholder:
        precopy_config += 'fd-placeholder=yes\n'
NAME = 'sb_ae_' + time.strftime('%Y%m%d_%H%M%S')
TRACE_TAG = 'sbpf' + NAME[-6:]
OBSERVER = BASE / 'ae-work/kernel-observation'
OUT = RUN_ROOT / NAME
OUT.mkdir(parents=True)
STATE = {'name': NAME, 'out': str(OUT), 'port': opts.port, 'parameters': vars(opts),
         'expected_value_bytes': opts.records * opts.field_length,
         'transfer_mode': 'K' if opts.kernel_transfer else 'U'}
for script_name in ['run_ae.py', 'verify_redis.py', 'k_mode.py']:
    (OUT / script_name).write_bytes(Path(__file__).with_name(script_name).read_bytes())
if opts.runtime_snapshot:
    (OUT / 'runtime_snapshot.py').write_bytes(Path(__file__).with_name('runtime_snapshot.py').read_bytes())
IMAGE = os.environ.get('SB_REDIS_IMAGE', 'm.daocloud.io/docker.io/library/redis:latest')

def event(kind, event_time_ns=None, **kw):
    row = dict(time_ns=event_time_ns if event_time_ns is not None else time.time_ns(), event=kind, **kw)
    with (OUT / 'events.jsonl').open('a') as f:
        f.write(json.dumps(row) + '\n')
    print(json.dumps(row), flush=True)

def cmd(host, args, timeout=30, check=True):
    command = args if host == 'knode2' else ['ssh', '-oBatchMode=yes', '-oConnectTimeout=8', host, shlex.join(args)]
    p = subprocess.run(command, text=True, errors="replace", capture_output=True, timeout=timeout)
    if check and p.returncode:
        raise RuntimeError(f'{host}: {shlex.join(args)}: {p.stdout}\n{p.stderr}')
    return p.stdout.strip()

def py(host, script):
    return cmd(host, ['sudo', '-n', 'python3', '-c', script])

def spawn(host, label, args):
    if opts.numa_node is not None and label == 'pageclient':
        args = ['numactl', '--cpunodebind=' + str(opts.numa_node), '--membind=' + str(opts.numa_node)] + args
    if opts.kernel_trace and label == 'pageclient':
        args = [str(OBSERVER / 'observe-run'), str(OBSERVER / 'observe.bpf.o'), str(OUT), TRACE_TAG, str(opts.child_mib)] + args
        STATE['kernel_observer_command'] = args
    # A wrapper records the actual command's exit status. A PID alone can be
    # reused; the status file is also needed to abort promptly on early failure.
    wrapper = ("import subprocess,pathlib,json,time; "
               f"p=subprocess.Popen({args!r}); rc=p.wait(); completed=time.time_ns(); "
               f"end=pathlib.Path({str(OUT / (label + '.completion.json'))!r}); "
               "temporary=end.with_suffix('.tmp'); "
               "temporary.write_text(json.dumps(dict(exit_code=rc,time_ns=completed))); temporary.replace(end); "
               f"target=pathlib.Path({str(OUT / (label + '.status'))!r}); "
               "temporary=target.with_suffix('.status.tmp'); temporary.write_text(str(rc)); temporary.replace(target)")
    script = ('import subprocess,pathlib; '
              f'p=pathlib.Path({str(OUT)!r});p.mkdir(parents=True,exist_ok=True); '
              f'f=open(p/{label + ".log"!r},"w"); '
              f'child=subprocess.Popen(["python3","-c",{wrapper!r}],stdout=f,stderr=subprocess.STDOUT,start_new_session=True); '
              'print(child.pid)')
    pid = int(py(host, script))
    STATE[label + '_pid'] = pid
    save()
    event('spawn', host=host, label=label, pid=pid)

def check_migration_jobs():
    for host, label in [('knode2', 'checkpoint'), ('knode3', 'restore'), ('knode3', 'pageclient')]:
        if label + '_pid' not in STATE:
            continue
        status = py(host, f'from pathlib import Path;p=Path({str(OUT)!r})/{label + ".status"!r};print(p.read_text() if p.exists() else "running")')
        if status != 'running' and int(status) != 0:
            raise RuntimeError(f'{host} {label} exited with {status}; see {label}.log')

def save():
    (OUT / 'state.json').write_text(json.dumps(STATE, indent=2))

def runtime_snapshot(host, phase):
    if not opts.runtime_snapshot:
        return
    script = Path(__file__).with_name('runtime_snapshot.py').read_text()
    raw = py(host, script.replace("if __name__ == '__main__':", "if False:") +
             '\nprint(json.dumps(collect(' + repr(NAME) + '), indent=2))\n')
    data = json.loads(raw)
    if data['container_id'] != STATE[host + '_cid']:
        raise RuntimeError('Runtime snapshot container identity mismatch')
    if opts.numa_node is not None:
        cpus = set()
        for part in data['host_config']['CpusetCpus'].split(','):
            first, _, last = part.partition('-')
            cpus.update(range(int(first), int(last or first) + 1))
        if data['host_config']['CpusetMems'] != str(opts.numa_node) or not cpus or any(
                not set(t['affinity']) <= cpus for t in data['tasks'].values()):
            raise RuntimeError('Runtime NUMA/CPU binding mismatch')
    (OUT / ('runtime-' + phase + '.json')).write_text(json.dumps(data, indent=2) + '\n')
    event('runtime_snapshot', host=host, phase=phase, pid=data['pid'],
          collection_ms=(data['finished_ns'] - data['time_ns']) / 1e6)

def canary(mode):
    script = Path(__file__).with_name('canary_redis.py').read_text()
    argv = ['python3', '-', mode, '--host', '10.0.0.62', '--port', str(opts.port),
            '--mib', str(opts.canary_mib), '--seed', NAME]
    result = subprocess.run(['ssh', '-oBatchMode=yes', 'knode1', shlex.join(argv)],
                            input=script, text=True, capture_output=True, timeout=180)
    (OUT / ('canary-' + mode + '.json')).write_text(result.stdout)
    (OUT / ('canary-' + mode + '.stderr')).write_text(result.stderr)
    if result.returncode:
        raise RuntimeError('Canary ' + mode + ' failed: ' + result.stderr[-2000:])
    event('canary_' + mode, result=json.loads(result.stdout))

def check_memory_children(host, phase):
    if not opts.memory_children:
        return
    raw = cmd(host, ['docker', 'exec', NAME, '/ae-memory-fixture', 'check',
                     str(memory_processes)], timeout=120)
    rows = [json.loads(line) for line in raw.splitlines()]
    (OUT / ('memory-children-' + phase + '.json')).write_text(json.dumps(rows, indent=2))
    if len(rows) != memory_processes or any(not r['ok'] or r['bytes'] != opts.child_mib * 1024 * 1024 for r in rows):
        raise RuntimeError('Child memory validation failed')
    if opts.fd_groups and any(r.get('fd_semaphore') != opts.fd_adversarial or r.get('fd_queued_udp') != opts.fd_adversarial or r.get('fd_groups') != opts.fd_groups or r.get('fd_bad') != 0 or not r.get('fd_churn_epoch') for r in rows):
        raise RuntimeError('FD/socket state verification failed')
    if opts.cow_descendants:
        for i in range(opts.memory_children):
            if rows[i + opts.memory_children]['parent_pid'] != rows[i]['pid']:
                raise RuntimeError('COW descendant parent identity changed')
    if phase == 'target':
        before = json.loads((OUT / 'memory-children-source.json').read_text())
        for a, b in zip(before, rows):
            if opts.fd_groups and b['fd_churn_epoch'] <= a['fd_churn_epoch']:
                raise RuntimeError('FD churn stopped after migration')
            if a['child'] != b['child'] or a['pid'] != b['pid'] or a['address'] != b['address'] or b['operations'] <= a['operations']:
                raise RuntimeError('Child identity, mapping or operation progress changed')
    if opts.dynamic_memory and phase == 'target' and any(not r.get('dynamic_done') or not r.get('dynamic_fork_ok') or r.get('dynamic_bad_words') != 0 for r in rows):
        raise RuntimeError('Dynamic memory lifecycle validation failed')
    if opts.page_probe and phase == 'target':
        if any(not r.get('probe_done') or r.get('probe_bad') != 0 or r.get('probe_reads') != opts.child_mib * 256 // 5 * 5 for r in rows):
            raise RuntimeError('Page probe did not finish or its memory content changed')
        for row in rows:
            name = 'probe-' + str(row['pid']) + '.csv'
            data = subprocess.run(['ssh','-oBatchMode=yes',host,shlex.join(['sudo','-n','cat',str(OUT / 'control' / name)])],capture_output=True,timeout=60,check=True).stdout
            (OUT / name).write_bytes(data)
    event('memory_children_' + phase, processes=rows)

def wait_for(host, path, timeout=45):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.endswith('/stop') or path.endswith('/pages.complete'):
            check_migration_jobs()
        if py(host, f'import os;print(int(os.path.exists({path!r})))') == '1':
            return
        time.sleep(.15)
    raise TimeoutError(f'{host}: {path}')

YCSB = str(BASE / 'build/YCSB')
CP = ':'.join(YCSB + '/' + p for p in ['core/target/classes', 'redis/target/classes',
                                         'redis/target/dependency/*', 'core/target/dependency/*'])

def job(label, args):
    wrapper = ("import subprocess,pathlib,json,time; "
               f"p=subprocess.Popen({args!r}); rc=p.wait(); "
               f"target=pathlib.Path({str(OUT / (label + '.exit'))!r}); "
               "temporary=target.with_suffix('.exit.tmp'); temporary.write_text(str(rc)); temporary.replace(target)")
    spawn('knode1', label, ['python3', '-c', wrapper])

def await_job(label, timeout):
    wait_for('knode1', str(OUT / (label + '.exit')), timeout)
    result = cmd('knode1', ['sudo', '-n', 'cat', str(OUT / (label + '.exit'))])
    log = cmd('knode1', ['sudo', '-n', 'cat', str(OUT / (label + '.log'))], timeout=30)
    (OUT / (label + '.log')).write_text(log + '\n')
    if result != '0':
        raise RuntimeError(label + ' failed, exit=' + result)
    event(label + '_completed')

def capture_logs():
    # Allocator/sanitizer diagnostics go to process stderr, not CRIU's own log.
    for label in ['restore', 'pageclient']:
        if label + '_pid' not in STATE:
            continue
        try:
            args = ['ssh', '-oBatchMode=yes', 'knode3', shlex.join(['sudo', '-n', 'cat', str(OUT / (label + '.log'))])]
            data = subprocess.run(args, capture_output=True, timeout=45, check=True).stdout
            (OUT / (label + '-stdio.log')).write_bytes(data)
        except Exception as e:
            event('collect_log_error', name=label + '-stdio.log', error=str(e))
    for host, names in [('knode2', ['dump.log', 'score.log']),
                        ('knode3', ['restore.log', 'pageclient.log'])]:
        for name in names:
            try:
                args = ['sudo', '-n', 'cat', '/var/lib/criu/' + name]
                if host != 'knode2':
                    args = ['ssh', '-oBatchMode=yes', host, shlex.join(args)]
                data = subprocess.run(args, capture_output=True, timeout=45, check=True).stdout
                (OUT / name).write_bytes(data)
            except Exception as e:
                event('collect_log_error', name=name, error=str(e))
    if opts.page_trace:
        try:
            data = subprocess.run(['ssh','-oBatchMode=yes','knode3',shlex.join(['sudo','-n','cat',str(OUT / 'page-trace.csv')])],capture_output=True,timeout=60,check=True).stdout
            (OUT / 'page-trace.csv').write_bytes(data)
        except Exception as e:
            event('collect_log_error', name='page-trace.csv', error=str(e))
    if opts.kernel_trace:
        for name in ['kernel-events.bin', 'kernel-observer.json']:
            try:
                data = subprocess.run(['ssh','-oBatchMode=yes','knode3',shlex.join(['sudo','-n','cat',str(OUT / name)])],capture_output=True,timeout=60,check=True).stdout
                (OUT / name).write_bytes(data)
            except Exception as e:
                event('collect_log_error', name=name, error=str(e))
    if opts.fast_cutover:
        for name in ['cutover-client.json', 'cutover_listener.log']:
            try:
                (OUT / name).write_text(cmd('knode1', ['sudo', '-n', 'cat', str(OUT / name)]) + '\n')
            except Exception as e:
                event('collect_log_error', name=name, error=str(e))

try:
    save()
    # A deadlocked/custom-module CRIU task can keep the fixed migration ports,
    # namespaces and memory alive even after the driver has exited.
    preflight = "from pathlib import Path; active=[]\nfor p in Path('/proc').iterdir():\n if not p.name.isdigit():continue\n try:\n  if (p/'comm').read_text().strip()=='criu':active.append(p.name)\n except OSError:pass\nprint(' '.join(active))"
    for host in ['knode2', 'knode3']:
        active = cmd(host, ['python3', '-c', preflight])
        if active:
            raise RuntimeError(f'{host} has existing CRIU tasks {active}; finish or recover them before starting another migration')
    if opts.kernel_transfer:
        probe = (SCRIPT_ROOT / 'k_mode.py').read_text()
        hosts = {host: json.loads(py(host, probe + '\nprint(json.dumps(host_probe(' +
                 repr(str(CRIU_ROOT / 'criu/criu')) + ',' + repr(opts.kernel_device) + ',' + str(opts.kernel_gid) + ')))'))
                 for host in ('knode2','knode3')}
        errors = validate_preflight(hosts, opts.kernel_export_workers)
        STATE['kernel_preflight'] = {'hosts': hosts, 'errors': errors, 'passed': not errors}
        (OUT / 'kernel-preflight.json').write_text(json.dumps(STATE['kernel_preflight'], indent=2)+'\n')
        save()
        if opts.preflight_only:
            print(json.dumps(STATE['kernel_preflight'], indent=2), flush=True)
            raise SystemExit(2 if errors else 0)
        if errors:
            raise RuntimeError('K preflight failed: ' + '; '.join(errors))
    hashes = {host: cmd(host, ['sha256sum', str(CRIU_ROOT / 'criu/criu')]).split()[0]
              for host in ['knode2', 'knode3']}
    if hashes['knode2'] != hashes['knode3']:
        raise RuntimeError('Source and destination CRIU binaries differ')
    STATE['criu_sha256'] = hashes
    if opts.kernel_trace:
        STATE['diagnostic_only'] = True
        STATE['kernel_observer_sha256'] = {}
        for name in ['observe-run', 'observe.bpf.o']:
            values = {host: cmd(host, ['sha256sum', str(OBSERVER / name)]).split()[0] for host in ['knode2', 'knode3']}
            if len(set(values.values())) != 1:
                raise RuntimeError('Observer binaries differ')
            STATE['kernel_observer_sha256'][name] = values
        for name in ['observe.h','observe.bpf.c','observe-run.c','build.sh','kernel-uapi.sha256']:
            target = OUT / 'kernel-observer-source' / name
            target.parent.mkdir(exist_ok=True)
            target.write_bytes((OBSERVER / name).read_bytes())
    STATE['rdma_qos'] = {host: cmd(host, ['sudo','-n','mlnx_qos','-i','ens4f1','-a']) for host in ['knode2','knode3']}
    revision = subprocess.run(['git', '-C', str(BASE), 'rev-parse', 'HEAD'], capture_output=True, text=True)
    STATE['source_revision'] = revision.stdout.strip() or 'uncommitted source checkout'
    if opts.memory_children:
        source = Path(__file__).with_name('memory_fixture.c')
        (OUT / 'memory_fixture.c').write_bytes(source.read_bytes())
        (OUT / 'fd_state_fixture.h').write_bytes(source.with_name('fd_state_fixture.h').read_bytes())
        (OUT / 'page_probe_fixture.h').write_bytes(source.with_name('page_probe_fixture.h').read_bytes())
        STATE['memory_fixture_sha256'] = {host: cmd(host, ['sha256sum', str(BASE / 'build/fixture/memory_fixture')]).split()[0] for host in ['knode2', 'knode3']}
        if len(set(STATE['memory_fixture_sha256'].values())) != 1:
            raise RuntimeError('Fixture binaries differ')
    save()
    for host in ['knode2', 'knode3']:
        if not cmd(host, ['docker', 'network', 'ls', '-q', '--filter', 'name=^sb-ae-net$']):
            cmd(host, ['docker', 'network', 'create', '--subnet', '172.30.52.0/24', 'sb-ae-net'])
        args = ['docker', 'create', '--name', NAME, '--label', 'swiftbaton.ae=true',
                '--restart=no', '--network', 'sb-ae-net', '--ip', '172.30.52.3',
                '--security-opt', 'seccomp=unconfined', '-p', str(opts.port) + ':6379']
        if opts.numa_node is not None:
            cpulist = cmd(host, ['cat', f'/sys/devices/system/node/node{opts.numa_node}/cpulist'])
            if not re.fullmatch(r'[0-9,-]+', cpulist):
                raise RuntimeError('Unexpected NUMA CPU list: ' + repr(cpulist))
            args += ['--cpuset-cpus', cpulist, '--cpuset-mems', str(opts.numa_node)]
        command = ['redis-server', '--save', '', '--appendonly', 'no']
        if opts.dynamic_memory or opts.page_probe:
            control = OUT / 'control'
            py(host, f'from pathlib import Path;p=Path({str(control)!r});p.mkdir(parents=True,exist_ok=True)')
            if host == 'knode3':
                py(host, f'from pathlib import Path;Path({str(control / "target")!r}).write_text("target AS lifecycle test\\n")')
            mount = f'type=bind,src={control},dst=/ae-control' + ('' if opts.page_probe else ',readonly')
            args += ['--mount', mount, '--env', 'SB_AE_PAGE_PROBE=1' if opts.page_probe else 'SB_AE_DYNAMIC=1']
        if opts.kernel_trace:
            args += ['--env', 'SB_AE_TRACE_TAG=' + TRACE_TAG]
        if opts.memory_children:
            fixture = str(BASE / 'build/fixture/memory_fixture')
            if opts.fd_groups:
                args += ['--env', 'SB_AE_FD_GROUPS=' + str(opts.fd_groups)]
                if opts.fd_adversarial:
                    args += ['--env', 'SB_AE_FD_ADVERSARIAL=1']
            if opts.cow_descendants:
                args += ['--env', 'SB_AE_COW_DESCENDANTS=' + str(opts.memory_children)]
            args += ['--mount', f'type=bind,src={fixture},dst=/ae-memory-fixture,readonly',
                     '--entrypoint', '/ae-memory-fixture']
            command = ['launch', str(opts.memory_children), str(opts.child_mib),
                       str(opts.child_workers), '0x52ae2026', '--'] + command
        args += [IMAGE] + command
        STATE[host + '_cid'] = cmd(host, args)
        save()
    cmd('knode2', ['docker', 'start', NAME])
    time.sleep(1)
    cmd('knode2', ['redis-cli', '-p', str(opts.port), 'SET', 'ae:sentinel', NAME])
    endpoint = cmd('knode1', ['timeout', '4', 'redis-cli', '-h', '10.0.0.62', '-p', str(opts.port),
                             'GET', 'ae:sentinel'])
    if endpoint != NAME:
        raise RuntimeError('Client does not reach this source container; check retained experiment NAT rules')
    config_text = '\n'.join([
        'workload=site.ycsb.workloads.CoreWorkload', 'recordcount=' + str(opts.records),
        'operationcount=1000000000', 'fieldcount=1', 'fieldlength=' + str(opts.field_length),
        'fieldlengthdistribution=constant', 'zeropadding=32',
        'readproportion=0.5', 'updateproportion=0.5', 'scanproportion=0', 'insertproportion=0',
        'requestdistribution=zipfian', 'zipfzeta=0.99', 'readallfields=true',
        'redis.host=10.0.0.62', 'redis.port=' + str(opts.port), 'redis.timeout=100',
        'threadcount=' + str(opts.threads), 'status.interval=10', 'measurementtype=hdrhistogram',
        'maxexecutiontime=' + str(opts.duration), ''])
    (OUT / 'workload.properties').write_text(config_text)
    py('knode1', f'from pathlib import Path;p=Path({str(OUT)!r});p.mkdir(parents=True);(p/"workload.properties").write_text({config_text!r})')
    base_cmd = ['java', '-Xms1g', '-Xmx4g', '-cp', CP, 'site.ycsb.Client', '-db',
                'site.ycsb.db.RedisClient', '-s', '-P', str(OUT / 'workload.properties')]
    job('load', base_cmd + ['-load', '-p', 'status.interval=1000', '-p', 'maxexecutiontime=600'])
    event('load_started', records=opts.records, field_length=opts.field_length,
          expected_value_bytes=opts.records * opts.field_length)
    await_job('load', 650)
    if opts.canary_mib:
        canary('load')
    count = int(cmd('knode2', ['redis-cli', '-p', str(opts.port), 'DBSIZE']))
    if count != opts.records + 2 + bool(opts.canary_mib):
        raise RuntimeError('Unexpected key count after load: ' + str(count))
    STATE['source_memory'] = cmd('knode2', ['redis-cli', '-p', str(opts.port), 'INFO', 'memory'])
    STATE['source_key_count'] = count
    save()
    runtime_snapshot('knode2', 'source-before-workload')
    job('run', base_cmd + ['-t'])
    event('workload_started')
    time.sleep(opts.warmup)
    STATE['source_memory_before_migration'] = cmd('knode2', ['redis-cli', '-p', str(opts.port), 'INFO', 'memory'])
    pid = int(cmd('knode2', ['docker', 'inspect', '-f', '{{.State.Pid}}', NAME]))
    STATE['source_pid'] = pid
    mig = f'/var/lib/criu/migrate_{pid}'
    STATE['migration_dir'] = mig
    rule = ['-d', '10.0.0.62/32', '-p', 'tcp', '--dport', str(opts.port),
            '-m', 'comment', '--comment', NAME, '-j', 'DNAT',
            '--to-destination', '10.0.0.63:' + str(opts.port)]
    STATE['nat_rule'] = rule
    save()
    config = '[criu]\nlazy-pages=yes\naddress=0.0.0.0\nport=12346\nsync_addr=10.0.0.63\nsync_port=4568\n'
    config += precopy_config
    if opts.image_rdma:
        config += 'image-rdma=yes\n'
    py('knode2', f'from pathlib import Path;p=Path({mig!r});p.mkdir();(p/"imgs_dir").mkdir();(p/"work_dir").mkdir();(p/"config_ck.cfg").write_text({config!r})')
    if opts.fast_cutover:
        if not opts.image_rdma:
            raise RuntimeError('--fast-cutover currently requires --image-rdma')
        cutover = dict(name=NAME, token=secrets.token_hex(24), port=opts.port, nat_rule=rule,
                       stop_file=f'/dev/shm/swiftbaton-images-{pid}/stop')
        path = str(OUT / 'cutover-config.json')
        py('knode1', f'from pathlib import Path;p=Path({path!r});p.write_text({json.dumps(cutover)!r});p.chmod(0o600)')
        helper = str(SCRIPT_ROOT / 'fast_cutover.py')
        spawn('knode1', 'cutover_listener', ['python3', helper, 'listen', path])
        wait_for('knode1', str(OUT / 'cutover.ready'), 10)
        cutover['listen_port'] = json.loads(cmd('knode1', ['sudo', '-n', 'cat', str(OUT / 'cutover.ready')]))['port']
        (OUT / 'cutover-config.json').write_text(json.dumps(cutover))
        (OUT / 'cutover-config.json').chmod(0o600)
        spawn('knode2', 'cutover_trigger', ['python3', helper, 'trigger', path])
        wait_for('knode2', str(OUT / 'cutover.connected'), 12)
        event('fast_cutover_armed')
    if opts.kernel_transfer:
        for host, running in (('knode2', True), ('knode3', False)):
            info = json.loads(cmd(host, ['docker', 'inspect', STATE[host+'_cid']]))[0]
            validate_container(info, STATE[host+'_cid'], running)
            (OUT/('container-before-'+host+'.json')).write_text(json.dumps(info,indent=2)+'\n')
    check_memory_children('knode2', 'source')
    event('checkpoint_start', source_pid=pid)
    spawn('knode2', 'checkpoint', ['docker', 'checkpoint', 'create', NAME, 'migrate_dir'])
    wait_for('knode2', mig + '/tmpdir.txt')
    source_images = py('knode2', f'from pathlib import Path;print(Path({mig + "/tmpdir.txt"!r}).read_text().strip())')
    source_bootstrap = source_images
    if opts.image_rdma:
        source_images = f'/dev/shm/swiftbaton-images-{pid}'
        wait_for('knode2', source_images + '/psroot')
        STATE['ram_images'] = source_images
        py('knode2', f'from pathlib import Path;Path({source_images + "/ae-owner"!r}).write_text({NAME!r})')
    STATE['source_images'] = source_images
    save()
    py('knode3', f'from pathlib import Path;p=Path({mig!r});p.mkdir();(p/"imgs_dir").mkdir();(p/"work_dir").mkdir()')
    checkpoint_dir = '/var/lib/docker/containers/' + STATE['knode3_cid'] + '/checkpoints/migrate_dir'
    if opts.image_rdma:
        py('knode3', f'from pathlib import Path;p=Path({source_images!r});p.mkdir(mode=0o700);link=Path({mig + "/imgs_dir"!r});link.rmdir();link.symlink_to(p);cp=Path({checkpoint_dir!r});cp.mkdir();(cp/"psroot").write_text({str(pid)!r}+"\\n")')
        py('knode3', f'from pathlib import Path;Path({source_images + "/ae-owner"!r}).write_text({NAME!r})')
        # runc reads this JSON before it launches the CRIU RDMA receiver.
        # It is runtime bootstrap text; every CRIU .img travels through RDMA.
        descriptors = cmd('knode2', ['sudo', '-n', 'cat', source_bootstrap + '/descriptors.json'])
        py('knode3', f'from pathlib import Path;Path({checkpoint_dir + "/descriptors.json"!r}).write_text({descriptors!r})')
        event('checkpoint_ram_ready', source_images=source_images)
    else:
        cmd('knode3', ['sudo', '-n', 'sshfs', f'root@10.0.0.62:{source_images}', mig + '/imgs_dir',
                      '-o', 'allow_other,cache=no,entry_timeout=0,attr_timeout=0,negative_timeout=0,ServerAliveInterval=5,ServerAliveCountMax=3'])
        event('checkpoint_mounted', source_images=source_images)
        cmd('knode3', ['sudo', '-n', 'cp', '-a', mig + '/imgs_dir', checkpoint_dir])
    config = f'[criu]\nlazy-pages=yes\naddress=10.0.0.62\nport=12346\nsync_addr=10.0.0.62\nsync_port=4568\nimgs_dir={mig}/imgs_dir\n'
    config += precopy_config
    if opts.image_rdma:
        config += 'image-rdma=yes\n'
    py('knode3', f'from pathlib import Path;Path({mig + "/config_res.cfg"!r}).write_text({config!r})')
    spawn('knode3', 'restore', ['docker', 'start', '--checkpoint', 'migrate_dir', NAME])
    work = '/run/containerd/io.containerd.runtime.v2.task/moby/' + STATE['knode3_cid'] + '/work'
    wait_for('knode3', work + '/sync.sock')
    if opts.kernel_transfer:
        spawn('knode3', 'pageclient', ['/usr/bin/criu', 'lazy-pages', '-D', mig + '/imgs_dir', '-W', work,
              '--page-server', '--address', '10.0.0.62', '--port', '12346', '-v4'] +
              kernel_settings.argv() + ['--precopy-limit-mb', str(opts.precopy_limit_mb)])
    else:
        spawn('knode3', 'pageclient', ['criu', 'lazy-pages', '-D', mig + '/imgs_dir', '-W', work,
                                     '--page-server', '--address', '10.0.0.62', '--port', '12346', '-v4'] +
              (['--image-rdma'] if opts.image_rdma else []) +
              (['--u-precopy', '--precopy-workers', str(opts.precopy_workers),
                '--precopy-limit-mb', str(opts.precopy_limit_mb)] if opts.u_precopy else []) +
              (['--parent-stage'] if opts.parent_stage else []) +
              (['--parallel-transfer', '--install-workers', str(opts.install_workers),
                '--batch-pages', str(opts.batch_pages), '--bg-segment-pages', str(opts.bg_segment_pages), '--copy-workers', str(opts.copy_workers), '--fault-read-batch', str(opts.fault_read_batch), '--fault-install-workers', str(opts.fault_install_workers), '--fault-workers', str(opts.fault_workers), '--prefetch-workers', str(opts.prefetch_workers)] if opts.parallel_transfer else []) +
              ['--' + option.replace('_', '-') for option in ['no_prefetch', 'no_hot_first', 'no_pretransfer','sync_fault_transport','fault_trace','reader_preferred_lock','spin_lifecycle','fixed_ready_scan','serial_prefetch_install','serial_background_install','no_bg_fault_assist','bg_round_robin','install_trace','compact_bg_wire','serial_ps_prepare','defer_fault_credits'] if getattr(opts, option)] +
              (['--rdma-mtu',str(opts.rdma_mtu)] if opts.rdma_mtu else []) +
              (['--prefetch-window',str(opts.prefetch_window)] if opts.parallel_transfer else []) +
              (['--page-trace', str(OUT / 'page-trace.csv')] if opts.page_trace else []))
    event('waiting_for_source_stop')
    wait_for('knode2' if opts.image_rdma else 'knode3', source_images + '/stop' if opts.image_rdma else mig + '/imgs_dir/stop', 60)
    event('source_stop_observed')
    if opts.fast_cutover:
        wait_for('knode2', str(OUT / 'cutover.result.json'), 30)
        result = json.loads((OUT / 'cutover.result.json').read_text())
        STATE['nat_installed'] = result.get('nat_installed', False)
        save()
        if not result['ok']:
            raise RuntimeError('Fast network cutover failed: ' + result.get('error', 'unknown error'))
        event('network_cutover', event_time_ns=result['source_ack_time_ns'],
              client_time_ns=result['completed_time_ns'], trigger_to_ack_ms=result['trigger_to_ack_ms'])
    else:
        cmd('knode1', ['sudo', '-n', 'iptables', '-w', '10', '-t', 'nat', '-I', 'OUTPUT', '1'] + rule)
        STATE['nat_installed'] = True
        save()
        cmd('knode1', ['sudo', '-n', 'conntrack', '-D', '-p', 'tcp', '--dst', '10.0.0.62',
                      '--dport', str(opts.port)], check=False)
        event('network_cutover', client_time_ns=py('knode1', 'import time;print(time.time_ns())'))
    event('waiting_for_restore')
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        check_migration_jobs()
        result = cmd('knode3', ['timeout', '2', 'redis-cli', '-p', str(opts.port), 'GET', 'ae:sentinel'], timeout=5, check=False)
        if result == NAME:
            event('sentinel_verified', value=result)
            STATE['restore_verified'] = True
            save()
            break
        time.sleep(.5)
    else:
        raise TimeoutError('Restored Redis did not return the source sentinel')
    if opts.kernel_transfer:
        # K returns only after marker retirement/workqueue drain and source ACK.
        # It never creates U's pages.complete, and owns source process termination.
        deadline = time.monotonic() + 120
        while True:
            check_migration_jobs()
            statuses = {label: py(host, f'from pathlib import Path;p=Path({str(OUT)!r})/{label + ".status"!r};print(p.read_text() if p.exists() else "running")')
                        for host, label in [('knode2','checkpoint'),('knode3','restore'),('knode3','pageclient')]}
            info = json.loads(cmd('knode2', ['docker','inspect', STATE['knode2_cid']]))[0]
            if all(value == '0' for value in statuses.values()) and not info['State']['Running']:
                break
            if time.monotonic() >= deadline:
                raise TimeoutError('K migration jobs/source retirement not complete: ' + str(statuses))
            time.sleep(.15)
        capture_logs()
        stats = validate_completion({key:int(value) for key,value in statuses.items()}, info,
            STATE['knode2_cid'], (OUT/'dump.log').read_text(errors='replace'),
            (OUT/'pageclient.log').read_text(errors='replace'))
        STATE['kernel_stats'] = stats
        mode = STATE['kernel_preflight']['hosts']['knode3'].get('session_dispatch')
        if mode is not None:
            STATE['kernel_dispatch'] = validate_dispatch((OUT/'pageclient.log').read_text(errors='replace'),
                mode == 'Y', opts.prefetch_workers, opts.install_workers)
        STATE['kernel_export_settings'] = validate_export_config((OUT/'dump.log').read_text(errors='replace'),
            opts.kernel_export_workers, opts.kernel_export_chunk_mb)
        STATE['kernel_ps_settings'] = validate_ps_config((OUT/'dump.log').read_text(errors='replace'),
            opts.kernel_ps_chunk_mb, opts.no_pretransfer)
        completions = {label: json.loads(py(host, f'from pathlib import Path;print((Path({str(OUT)!r})/{label + ".completion.json"!r}).read_text())'))
                       for host, label in [('knode2','checkpoint'),('knode3','restore'),('knode3','pageclient')]}
        if any(row['exit_code'] != 0 or row['time_ns'] <= 0 for row in completions.values()):
            raise RuntimeError('Inconsistent K command completion timestamps')
        (OUT/'kernel-completion.json').write_text(json.dumps({'statuses':statuses,
            'source':info,'stats':stats,'commands':completions},indent=2)+'\n')
        # Use the source command's own completion time, not the later log-copy
        # observation. It is a conservative marker after final ACK/MR revocation.
        event('all_pages_copied', event_time_ns=completions['checkpoint']['time_ns'], kernel_stats=stats,
              marker='source checkpoint command returned successfully after K drain/ACK')
    else:
        wait_for('knode3', work + '/pages.complete', 90)
        event('all_pages_copied')
        current_pid = int(cmd('knode2', ['docker', 'inspect', '-f', '{{.State.Pid}}', NAME]))
        if current_pid != pid:
            raise RuntimeError('Source PID changed; refusing to retire an unrecognized process')
        cmd('knode2', ['sudo', '-n', 'kill', '-KILL', str(pid)])
        py('knode2', f'''from pathlib import Path
import os,signal
for p in Path('/proc').iterdir():
 if not p.name.isdigit():continue
 try:a=(p/'cmdline').read_bytes().replace(b'\\0',b' ').decode(errors='replace')
 except OSError:continue
 if a.startswith('criu: dump --rpc -t {pid} '):
  try:os.kill(int(p.name),signal.SIGKILL)
  except ProcessLookupError:pass
''')
    STATE['source_retired'] = True
    save()
    event('source_retired', event_time_ns=completions['checkpoint']['time_ns'] if opts.kernel_transfer else None)
    await_job('run', opts.duration + 60)
    runtime_snapshot('knode3', 'target-after-workload')
    if opts.poststeady_seconds:
        # A separate JVM and log preserve the original recovery measurement.
        job('poststeady', base_cmd + ['-t', '-p', 'maxexecutiontime=' + str(opts.poststeady_seconds)])
        event('poststeady_started', duration=opts.poststeady_seconds)
        await_job('poststeady', opts.poststeady_seconds + 60)
        runtime_snapshot('knode3', 'target-after-poststeady')
    STATE['destination_key_count'] = int(cmd('knode3', ['redis-cli', '-p', str(opts.port), 'DBSIZE']))
    STATE['destination_memory'] = cmd('knode3', ['redis-cli', '-p', str(opts.port), 'INFO', 'memory'])
    if STATE['destination_key_count'] != opts.records + 2 + bool(opts.canary_mib):
        raise RuntimeError('Key count changed across migration')
    event('key_count_verified', count=STATE['destination_key_count'])
    # Exercise the same address that the YCSB client used after the cutover.
    actual = cmd('knode1', ['timeout', '5', 'redis-cli', '-h', '10.0.0.62', '-p', str(opts.port),
                          'GET', 'ae:sentinel'])
    if actual != NAME:
        raise RuntimeError('Client endpoint does not reach the migrated Redis')
    event('client_endpoint_verified')
    if opts.canary_mib:
        canary('verify')
    verifier = Path(__file__).with_name('verify_redis.py').read_text()
    verify_args = ['python3', '-', '--host', '10.0.0.62', '--port', str(opts.port),
                   '--records', str(opts.records), '--field-length', str(opts.field_length), '--sentinel', NAME,
                   '--extra-keys', str(int(bool(opts.canary_mib)))]
    verification = subprocess.run(
        ['ssh', '-oBatchMode=yes', 'knode1', shlex.join(verify_args)],
        input=verifier, text=True, capture_output=True, timeout=180)
    (OUT / 'validation.json').write_text(verification.stdout)
    (OUT / 'validation.stderr').write_text(verification.stderr)
    if verification.returncode:
        raise RuntimeError('Full record validation failed; see validation.json and validation.stderr')
    STATE['validation'] = json.loads(verification.stdout)
    event('all_records_verified', checked=STATE['validation']['checked_records'])
    check_memory_children('knode3', 'target')
    if opts.cow_descendants and not opts.kernel_transfer:
        capture_logs()
        text = (OUT / 'restore.log').read_text(errors='replace')
        preserved = {int(pid): int(size) for pid, size in re.findall(
            r'SB_FORK private_unneeded pid=(\d+) regions=\d+ bytes=\d+ cow_preserved_bytes=(\d+)', text)}
        rows = json.loads((OUT / 'memory-children-target.json').read_text())
        if any(preserved.get(r['pid'], 0) < opts.child_mib * 1024 * 1024
               for r in rows[:opts.memory_children]):
            raise RuntimeError('COW parent mappings were not retained by the restore fork filter')
        result = {'cow_preserved_bytes_by_pid': preserved, 'parent_child_content_and_counters_verified': True}
        (OUT / 'fork-validation.json').write_text(json.dumps(result, indent=2))
        event('cow_inheritance_verified', result=result)
    if opts.dynamic_memory:
        capture_logs()
        lines = [line for line in (OUT / 'pageclient.log').read_text(errors='replace').splitlines() if 'SB_LIFECYCLE ' in line]
        if len(lines) != 1:
            raise RuntimeError('Missing lifecycle accounting')
        lifecycle = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', lines[0])}
        if any(lifecycle.get(k, 0) < memory_processes for k in ['remaps', 'removes', 'unmaps', 'forks']) or not lifecycle.get('zeroes'):
            raise RuntimeError('Dynamic operations did not exercise every UFFD event during AS: ' + str(lifecycle))
        (OUT / 'lifecycle-validation.json').write_text(json.dumps(lifecycle, indent=2))
        event('dynamic_lifecycle_verified', counts=lifecycle)
    if opts.fd_placeholder:
        capture_logs()
        restore_text = (OUT / 'restore.log').read_text(errors='replace')
        prepared = {int(k): int(n) for k, n in re.findall(r'SB_FDPOOL prepared kind=(\d+) requested=\d+ created=(\d+)', restore_text)}
        consumed = [{'pid': int(pid), 'kind': int(k), 'reused': int(n), 'fallback': int(m)}
                    for pid, k, n, m in re.findall(r'SB_FDPOOL consumed pid=(\d+) kind=(\d+) reused=(\d+) fallback=(\d+)', restore_text)]
        reused = {k: sum(r['reused'] for r in consumed if r['kind'] == k) for k in range(7)}
        contexts = re.findall(r'SB_FDPOOL enter pid=(\d+) context=(\d+)', restore_text)
        result = {'prepared': prepared, 'reused': reused, 'per_process': consumed,
                  'contexts': contexts, 'actual_ps_objects_consumed': sum(reused.values()),
                  'note': 'Blank object creation only. Final image still restores all state; unsupported types/context mismatch/underprediction use original creation.'}
        (OUT / 'fd-placeholder-validation.json').write_text(json.dumps(result, indent=2))
        required = [0, 2, 3, 4] if opts.fd_groups else [2, 3]
        if any(not reused[k] or reused[k] > prepared.get(k, 0) for k in required):
            raise RuntimeError('FD placeholder did not actually supply each required object kind: ' + str(result))
        event('fd_placeholder_verified', result=result)
    if opts.vma_cache:
        capture_logs()
        dump_text = (OUT / 'dump.log').read_text(errors='replace')
        hits = [int(p) for p in re.findall(r'SB_VMA_CACHE hit pid=(\d+)', dump_text)]
        result = {'hit_pids': hits, 'redis_pid': STATE['source_pid'],
                  'redis_cache_used': STATE['source_pid'] in hits,
                  'monitor_ready': 'SB_VMA_CACHE monitor_ready' in dump_text,
                  'note': 'Unchanged monitored mappings may reuse PS smaps; other processes fall back to live smaps.'}
        (OUT / 'vma-cache-validation.json').write_text(json.dumps(result, indent=2))
        if not result['redis_cache_used']:
            raise RuntimeError('Redis did not exercise the VMA cache; see vma-cache-validation.json')
        event('vma_cache_verified', result=result)
    STATE['success'] = True
    save()
except Exception as e:
    event('failed', error=str(e))
    STATE['success'] = False
    STATE['error'] = str(e)
    save()
    raise
finally:
    if 'checkpoint_pid' in STATE or 'restore_pid' in STATE:
        capture_logs()
    print('STATE=' + str(OUT / 'state.json'), flush=True)
