#!/usr/bin/env python3
"""10 ms external service-throughput sampler for the LargeContainer fixture."""
import argparse
import datetime
import socket
import time

p = argparse.ArgumentParser()
for flag, kind in [('host', str), ('port', int), ('seconds', int), ('threads', int),
                   ('zipf', float), ('write-ratio', float), ('records', int)]:
    p.add_argument('--' + flag, required=True, type=kind)
a = p.parse_args()
period = .01
start = time.monotonic()
base = last = None
previous_count = 0
while time.monotonic() - start < a.seconds:
    tick = time.monotonic()
    try:
        with socket.create_connection((a.host, a.port), timeout=.005) as conn:
            conn.settimeout(.005)
            conn.sendall(b'STATS\n')
            value = int(conn.recv(64).strip())
            if base is None: base = value
            if last is None or value >= last:
                last = value
            # A counter regression is an invalid migration, not a recovery.
            else:
                raise RuntimeError(f'service counter regressed: {last} -> {value}')
    except (OSError, ValueError):
        pass
    completed = max(0, (last or 0) - (base or 0))
    elapsed = max(time.monotonic() - start, .001)
    delta = max(0, completed - previous_count)
    now = datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8)))
    stamp = now.strftime('%Y-%m-%d %H:%M:%S:') + f'{now.microsecond // 1000:03d}'
    print(f'{stamp} 0 sec: {completed} operations; {delta / period:.1f} current ops/sec;', flush=True)
    previous_count = completed
    time.sleep(max(0, tick + period - time.monotonic()))
