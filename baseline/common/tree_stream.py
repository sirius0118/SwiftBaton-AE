#!/usr/bin/env python3
"""Stream a CRIU image tree through a local rsocket_relay TCP endpoint.

Every inter-host byte travels over RDMA. Entries are copied incrementally, so
large Redis image sets do not require a second in-memory copy of the tree.
"""
import argparse
import hashlib
import json
import os
import pathlib
import socket
import struct
import time

MAGIC = b'SB-TREE-1\n'
CHUNK = 1024 * 1024


def entries(source):
    found = []
    for path in sorted(source.rglob('*')):
        if path.is_symlink() or not (path.is_file() or path.is_dir()):
            raise ValueError('unsupported image entry: ' + str(path))
        found.append(path)
    return found


def send(source, port):
    found = entries(source)
    digest = hashlib.sha256()
    transferred = 0
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
                    while True:
                        block = image.read(CHUNK)
                        if not block:
                            break
                        conn.sendall(block)
                        digest.update(block)
                        transferred += len(block)
        conn.sendall(digest.digest())
        conn.shutdown(socket.SHUT_WR)
        if recv_exact(conn, 2) != b'OK':
            raise RuntimeError('receiver rejected image tree')
    print('files=%d payload_bytes=%d sha256=%s elapsed_ms=%.3f' %
          (len(found), transferred, digest.hexdigest(), (time.monotonic() - start) * 1000), flush=True)


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


def receive(destination, port, max_bytes):
    if destination.exists():
        raise FileExistsError(destination)
    staging = destination.with_name(destination.name + '.receiving')
    if staging.exists():
        raise FileExistsError(staging)
    digest = hashlib.sha256()
    transferred = 0
    start = time.monotonic()
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(('127.0.0.1', port))
        listener.listen(1)
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
                            block = recv_exact(conn, min(CHUNK, remaining))
                            image.write(block)
                            digest.update(block)
                            transferred += len(block)
                            remaining -= len(block)
                os.chmod(path, info['mode'] & 0o777)
            expected = recv_exact(conn, 32)
            if digest.digest() != expected:
                raise ValueError('image tree digest mismatch')
            staging.rename(destination)
            conn.sendall(b'OK')
    print('files=%d payload_bytes=%d sha256=%s elapsed_ms=%.3f' %
          (count, transferred, digest.hexdigest(), (time.monotonic() - start) * 1000), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('send', 'receive'))
    parser.add_argument('directory', type=pathlib.Path)
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--max-gib', type=int, default=64)
    args = parser.parse_args()
    if args.mode == 'send':
        send(args.directory, args.port)
    else:
        receive(args.directory, args.port, args.max_gib * (1 << 30))


if __name__ == '__main__':
    main()
