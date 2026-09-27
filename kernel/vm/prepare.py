#!/usr/bin/env python3
"""Copy exact in-tree VM modules and runtime dependencies, never host OFED modules."""
from pathlib import Path
import os
import re
import shutil
import subprocess

root = Path(os.environ.get("SBK_VM_ROOT", str(Path(__file__).resolve().parent / "root")))
kernel = Path(os.environ.get("KDIR", "/home/k8s/exper/zxz/linux-5.15.167"))
modules = {p.stem.replace("-", "_"): p for p in kernel.rglob("*.ko")}
ofed = os.environ.get("SBK_OFED_ROOT")
if ofed:
    modules.update({p.stem.replace("-", "_"): p for p in Path(ofed).rglob("*.ko")})
seen, order = set(), []
def module(name):
    name = name.replace("-", "_")
    if name in seen:
        return
    seen.add(name)
    p = modules[name]
    deps = subprocess.check_output(["modinfo", "-F", "depends", str(p)], text=True).strip()
    for dep in filter(None, deps.split(",")):
        module(dep)
    dest = root / "modules" / p.name
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(p, dest)
    order.append("insmod /modules/" + p.name + " || exit 1")
for name in (("ib_core",) if ofed else ("crc32_generic", "dummy", "e1000", "rdma_rxe")):
    module(name)
for name in filter(None, os.environ.get("SBK_VM_EXTRA_MODULES", "").split(",")):
    module(name)
(root / "load-modules.sh").write_text("#!/bin/sh\n" + "\n".join(order) + "\n")
for binary in ("/usr/bin/rdma", "/usr/sbin/ip"):
    deps = subprocess.check_output(["ldd", binary], text=True)
    for src in [binary] + re.findall(r"(/[^\s]+)", deps):
        target = root / src.lstrip("/")
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, target)
print("VM module load order:", ", ".join(seen))
