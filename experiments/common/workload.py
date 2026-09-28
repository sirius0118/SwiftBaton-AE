"""Workload-specific hooks for the existing U/K migration drivers.

The migration protocol remains in scripts/ae/{u,k}/run_ae.py.  This module
only supplies container, client and validation details, so an experiment may
change the service without changing the migration timing path.
"""
from __future__ import annotations

import json
import os
import shlex
import socket
import subprocess
import sys
import time
from pathlib import Path


PORTS = {'redis': 6379, 'memcached': 11211, 'voltdb': 21212,
         'mysql': 3306, 'largecontainer': 6379}
IMAGES = {'redis': 'm.daocloud.io/docker.io/library/redis:latest',
          'memcached': 'memcached:latest', 'voltdb': 'voltdb:latest',
          'mysql': 'mysql:5.7', 'largecontainer': 'swiftbaton/largecontainer:ae'}
BINDINGS = {'redis': ('redis', 'site.ycsb.db.RedisClient'),
            'memcached': ('memcached', 'site.ycsb.db.MemcachedClient'),
            'voltdb': ('voltdb', 'site.ycsb.db.voltdb.VoltClient4'),
            'mysql': ('jdbc', 'site.ycsb.db.JdbcDBClient')}


def image(kind):
    return os.environ.get('SB_AE_IMAGE', IMAGES[kind])


