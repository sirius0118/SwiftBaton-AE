#!/usr/bin/env python3
"""Verify that a remote-fork run served every demand page through rsocket."""
import json
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
state = json.loads((root / 'state.json').read_text())
dump = (root / 'dump.log').read_text(errors='replace')
target = (root / 'pageclient.log').read_text(errors='replace')

def one(pattern, data, label):
    rows = re.findall(pattern, data)
    if len(rows) != 1:
        raise SystemExit(f'{label}: expected one summary, found {len(rows)}')
    return tuple(map(int, rows[0]))

src_regions, src_workers, src_port = one(
    r'SBK_RSOCKET_SOURCE_READY regions=(\d+) workers=(\d+) port=(\d+)', dump,
    'source ready')
dst_regions, dst_workers, dst_port = one(
    r'SBK_RSOCKET_TARGET_READY regions=(\d+) workers=(\d+) port=(\d+)', target,
    'target ready')
src_connections, reads, src_bytes, src_errors = one(
    r'SBK_RSOCKET_SOURCE_SUMMARY connections=(\d+) reads=(\d+) bytes=(\d+) errors=(\d+)',
    dump, 'source')
dst_connections, faults, dst_bytes, dst_errors = one(
    r'SBK_RSOCKET_TARGET_SUMMARY connections=(\d+) faults=(\d+) bytes=(\d+) errors=(\d+)',
    target, 'target')
stats = state['kernel_stats']
image_source = re.findall(
    r'SB_IMAGE sent phase=(\d+) files=(\d+) bytes=(\d+) transport=(\w+)', dump)
image_target = re.findall(
    r'SB_IMAGE received phase=(\d+) files=(\d+) bytes=(\d+) storage=tmpfs', target)
if not image_source or len(image_source) != len(image_target) or any(
        sent[3] != 'rsocket' or sent[:3] != received
        for sent, received in zip(image_source, image_target)):
    raise SystemExit('CRIU image payloads did not match over rsocket')
if not state.get('success') or state['validation']['checked_records'] != state['parameters']['records']:
    raise SystemExit('run did not complete full Redis validation')
if not (src_regions == dst_regions == stats['regions'] and
        src_workers == dst_workers == src_connections == dst_connections and
        src_port == dst_port and reads == faults == stats['PF'] and
        src_bytes == dst_bytes == reads * 4096 and reads > 0 and
        src_errors == dst_errors == stats['errors'] == 0):
    raise SystemExit('rsocket source, target and kernel page accounting differ')
if any(stats[key] for key in ('PS', 'FT', 'BG', 'invalid')):
    raise SystemExit('remote-fork unexpectedly transferred non-demand pages')
if stats['PF'] + stats['retired_unfetched'] != stats['pages']:
    raise SystemExit('unfetched marker accounting is incomplete')

result = dict(transport='rsocket images and proxy demand pages', regions=src_regions,
              connections=src_connections, pages=reads, bytes=src_bytes,
              retired_unfetched=stats['retired_unfetched'], errors=0,
              image_phases=[dict(phase=int(phase), files=int(files), bytes=int(byte_count))
                            for phase, files, byte_count, _ in image_source])
(root / 'rsocket-remote-fork-validation.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
