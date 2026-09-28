#!/usr/bin/env python3
"""Verify rsocket AS lanes transferred and acknowledged real migration pages."""
import json
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
require_snapshot = '--require-snapshot' in sys.argv[2:]
require_images = '--require-images-rsocket' in sys.argv[2:]
allow_empty_pages = '--allow-empty-page-lanes' in sys.argv[2:]
pattern = re.compile(
    r'SB_AS_RSOCKET_SUMMARY side=(source|target) lane=(\d+) '
    r'writes=(\d+) bytes=(\d+) received=(\d+) acknowledged=(\d+) error=(\d+)')
measurements = {}
for name in ('dump.log', 'pageclient.log'):
    matches = pattern.findall((root / name).read_text(errors='replace'))
    if len(matches) != 3:
        raise SystemExit(f'{name}: expected three AS rsocket summaries; found {len(matches)}')
    for side, lane, writes, byte_count, received, acknowledged, error in matches:
        key = (side, int(lane))
        if key in measurements:
            raise SystemExit(f'duplicate AS lane summary: {key}')
        measurements[key] = dict(writes=int(writes), bytes=int(byte_count),
                                 received=int(received), acknowledged=int(acknowledged),
                                 error=int(error))
for lane in range(3):
    source = measurements.get(('source', lane))
    target = measurements.get(('target', lane))
    if not source or not target or any(side['error'] for side in (source, target)) or \
       source['writes'] != source['acknowledged'] or \
       target['writes'] != target['acknowledged'] or \
       source['writes'] != target['received'] or \
       target['writes'] != source['received']:
        raise SystemExit(f'rsocket AS lane {lane} failed bidirectional accounting')
if not allow_empty_pages and not (measurements[('source', 0)]['bytes'] > 4096 and
                                  measurements[('source', 2)]['bytes'] > 4096):
    raise SystemExit('fault and background rsocket lanes must both carry pages')
source_snapshot = re.findall(r'SB_PCLIVE rsocket_bind ip=[^ ]+ port=\d+ bytes=(\d+)',
                             (root / 'dump.log').read_text(errors='replace'))
target_snapshot = re.findall(
    r'SB_PCLIVE rsocket_initial bytes=(\d+) sessions=(\d+) elapsed_ns=(\d+)',
    (root / 'pageclient.log').read_text(errors='replace'))
if require_snapshot and (len(source_snapshot) != 1 or len(target_snapshot) != 1 or
                         int(source_snapshot[0]) != int(target_snapshot[0][0]) or
                         int(target_snapshot[0][1]) < 1 or int(target_snapshot[0][2]) < 1):
    raise SystemExit('rsocket PS snapshot accounting failed')
image_source = re.findall(
    r'SB_IMAGE sent phase=(\d+) files=(\d+) bytes=(\d+) transport=(\w+)',
    (root / 'dump.log').read_text(errors='replace'))
image_target = re.findall(
    r'SB_IMAGE received phase=(\d+) files=(\d+) bytes=(\d+) storage=tmpfs',
    (root / 'pageclient.log').read_text(errors='replace'))
if require_images and (not image_source or len(image_source) != len(image_target) or
                       any(sent[3] != 'rsocket' or sent[:3] != received
                           for sent, received in zip(image_source, image_target))):
    raise SystemExit('rsocket CRIU image phase accounting failed')
result = {'transport': 'rsocket direct-write AS',
          'ps_snapshot': dict(source_bytes=int(source_snapshot[0]),
                              target_bytes=int(target_snapshot[0][0]),
                              sessions=int(target_snapshot[0][1]),
                              elapsed_ns=int(target_snapshot[0][2]))
          if len(source_snapshot) == len(target_snapshot) == 1 else None,
          'image_phases': [dict(phase=int(phase), files=int(files), bytes=int(byte_count),
                                transport=transport)
                           for phase, files, byte_count, transport in image_source],
          'lanes': {str(lane): {'source': measurements[('source', lane)],
                                'target': measurements[('target', lane)]}
                    for lane in range(3)}}
(root / 'rsocket-as-validation.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
