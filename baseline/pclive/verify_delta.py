#!/usr/bin/env python3
"""Cross-check source, RDMA, and resident PCLive second-round accounting."""
import json
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])


def one(name, pattern):
    text = (root / name).read_text(errors='replace')
    matches = re.findall(pattern, text)
    if len(matches) != 1:
        raise SystemExit(f'{name}: expected one PCLive delta summary, found {len(matches)}')
    return list(map(int, matches[0]))


pages, copied, changed, source_payload, source_workers = one(
    'dump.log',
    r'SB_PCLIVE refresh_round pages=(\d+) copied=(\d+) changed=(\d+) '
    r'rdma_payload_bytes=(\d+) workers=(\d+)')
metadata, rdma_payload, rdma_changed, runs, completions, total = one(
    'pageclient.log',
    r'SB_PCLIVE rdma_delta metadata_bytes=(\d+) payload_bytes=(\d+) '
    r'changed_pages=(\d+) runs=(\d+) completions=(\d+) total_bytes=(\d+)')
resident_pages, resident_bytes, resident_workers = one(
    'restore.log', r'SB_PCLIVE resident_refresh pages=(\d+) bytes=(\d+) workers=(\d+)')
if not (0 <= resident_pages <= changed <= copied <= pages and
        source_payload == rdma_payload == changed * 4096 and
        rdma_changed == changed and resident_bytes == resident_pages * 4096 and
        total == metadata + rdma_payload and metadata > 0 and
        source_workers > 0 and resident_workers > 0 and
        (changed == 0 or runs > 0) and completions > 0):
    raise SystemExit('PCLive source/RDMA/resident delta accounting mismatch')
full_bytes = metadata + pages * 4096
result = {
    'candidates': pages,
    'source_copied': copied,
    'changed_pages': changed,
    'rdma_metadata_bytes': metadata,
    'rdma_payload_bytes': rdma_payload,
    'rdma_total_bytes': total,
    'full_second_snapshot_bytes': full_bytes,
    'rdma_bytes_avoided': full_bytes - total,
    'resident_refreshed_pages': resident_pages,
    'changed_runs': runs,
    'rdma_completions': completions,
}
(root / 'pclive-delta-validation.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
