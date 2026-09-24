#!/usr/bin/env python3
"""Arm before checkpoint; signal knode1 directly when CRIU locks source networking."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import time


def write_json(path, data):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(data, indent=2) + '\n')
    os.replace(temporary, path)


def receive(stream):
    data = stream.readline(4097)
    if not data.endswith(b'\n') or len(data) > 4096:
        raise ValueError('Invalid cutover control frame')
    return json.loads(data)


def send(stream, data):
    stream.write(json.dumps(data).encode() + b'\n')
    stream.flush()


def command(argv, accepted=(0,)):
    p = subprocess.run(argv, capture_output=True, text=True, timeout=12)
    if p.returncode not in accepted:
        raise RuntimeError(f'{argv[0]} exited {p.returncode}: {p.stderr}')
    if p.returncode == 1 and '0 flow entries have been deleted' not in p.stderr:
        raise RuntimeError(f'{argv[0]} did not confirm an empty conntrack selection: {p.stderr}')
    return dict(returncode=p.returncode, stderr=p.stderr.strip())


def listen(config, out):
    with socket.socket() as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(('10.0.0.61', 0))
        server.listen(1)
        server.settimeout(180)
        write_json(out / 'cutover.ready', dict(port=server.getsockname()[1]))
        connection, address = server.accept()
        with connection:
            if address[0] != '10.0.0.62':
                raise ValueError('Control connection must come from knode2')
            connection.settimeout(180)
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            with connection.makefile('rwb') as stream:
                request = receive(stream)
                if request != dict(op='arm', token=config['token']):
                    raise ValueError('Invalid arm request')
                send(stream, dict(status='armed'))
                if receive(stream) != dict(op='cutover'):
                    raise ValueError('Invalid cutover request')
                start = time.monotonic_ns()
                result = dict(received_time_ns=time.time_ns())
                try:
                    result['iptables'] = command(
                        ['iptables', '-w', '10', '-W', '1000', '-t', 'nat', '-I', 'OUTPUT', '1']
                        + config['nat_rule'])
                    result['nat_installed'] = True
                    result['nat_time_ns'] = time.time_ns()
                    # conntrack returns 1 when no matching connections exist.
                    result['conntrack'] = command(
                        ['conntrack', '-D', '-p', 'tcp', '--dst', '10.0.0.62',
                         '--dport', str(config['port'])], accepted=(0, 1))
                    result['ok'] = True
                except Exception as error:
                    result.update(ok=False, error=str(error))
                result['completed_time_ns'] = time.time_ns()
                result['commands_ms'] = (time.monotonic_ns() - start) / 1e6
                write_json(out / 'cutover-client.json', result)
                send(stream, result)
                if not result['ok']:
                    raise RuntimeError(result['error'])


def trigger(config, out):
    with socket.create_connection(('10.0.0.61', config['listen_port']), timeout=10) as connection:
        connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        connection.settimeout(180)
        with connection.makefile('rwb') as stream:
            send(stream, dict(op='arm', token=config['token']))
            if receive(stream) != dict(status='armed'):
                raise ValueError('Cutover listener did not arm')
            write_json(out / 'cutover.connected', dict(time_ns=time.time_ns()))
            stop = Path(config['stop_file'])
            deadline = time.monotonic() + 180
            while not stop.exists():
                if time.monotonic() >= deadline:
                    raise TimeoutError('No source stop marker')
                time.sleep(.0005)
            observed_time_ns = time.time_ns()
            start = time.monotonic_ns()
            send(stream, dict(op='cutover'))
            result = receive(stream)
            result['source_observed_time_ns'] = observed_time_ns
            result['source_ack_time_ns'] = time.time_ns()
            result['trigger_to_ack_ms'] = (time.monotonic_ns() - start) / 1e6
            write_json(out / 'cutover.result.json', result)
            if not result['ok']:
                raise RuntimeError(result['error'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('role', choices=['listen', 'trigger'])
    parser.add_argument('config', type=Path)
    args = parser.parse_args()
    config = json.loads(args.config.read_text())
    if not config['name'].startswith('sb_ae_'):
        raise SystemExit('Not an AE run')
    if not 1024 <= config['port'] <= 65535:
        raise SystemExit('Invalid experiment port')
    out = args.config.parent
    try:
        (listen if args.role == 'listen' else trigger)(config, out)
    except Exception as error:
        write_json(out / ('cutover-' + args.role + '-error.json'), dict(error=str(error)))
        raise
