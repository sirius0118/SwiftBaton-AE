#!/usr/bin/env python3
"""Build either supported SwiftBaton-K kernel patch series in an isolated tree."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("source", type=Path, help="pristine Linux 5.15.167 source")
parser.add_argument("--jobs", type=int, default=8)
parser.add_argument("--arm-optimizations", action="store_true",
                    help="include prepared ARM and batched token accounting")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = args.source.resolve()
suffix = "-swiftbaton-k1-arm-batch" if args.arm_optimizations else "-swiftbaton-k1"
destination = root / "build" / ("linux-5.15.167" + suffix)
patches = [root / "kernel/patches/linux-5.15.167-sbk-pte.patch"]
if args.arm_optimizations:
    patches += [
        root / "kernel/patches/linux-5.15.167-sbk-prepared-arm.patch",
        root / "kernel/patches/linux-5.15.167-sbk-arm-batch.patch",
    ]
marker = destination / ".swiftbaton-source.json"
identity = {
    "source": str(source),
    "patch_sha256": [hashlib.sha256(path.read_bytes()).hexdigest() for path in patches],
    "localversion": suffix,
}
if not 1 <= args.jobs <= 80:
    parser.error("jobs must be 1..80")
if not marker.exists():
    if destination.exists():
        raise SystemExit("Build tree exists without a matching marker; inspect before reuse")
    version = subprocess.check_output(
        ["make", "-s", "-C", str(source), "kernelversion"], text=True).strip()
    if version != "5.15.167":
        raise SystemExit("Requires pristine Linux 5.15.167 source")
    with patches[0].open("rb") as stream:
        subprocess.run(["patch", "--dry-run", "-p1", "--batch", "--forward"],
                       cwd=source, stdin=stream, check=True)
    destination.mkdir(parents=True)
    excludes = [
        ".git/", "*.o", "*.ko", "*.a", ".*.cmd", "*.mod", "*.mod.c",
        "Module.symvers", "modules.order", "/vmlinux", "/System.map",
        "/arch/x86/boot/bzImage", "/certs/signing_key*", ".sbk-*",
    ]
    subprocess.run(
        ["rsync", "-a"] + ["--exclude=" + item for item in excludes] +
        [str(source) + "/", str(destination) + "/"], check=True)
    for patch in patches:
        with patch.open("rb") as stream:
            subprocess.run(["patch", "-p1", "--batch", "--forward"],
                           cwd=destination, stdin=stream, check=True)
    config = (root / "kernel/config-5.15.167-swiftbaton-k1").read_text()
    old = 'CONFIG_LOCALVERSION="-swiftbaton-k1"'
    if config.count(old) != 1:
        raise SystemExit("Kernel config localversion has changed")
    (destination / ".config").write_text(
        config.replace(old, 'CONFIG_LOCALVERSION="' + suffix + '"'))
    marker.write_text(json.dumps(identity, indent=2) + "\n")
elif json.loads(marker.read_text()) != identity:
    raise SystemExit("Source or patch set changed; use a fresh build directory")
subprocess.run(["make", "olddefconfig"], cwd=destination, check=True)
release = subprocess.check_output(
    ["make", "-s", "kernelrelease"], cwd=destination, text=True).strip()
if release != "5.15.167" + suffix:
    raise SystemExit("Unexpected kernel release " + release)
subprocess.run(
    ["make", "-j" + str(args.jobs), "bzImage", "modules"],
    cwd=destination, check=True)
print("Built " + release + " at " + str(destination) + "; nothing installed.")
