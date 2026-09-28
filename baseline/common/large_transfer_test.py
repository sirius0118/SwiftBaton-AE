#!/usr/bin/env python3
"""Verify rsocket image streaming across the old long-session stall boundary.

Run on Node2. Owns and removes only its unique /tmp test directories.
"""
import argparse
import hashlib
from pathlib import Path
import shlex
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--gib', type=float, default=4.5)
a = p.parse_args()
if not .01 <= a.gib <= 12:
    p.error('--gib must be 0.01..12')
size = int(a.gib * 1024 ** 3)
name = 'sb-rsocket-large-%d' % int(time.time())
src = Path('/tmp') / name
dst = Path('/tmp') / name
relay = ROOT / 'rsocket_relay'
stream = ROOT / 'tree_stream.py'
port = 28000 + (int(time.time()) % 3000) * 3
target_pids = []
source_relays = []
lanes = 8


def remote(argv, input_bytes=None, timeout=30):
    return subprocess.run(['ssh', '-oBatchMode=yes', 'knode3', shlex.join(list(map(str, argv)))],
                          input=input_bytes, capture_output=True, check=True,
                          timeout=timeout).stdout.decode().strip()


def launch(argv, label):
    code = ('import subprocess,sys; p=subprocess.Popen(sys.argv[2:],'
            'stdout=open(sys.argv[1],"w"),stderr=subprocess.STDOUT,start_new_session=True);'
            'print(p.pid)')
    pid = int(remote(['python3', '-c', code, dst / (label + '.log'), *argv]))
    target_pids.append(pid)


try:
    src.mkdir()
    (src / 'images').mkdir()
    with (src / 'images/pages.img').open('wb') as image:
        image.truncate(size)
        # Sparse zero data can hide reordered segments. Put distinct content at
        # each segment boundary and at EOF, then verify the whole received file.
        for offset in range(0, size, 128 * 1024 * 1024):
            image.seek(offset)
            image.write(hashlib.sha256(str(offset).encode()).digest() * 128)
        image.seek(size - 4096)
        image.write(hashlib.sha256(b'end' + str(size).encode()).digest() * 128)
    remote(['mkdir', '-m', '700', dst])
    remote(['sh', '-c', 'cat > ' + shlex.quote(str(dst / 'stream.py'))], stream.read_bytes())
    remote(['sh', '-c', 'cat > ' + shlex.quote(str(dst / 'relay'))], relay.read_bytes())
    remote(['chmod', '+x', dst / 'relay'])
    launch([dst / 'relay', 'listen', '10.0.0.63', port + 1,
            '127.0.0.1', port], 'relay-control')
    target_data_ports = []
    source_data_ports = []
    for i in range(lanes):
        target_data = port + 3 + 3 * i
        rdma_data = target_data + 1
        source_data = target_data + 2
        target_data_ports.append(target_data)
        source_data_ports.append(source_data)
        launch([dst / 'relay', 'listen', '10.0.0.63', rdma_data,
                '127.0.0.1', target_data], 'relay-data-%d' % i)
    launch(['python3', dst / 'stream.py', 'receive', dst / 'received',
            '--port', port, '--data-ports', ','.join(map(str, target_data_ports))], 'receiver')
    for label, local, rdma in [('control', port + 2, port + 1)] + [
            ('data-%d' % i, source_data_ports[i], target_data_ports[i] + 1)
            for i in range(lanes)]:
        log = (src / ('relay-' + label + '.log')).open('w')
        source_relays.append(subprocess.Popen([relay, 'connect', '127.0.0.1',
            str(local), '10.0.0.63', str(rdma)],
            stdout=log, stderr=subprocess.STDOUT))
        log.close()
    time.sleep(.5)
    output = subprocess.check_output(['python3', stream, 'send', src / 'images',
                                      '--port', str(port + 2), '--data-ports',
                                      ','.join(map(str, source_data_ports))],
                                     text=True, timeout=600).strip()
    actual = int(remote(['stat', '-c', '%s', dst / 'received/pages.img']))
    if actual != size:
        raise RuntimeError('received size %d != %d' % (actual, size))
    local_hash = subprocess.check_output(['sha256sum', src / 'images/pages.img'],
                                         text=True).split()[0]
    target_hash = remote(['sha256sum', dst / 'received/pages.img']).split()[0]
    if target_hash != local_hash:
        raise RuntimeError('received image content digest mismatch')
    print('PASS size=%d %s' % (size, output), flush=True)
finally:
    for source_relay in source_relays:
        source_relay.terminate()
        try: source_relay.wait(timeout=3)
        except subprocess.TimeoutExpired: source_relay.kill(); source_relay.wait()
    for pid in target_pids:
        subprocess.run(['ssh', '-oBatchMode=yes', 'knode3',
                        shlex.join(['kill', '-TERM', str(pid)])],
                       capture_output=True, timeout=10)
    subprocess.run(['ssh', '-oBatchMode=yes', 'knode3',
                    shlex.join(['rm', '-rf', '--', str(dst)])],
                   capture_output=True, timeout=60)
    subprocess.run(['rm', '-rf', '--', str(src)], check=True)
