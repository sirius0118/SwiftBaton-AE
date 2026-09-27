#!/usr/bin/env python3
"""Atomically select a verified /usr/bin/criu symlink during an owned trial."""
import argparse
import os
from pathlib import Path
from uuid import uuid4

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('expected')
parser.add_argument('target')
args = parser.parse_args()
path = Path('/usr/bin/criu')
if not path.is_symlink() or os.readlink(path) != args.expected:
    raise SystemExit('CRIU selection changed concurrently; refusing to replace it')
if not Path(args.target).is_file():
    raise SystemExit('target CRIU binary does not exist')
temporary = path.parent / ('.swiftbaton-baseline-criu-' + uuid4().hex)
os.symlink(args.target, temporary)
try:
    os.replace(temporary, path)
finally:
    if temporary.is_symlink():
        temporary.unlink()
