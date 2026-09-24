#!/usr/bin/env python3
"""Load or byte-verify deterministic immutable data alongside YCSB writes."""
import argparse
import hashlib
import json
import socket

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('mode', choices=['load', 'verify'])
p.add_argument('--host', required=True)
p.add_argument('--port', type=int, required=True)
p.add_argument('--mib', type=int, required=True)
p.add_argument('--seed', required=True)
a = p.parse_args()
if not 1 <= a.mib <= 1024:
    p.error('mib must be 1..1024')
s = socket.create_connection((a.host, a.port), timeout=10)
s.settimeout(30)
f = s.makefile('rb')

def reply():
    line = f.readline()
    if not line:
        raise EOFError('Redis closed the connection')
    tag, value = line[:1], line[1:-2]
    if tag == b':':
        return int(value)
    if tag == b'+':
        return value
    if tag == b'-':
        raise RuntimeError(value.decode())
    if tag == b'$':
        n = int(value)
        if n < 0:
            return None
        value = f.read(n)
        if len(value) != n or f.read(2) != b'\r\n':
            raise ValueError('Incomplete bulk reply')
        return value
    if tag == b'*':
        return [reply() for _ in range(int(value))]
    raise ValueError('Invalid RESP reply')

def command(*parts):
    parts = [x if isinstance(x, bytes) else str(x).encode() for x in parts]
    s.sendall(b'*%d\r\n' % len(parts) + b''.join(b'$%d\r\n' % len(x) + x + b'\r\n' for x in parts))
    return reply()

count = a.mib * 256
digest = hashlib.sha256()
for first in range(0, count, 64):
    fields = list(range(first, min(first + 64, count)))
    expected = [hashlib.shake_256((a.seed + '/' + str(i)).encode()).digest(4096) for i in fields]
    if a.mode == 'load':
        pairs = [part for i, value in zip(fields, expected) for part in (i, value)]
        command('HSET', 'ae:canary', *pairs)
        actual = expected
    else:
        actual = command('HMGET', 'ae:canary', *fields)
        if actual != expected:
            bad = [i for i, got, want in zip(fields, actual, expected) if got != want]
            raise RuntimeError('Canary value mismatch at fields ' + str(bad))
    for value in actual:
        digest.update(value)
if command('HLEN', 'ae:canary') != count:
    raise RuntimeError('Canary field count changed')
print(json.dumps(dict(mode=a.mode, bytes=count * 4096, fields=count,
                      sha256=digest.hexdigest(), byte_verified=a.mode == 'verify')))
