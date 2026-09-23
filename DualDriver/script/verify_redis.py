#!/usr/bin/env python3
"""Check every indexed YCSB record after the source process has been retired."""
import argparse
import json
import socket
import time

p = argparse.ArgumentParser()
p.add_argument('--host', required=True)
p.add_argument('--port', type=int, required=True)
p.add_argument('--records', type=int, required=True)
p.add_argument('--field-length', type=int, default=1024)
p.add_argument('--sentinel', required=True)
p.add_argument('--extra-keys', type=int, default=0)
args = p.parse_args()
if args.records < 1 or not 1 <= args.field_length <= 1048576:
    p.error('records must be positive and field-length must be 1..1048576 bytes')
s = socket.create_connection((args.host, args.port), timeout=10)
s.settimeout(30)
f = s.makefile('rb')

def read():
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
        size = int(value)
        if size < 0:
            return None
        data = f.read(size)
        if f.read(2) != b'\r\n':
            raise ValueError('Invalid bulk reply')
        return data
    if tag == b'*':
        return [read() for _ in range(int(value))]
    raise ValueError('Unknown Redis reply')

def command(*parts):
    encoded = [str(v).encode() for v in parts]
    s.sendall(b'*%d\r\n' % len(parts) + b''.join(b'$%d\r\n' % len(v) + v + b'\r\n' for v in encoded))
    return read()

started = time.time_ns()
keys = command('DBSIZE')
indexed = command('ZCARD', '_indices')
sentinel = command('GET', 'ae:sentinel').decode()
assert indexed == args.records, (indexed, args.records)
assert keys == args.records + 2 + args.extra_keys, keys
assert sentinel == args.sentinel, sentinel
script = "local ks=redis.call('ZRANGE','_indices',ARGV[1],ARGV[2]); local expected=tonumber(ARGV[3]); local bad=0; for _,k in ipairs(ks) do if redis.call('HSTRLEN',k,'field0')~=expected then bad=bad+1 end end; return {#ks,bad}"
checked = bad = 0
for first in range(0, args.records, 1000):
    count, failures = command('EVAL', script, 0, first, min(first + 999, args.records - 1), args.field_length)
    checked += count
    bad += failures
result = dict(started_ns=started, finished_ns=time.time_ns(), endpoint=f'{args.host}:{args.port}',
              indexed_records=indexed, checked_records=checked, missing_or_wrong_length=bad,
              total_keys=keys, sentinel_verified=True, field_length=args.field_length,
              expected_value_bytes=args.records * args.field_length,
              validation=f'All indexed records have field0 of length {args.field_length}; this is not a byte-for-byte value checksum.')
print(json.dumps(result, indent=2))
assert checked == args.records and bad == 0, result
