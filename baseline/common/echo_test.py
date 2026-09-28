#!/usr/bin/env python3
"""Temporary exact-byte transport probe for rsocket_relay."""
import hashlib
import os
import socket
import sys

mode = sys.argv[1]
if mode == 'server':
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(('127.0.0.1', int(sys.argv[2])))
    listener.listen(1)
    with listener.accept()[0] as peer:
        total = 0
        while True:
            block = peer.recv(65536)
            if not block:
                break
            peer.sendall(block)
            total += len(block)
        print('server_bytes', total, flush=True)
else:
    content = hashlib.sha256(b'swiftbaton-rsocket-baseline').digest() * 32768
    with socket.create_connection(('127.0.0.1', int(sys.argv[2])), 10) as peer:
        peer.settimeout(10)
        peer.sendall(content)
        peer.shutdown(socket.SHUT_WR)
        received = bytearray()
        while True:
            block = peer.recv(65536)
            if not block:
                break
            received.extend(block)
    if bytes(received) != content:
        raise SystemExit('byte mismatch: %d / %d' % (len(received), len(content)))
    print('PASS bytes=%d sha256=%s' % (len(content), hashlib.sha256(content).hexdigest()))
