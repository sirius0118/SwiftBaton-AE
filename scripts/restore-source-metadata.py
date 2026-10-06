#!/usr/bin/env python3
"""Check/restore Git links and executable bits after a flattened source download.

Only paths recorded in source-metadata.json are changed. An existing regular
file is replaced by a link only when its bytes are exactly the recorded target
(with an optional trailing newline). No Git installation is needed by consumers.
"""
import argparse
import json
import os
from pathlib import Path
import stat
import sys


def inside(root, path):
    try:
        path.resolve().relative_to(root)
    except (ValueError, RuntimeError):
        raise ValueError("path escapes the checkout or contains a link loop: " + str(path))


def restore(root, metadata, repair=False):
    root = root.resolve()
    changes = []
    errors = []
    for relative, target in sorted(metadata["symlinks"].items(), key=lambda item: (item[0].count("/"), item[0])):
        path = root / relative
        try:
            if Path(relative).is_absolute() or ".." in Path(relative).parts:
                raise ValueError("invalid manifest path: " + relative)
            inside(root, path.parent)
            inside(root, path.parent / target)
            if path.is_symlink():
                if os.readlink(path) != target:
                    raise ValueError("unexpected symlink target: " + relative)
                continue
            if path.exists() and (not path.is_file() or path.read_bytes() not in
                                  (target.encode(), (target + "\n").encode())):
                raise ValueError("refusing to replace modified file: " + relative)
            changes.append("symlink " + relative)
            if repair:
                path.parent.mkdir(parents=True, exist_ok=True)
                if path.exists():
                    path.unlink()
                path.symlink_to(target)
        except (OSError, ValueError) as error:
            errors.append(str(error))
    for relative in metadata["executables"]:
        path = root / relative
        try:
            if Path(relative).is_absolute() or ".." in Path(relative).parts:
                raise ValueError("invalid manifest path: " + relative)
            inside(root, path.parent)
            if path.is_symlink() or not path.is_file():
                raise ValueError("missing or non-regular executable: " + relative)
            mode = path.stat().st_mode
            if mode & 0o111 != 0o111:
                changes.append("executable " + relative)
                if repair:
                    path.chmod(stat.S_IMODE(mode) | 0o111)
        except (OSError, ValueError) as error:
            errors.append(str(error))
    return changes, errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--manifest", type=Path, default=Path(__file__).with_name("source-metadata.json"))
    parser.add_argument("--repair", action="store_true", help="apply repairs; otherwise check only")
    args = parser.parse_args()
    changes, errors = restore(args.root, json.loads(args.manifest.read_text()), args.repair)
    for error in errors:
        print(error, file=sys.stderr)
    verb = "Restored" if args.repair else "Needs repair:"
    print("{} {} source metadata entries; {} errors".format(verb, len(changes), len(errors)))
    if changes and not args.repair:
        print("Run python3 scripts/restore-source-metadata.py --repair before building.")
    return 1 if errors or (changes and not args.repair) else 0


if __name__ == "__main__":
    sys.exit(main())