def container(kind, port, opts):
    """Return image, command, extra create arguments, and container port."""
    if kind == 'redis':
        return image(kind), ['redis-server', '--save', '', '--appendonly', 'no'], [], 6379
    if kind == 'memcached':
        # Five million 1 KiB objects need well over 5 GiB with allocator overhead.
        return image(kind), ['memcached', '-m', str(max(256, opts.records * opts.field_length // 700000)), '-I', '2m', '-u', 'memcache'], [], 11211
    if kind == 'voltdb':
        # VoltDB's rotating log is not part of the in-memory benchmark state;
        # a regular file that keeps growing after the PS staging copy would
        # make CRIU's restore-side file-size validation reject the checkpoint.
        startup = ('rm -f /var/voltdb/voltdbroot/log/volt.log; '
                   'ln -s /dev/null /var/voltdb/voltdbroot/log/volt.log; '
                   'exec voltdb start -H localhost')
        return image(kind), ['sh', '-c', startup], [], 21212
    if kind == 'mysql':
        return (image(kind),
                ['mysqld', '--skip-log-bin', '--max-heap-table-size=8589934592',
                 '--tmp-table-size=8589934592'],
                ['--env', 'MYSQL_ROOT_PASSWORD=swiftbaton-benchmark',
                 '--env', 'MYSQL_DATABASE=ycsb'], 3306)
    if kind == 'largecontainer':
        return image(kind), ['--mib', str(opts.large_mib), '--threads', str(opts.large_workers),
                             '--range-kib', str(opts.large_range_kib), '--zipf', str(opts.zipf_zeta),
                             '--write-ratio', str(opts.write_ratio), '--sleep-us', str(opts.large_sleep_us),
                             '--max-iops', str(opts.large_thread_iops)], [], 6379
    raise ValueError(kind)


def properties(kind, opts):
    props = [
        'workload=site.ycsb.workloads.CoreWorkload', f'recordcount={opts.records}',
        'operationcount=1000000000', 'fieldcount=1', f'fieldlength={opts.field_length}',
        'fieldlengthdistribution=constant', 'zeropadding=32',
        f'readproportion={1 - opts.write_ratio:.6f}', f'updateproportion={opts.write_ratio:.6f}',
        'scanproportion=0', 'insertproportion=0',
        'requestdistribution=zipfian', f'zipfzeta={opts.zipf_zeta}',
        'readallfields=true', f'threadcount={opts.threads}',
        'status.interval=10', 'measurementtype=hdrhistogram',
        f'maxexecutiontime={opts.duration}',
    ]
    if kind == 'redis': props += [f'redis.host=10.0.0.62', f'redis.port={opts.port}', 'redis.timeout=100']
    elif kind == 'memcached': props += [f'memcached.hosts=10.0.0.62:{opts.port}', 'memcached.opTimeoutMillis=100']
    elif kind == 'voltdb': props += ['voltdb.servers=10.0.0.62:' + str(opts.port)]
    elif kind == 'mysql': props += [
        'db.driver=com.mysql.cj.jdbc.Driver',
        f'db.url=jdbc:mysql://10.0.0.62:{opts.port}/ycsb?useSSL=false&allowPublicKeyRetrieval=true',
        'db.user=root', 'db.passwd=swiftbaton-benchmark', 'table=usertable']
    return '\n'.join(props) + '\n'


def classpath(base, kind):
    module, _ = BINDINGS[kind]
    root = Path(base) / ('build/YCSB' if kind == 'redis' else 'build/benchmark-YCSB')
    parts = [root / 'core/target/classes', root / f'{module}/target/classes',
             root / f'{module}/target/dependency/*', root / 'core/target/dependency/*']
    if kind == 'mysql':
        parts.append(root / 'mysql-connector-java.jar')
    return ':'.join(str(x) for x in parts)


def client_command(base, kind, opts, properties_file):
    if kind == 'largecontainer':
        return ['python3', str(Path(base) / 'experiments/real_world/largecontainer/client.py'),
                '--host', '10.0.0.62', '--port', str(opts.port), '--seconds', str(opts.duration),
                '--threads', str(opts.threads), '--zipf', str(opts.zipf_zeta),
                '--write-ratio', str(opts.write_ratio), '--records', str(opts.records)]
    return ['java', '-Xms1g', '-Xmx4g', '-cp', classpath(base, kind),
            'site.ycsb.Client', '-db', BINDINGS[kind][1], '-s', '-P', str(properties_file)]


def wait_tcp(host, port, seconds=90):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            with socket.create_connection((host, port), timeout=.8):
                return
        except OSError:
            time.sleep(.3)
    raise TimeoutError(f'{host}:{port} was not ready within {seconds}s')


def prepare_database(kind, name, cmd, host='knode2'):
    if kind == 'mysql':
        sql = ('CREATE DATABASE IF NOT EXISTS ycsb; USE ycsb; '
               'CREATE TABLE IF NOT EXISTS usertable '
               '(YCSB_KEY VARCHAR(255) PRIMARY KEY, FIELD0 VARBINARY(16384)) ENGINE=MEMORY;')
        command = ['docker', 'exec', name, 'mysql', '-u', 'root', '-pswiftbaton-benchmark', '-e', sql]
        deadline = time.monotonic() + 90
        while True:
            try:
                cmd(host, command, timeout=15)
                return
            except RuntimeError:
                if time.monotonic() >= deadline:
                    raise
            time.sleep(1)


def stage_runtime_files(kind, name):
    """Copy service-created files into the stopped target container before PS.

    VoltDB's JNI .so files have random names under /tmp and remain mapped at
    restore.  MySQL's initial system tables must also have matching paths.
    This happens before the timed workload; it is not a substitute for a final
    changed-file synchronization when a workload writes persistent data.
    """
    paths = {'voltdb': ['/tmp', '/var/voltdb/voltdbroot'],
             'mysql': ['/var/lib/mysql']}.get(kind, [])
    for path in paths:
        source = subprocess.Popen(['docker', 'cp', name + ':' + path + '/.', '-'], stdout=subprocess.PIPE)
        target = subprocess.Popen(['ssh', '-oBatchMode=yes', 'knode3',
                                   shlex.join(['docker', 'cp', '-', name + ':' + path])],
                                  stdin=source.stdout)
        source.stdout.close()
        target_rc = target.wait()
        source_rc = source.wait()
        if source_rc or target_rc:
            raise RuntimeError(f'Could not stage {kind} {path}: source={source_rc}, target={target_rc}')


def probe(kind, host, port, name, cmd):
    if kind == 'redis':
        return cmd(host, ['timeout', '2', 'redis-cli', '-p', str(port), 'GET', 'ae:sentinel'], timeout=5, check=False) == name
    if kind == 'largecontainer':
        script = ("import socket; s=socket.create_connection(('127.0.0.1',"+str(port)+"),1);"
                  "s.sendall(b'PING\\n');print(s.recv(64).decode().strip())")
        return cmd(host, ['python3', '-c', script], timeout=5, check=False) == 'PONG'
    script = ("import socket; s=socket.create_connection(('127.0.0.1',"+str(port)+"),1);print('READY')")
    return cmd(host, ['python3', '-c', script], timeout=5, check=False) == 'READY'


def verify(kind, host, port, name, records, cmd):
    """Post-restore checks; full Redis validation stays in the original driver."""
    if kind == 'memcached':
        script = ("import socket; s=socket.create_connection(('127.0.0.1',"+str(port)+"),2);"
                  "s.sendall(b'stats\\r\\n');b=s.recv(4096);assert b'STAT ' in b;print(b.decode(errors='replace'))")
        raw = cmd(host, ['python3', '-c', script], timeout=8)
        count = next((int(x.split()[2]) for x in raw.splitlines() if x.startswith('STAT curr_items ')), None)
        if count is None or count < records * .99: raise RuntimeError(f'Memcached items after restore: {count}, expected {records}')
        return {'items': count}
    if kind == 'mysql':
        raw = cmd(host, ['docker', 'exec', name, 'mysql', '-N', '-u', 'root', '-pswiftbaton-benchmark',
                         '-e', 'SELECT COUNT(*) FROM ycsb.usertable'], timeout=30)
        count = int(raw.splitlines()[-1]);
        if count != records: raise RuntimeError(f'MySQL records after restore: {count}, expected {records}')
        return {'records': count}
    if kind == 'voltdb':
        raw = cmd(host, ['docker', 'exec', name, 'sqlcmd', '--query=SELECT COUNT(*) FROM Store;'], timeout=30)
        import re
        rows = [line.strip() for line in raw.splitlines()]
        counts = [int(line) for line in rows if re.fullmatch(r'\d+', line)]
        if not counts or counts[-1] < records * .99:
            raise RuntimeError('VoltDB record count unavailable or too small: ' + raw)
        return {'records': counts[-1]}
    if kind == 'largecontainer':
        script = ("import socket; s=socket.create_connection(('127.0.0.1',"+str(port)+"),2);"
                  "s.sendall(b'STATS\\n');print(s.recv(128).decode().strip())")
        raw = cmd(host, ['python3', '-c', script], timeout=5)
        count = int(raw.split()[0]);
        if count <= 0: raise RuntimeError('LargeContainer made no progress after restore')
        return {'operations': count}
    raise ValueError(kind)
