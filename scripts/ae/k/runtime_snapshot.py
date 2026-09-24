#!/usr/bin/env python3
"""Read an owned AE container's runtime layout outside timed migration windows."""
import argparse
import json
import os
import subprocess
import time
from pathlib import Path


def read(path):
    try:
        return Path(path).read_text()
    except OSError as error:
        return {'error': str(error)}


def collect(name):
    container = json.loads(subprocess.check_output(['docker', 'inspect', name], text=True))[0]
    if not name.startswith('sb_ae_') or container['Config']['Labels'].get('swiftbaton.ae') != 'true':
        raise RuntimeError('Only owned AE containers may be inspected')
    pid = container['State']['Pid']
    if not container['State']['Running'] or pid <= 1:
        raise RuntimeError('Container is not running')
    root = Path('/proc') / str(pid)
    result = {'time_ns': time.time_ns(), 'container': name, 'container_id': container['Id'],
              'pid': pid, 'host': os.uname().nodename, 'clock_ticks': os.sysconf('SC_CLK_TCK'),
              'host_config': {key: container['HostConfig'].get(key) for key in
                              ['CpusetCpus', 'CpusetMems', 'CpuQuota', 'CpuPeriod', 'CpuShares', 'Memory']},
              'process': {}, 'tasks': {}, 'system': {}, 'cgroups': {}}
    start_stat = (root / 'stat').read_text().rsplit(')', 1)[1].split()[19]
    for name in ['stat', 'status', 'sched', 'schedstat', 'smaps_rollup', 'numa_maps', 'maps', 'cgroup']:
        result['process'][name] = read(root / name)
    for task in sorted((root / 'task').iterdir()):
        entry = {name: read(task / name) for name in ['stat', 'status', 'schedstat', 'comm']}
        try:
            entry['affinity'] = sorted(os.sched_getaffinity(int(task.name)))
        except OSError as error:
            entry['affinity_error'] = str(error)
        result['tasks'][task.name] = entry
    paths = ['/proc/loadavg', '/proc/stat', '/proc/meminfo', '/proc/vmstat', '/proc/pressure/cpu',
             '/proc/pressure/memory', '/proc/net/snmp', '/proc/net/netstat',
             '/sys/kernel/mm/transparent_hugepage/enabled', '/sys/kernel/mm/transparent_hugepage/defrag',
             '/sys/class/net/ens4f1/device/numa_node']
    for pattern in ['node/node[0-9]*/cpulist', 'node/node[0-9]*/meminfo',
                    'cpu/cpu[0-9]*/cpufreq/scaling_governor', 'cpu/cpu[0-9]*/cpufreq/scaling_cur_freq']:
        paths += [str(p) for p in Path('/sys/devices/system').glob(pattern)]
    result['system'] = {path: read(path) for path in paths}
    # The current testbed is cgroup v1; support v2 as well and preserve missing
    # counters explicitly, rather than inventing zero throttling.
    mounts = []
    for line in Path('/proc/self/mountinfo').read_text().splitlines():
        left, right = line.split(' - ', 1)
        a, b = left.split(), right.split()
        # CRIU can leave bind aliases in checkpoint namespaces. The canonical
        # hierarchy suffices; avoid rereading identical counters via each alias.
        if b[0] in ['cgroup', 'cgroup2'] and a[4].startswith('/sys/fs/cgroup/'):
            mounts.append((a[3], Path(a[4]), set(b[2].split(',')), b[0]))
    for line in (root / 'cgroup').read_text().splitlines():
        _, controllers, group = line.split(':', 2)
        wanted = set(controllers.split(',')) if controllers else set()
        for mount_root, mount, available, kind in mounts:
            if (kind == 'cgroup2' and not wanted) or (kind == 'cgroup' and wanted <= available):
                try:
                    path = mount / Path(group).relative_to(mount_root)
                except ValueError:
                    continue
                files = ['cpu.stat', 'cpu.max', 'cpu.cfs_quota_us', 'cpu.cfs_period_us', 'cpuacct.usage',
                         'cpuset.cpus', 'cpuset.mems', 'cpuset.cpus.effective', 'cpuset.mems.effective',
                         'memory.stat', 'memory.numa_stat']
                result['cgroups'][str(path)] = {f: read(path / f) for f in files if (path / f).exists()}
    if (root / 'stat').read_text().rsplit(')', 1)[1].split()[19] != start_stat:
        raise RuntimeError('Container PID changed during observation')
    result['finished_ns'] = time.time_ns()
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('container')
    print(json.dumps(collect(parser.parse_args().container), indent=2))
