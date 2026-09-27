#!/usr/bin/env python3
"""Stream a CRIU image tree through a local rsocket_relay TCP endpoint.

Every inter-host byte travels over RDMA. Entries are copied incrementally, so
large Redis image sets do not require a second in-memory copy of the tree.
"""
import argparse
import contextlib
import hashlib
import json
import os
import pathlib
import socket
import struct
import time

MAGIC = b'SB-TREE-2\n'
CHUNK = 1024 * 1024
SEGMENT = 128 * CHUNK


def entries(source):
    found = []
    for path in sorted(source.rglob('*')):
        if path.is_symlink() or not (path.is_file() or path.is_dir()):
            raise ValueError('unsupported image entry: ' + str(path))
        found.append(path)
    return found


def send(source, port, data_ports):
    found = entries(source)
    digest = hashlib.sha256()
    transferred = 0
    sessions = 0
    start = time.monotonic()
    with socket.create_connection(('127.0.0.1', port), timeout=15) as conn:
        conn.settimeout(300)
        conn.sendall(MAGIC + struct.pack('!Q', len(found)))
        for path in found:
            st = path.stat()
            header = json.dumps({'path': path.relative_to(source).as_posix(),
                                 'directory': path.is_dir(), 'mode': st.st_mode & 0o777,
                                 'size': st.st_size if path.is_file() else 0},
                                separators=(',', ':')).encode()
            if len(header) > 65536:
                raise ValueError('path too long: ' + str(path))
            framed = struct.pack('!I', len(header)) + header
            conn.sendall(framed)
            digest.update(framed)
            if path.is_file():
                with path.open('rb') as image:
                    remaining = st.st_size
                    while remaining:
                        segment = min(remaining, SEGMENT)
                        data_port = data_ports[sessions % len(data_ports)]
                        with socket.create_connection(('127.0.0.1', data_port), timeout=15) as data:
                            data.settimeout(300)
                            part = segment
                            while part:
                                block = image.read(min(CHUNK, part))
                                if not block:
                                    raise EOFError('image changed during transfer: ' + str(path))
                                data.sendall(block)
                                digest.update(block)
                                transferred += len(block)
                                part -= len(block)
                            data.shutdown(socket.SHUT_WR)
                            if recv_exact(data, 2) != b'OK':
                                raise RuntimeError('receiver rejected image segment')
                        remaining -= segment
                        sessions += 1
        conn.sendall(digest.digest())
        conn.shutdown(socket.SHUT_WR)
        if recv_exact(conn, 2) != b'OK':
            raise RuntimeError('receiver rejected image tree')
    print('files=%d payload_bytes=%d data_sessions=%d sha256=%s elapsed_ms=%.3f' %
          (len(found), transferred, sessions, digest.hexdigest(), (time.monotonic() - start) * 1000), flush=True)


def recv_exact(conn, count):
    output = bytearray()
    while len(output) < count:
        block = conn.recv(count - len(output))
        if not block:
            raise EOFError('truncated image transfer')
        output.extend(block)
    return bytes(output)


def safe_path(root, name):
    rel = pathlib.PurePosixPath(name)
    if rel.is_absolute() or not name or '..' in rel.parts or rel.as_posix() != name:
        raise ValueError('unsafe image path: ' + repr(name))
    return root.joinpath(*rel.parts)


def receive(destination, port, data_ports, max_bytes):
    if destination.exists():
        raise FileExistsError(destination)
    staging = destination.with_name(destination.name + '.receiving')
    if staging.exists():
        raise FileExistsError(staging)
    digest = hashlib.sha256()
    transferred = 0
    sessions = 0
    start = time.monotonic()
    with socket.socket() as listener, contextlib.ExitStack() as sockets:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(('127.0.0.1', port))
        listener.listen(1)
        data_listeners = []
        for data_port in data_ports:
            if data_port == port:
                data_listeners.append(listener)
                continue
            data_listener = sockets.enter_context(socket.socket())
            data_listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            data_listener.bind(('127.0.0.1', data_port))
            data_listener.listen(1)
            data_listeners.append(data_listener)
        with listener.accept()[0] as conn:
            conn.settimeout(300)
            if recv_exact(conn, len(MAGIC)) != MAGIC:
                raise ValueError('bad image stream magic')
            count = struct.unpack('!Q', recv_exact(conn, 8))[0]
            if count > 1000000:
                raise ValueError('too many image entries')
            staging.mkdir(parents=True)
            for _ in range(count):
                length_bytes = recv_exact(conn, 4)
                length = struct.unpack('!I', length_bytes)[0]
                if length > 65536:
                    raise ValueError('oversized image entry')
                header = recv_exact(conn, length)
                digest.update(length_bytes + header)
                info = json.loads(header)
                path = safe_path(staging, info['path'])
                size = info['size']
                if not isinstance(size, int) or size < 0 or transferred + size > max_bytes:
                    raise ValueError('image tree exceeds configured limit')
                if info['directory']:
                    if size:
                        raise ValueError('directory has data')
                    path.mkdir(parents=True, exist_ok=True)
                else:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    with path.open('xb') as image:
                        remaining = size
                        while remaining:
                            segment = min(remaining, SEGMENT)
                            with data_listeners[sessions % len(data_listeners)].accept()[0] as data:
                                data.settimeout(300)
                                part = segment
                                while part:
                                    block = recv_exact(data, min(CHUNK, part))
                                    image.write(block)
                                    digest.update(block)
                                    transferred += len(block)
                                    part -= len(block)
                                if data.recv(1):
                                    raise ValueError('segment exceeds declared size')
                                data.sendall(b'OK')
                            remaining -= segment
                            sessions += 1
                os.chmod(path, info['mode'] & 0o777)
            expected = recv_exact(conn, 32)
            if digest.digest() != expected:
                raise ValueError('image tree digest mismatch')
            staging.rename(destination)
            conn.sendall(b'OK')
    print('files=%d payload_bytes=%d data_sessions=%d sha256=%s elapsed_ms=%.3f' %
          (count, transferred, sessions, digest.hexdigest(), (time.monotonic() - start) * 1000), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('send', 'receive'))
    parser.add_argument('directory', type=pathlib.Path)
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--data-ports', help='comma-separated loopback data ports; defaults to --port')
    parser.add_argument('--max-gib', type=int, default=64)
    args = parser.parse_args()
    data_ports = [int(port) for port in args.data_ports.split(',')] if args.data_ports else [args.port]
    if not data_ports or len(set(data_ports)) != len(data_ports) or any(not 1 <= port <= 65535 for port in data_ports):
        parser.error('invalid --data-ports')
    if args.mode == 'send':
        send(args.directory, args.port, data_ports)
    else:
        receive(args.directory, args.port, data_ports, args.max_gib * (1 << 30))


if __name__ == '__main__':
    main()
