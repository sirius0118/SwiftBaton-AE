#!/usr/bin/env python3
"""Build isolated U/K CRIU binaries for database JVM/file-lock workloads.

Docker's checkpoint RPC currently overrides the per-trial file-locks config.
The VoltDB JVM also exposes a non-readable VMA: RDMA helper RPCs reuse the
parasite argument buffer, so the cleanup callback needs a stable VMA copy.
These binaries include both changes, without modifying the sealed U/K builds.
The reviewed U/K binaries and installed /usr/bin/criu are never overwritten.
"""
import argparse
import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT
OLD = '\tif (req->has_file_locks)\n\t\topts.handle_file_locks = req->file_locks;'
NEW = ('\t/* Dedicated AE database build: the Docker RPC may explicitly clear '
       'the config-file setting. */\n'
       '\topts.handle_file_locks = true;')
OLD_DEMAND = 'if (rc < 0) die("demand address/queue");'
NEW_DEMAND = ('if (rc < 0) {\n'
              '                        pr_err("SB_TRANSFER invalid demand pid=%lu address=%lx rc=%d\\n", '
              '(unsigned long)vpidset[p], (unsigned long)address, rc);\n'
              '                        die("demand address/queue");\n'
              '                    }')
OLD_IOVS = ("\t\tnr_pages += pr->pe->nr_pages;\n\n"
            "\t\tfor (; n_vma < mm->n_vmas; n_vma++) {\n"
            "\t\t\tVmaEntry *vma = mm->vmas[n_vma];\n\n"
            "\t\t\tif (start >= vma->end)\n\t\t\t\tcontinue;\n\n"
            "\t\t\tiov = xzalloc(sizeof(*iov));\n\t\t\tif (!iov)\n"
            "\t\t\t\tgoto free_iovs;\n\n"
            "\t\t\tlen = min_t(uint64_t, end, vma->end) - start;")
NEW_IOVS = ("\t\tfor (; n_vma < mm->n_vmas; n_vma++) {\n"
            "\t\t\tVmaEntry *vma = mm->vmas[n_vma];\n\n"
            "\t\t\tif (start >= vma->end)\n\t\t\t\tcontinue;\n\n"
            "\t\t\tlen = min_t(uint64_t, end, vma->end) - start;\n"
            "\t\t\t/* The source catalog excludes PROT_NONE reservations. */\n"
            "\t\t\tif (!vma->prot) {\n"
            "\t\t\t\tif (end <= vma->end)\n\t\t\t\t\tbreak;\n"
            "\t\t\t\tstart = vma->end;\n\t\t\t\tcontinue;\n\t\t\t}\n"
            "\t\t\tnr_pages += len / page_size();\n"
            "\t\t\tiov = xzalloc(sizeof(*iov));\n\t\t\tif (!iov)\n"
            "\t\t\t\tgoto free_iovs;\n")
OLD_UFFD = ("\t\tif (!vma_entry_can_be_lazy(vma_entry))\n\t\t\tcontinue;\n")
NEW_UFFD = ("\t\tif (!vma_entry_can_be_lazy(vma_entry) || !vma_entry->prot)\n"
            "\t\t\tcontinue;\n")


def build(mode):
    source = SOURCE / ('criu-k' if mode == 'k' else 'criu')
    if not (source / 'criu/cr-service.c').exists():
        raise RuntimeError('Missing reviewed CRIU source: ' + str(source))
    output = ROOT / 'build' / ('criu-' + mode.upper() + '-filelocks')
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run(['rsync', '-a', '--exclude=.git/', str(source) + '/',
                    str(output) + '/'], check=True)
    path = output / 'criu/cr-service.c'
    body = path.read_text()
    if body.count(OLD) != 1:
        raise RuntimeError('Unexpected CRIU RPC file-lock option layout in ' + str(path))
    path.write_text(body.replace(OLD, NEW))
    subprocess.run(['patch', '--batch', '--forward', '-p1', '-i',
                    str(ROOT / 'experiments/patches/parasite-stable-vmas.patch')],
                   cwd=output, check=True)
    transfer = output / 'criu/sb-transfer.c'
    transfer_body = transfer.read_text()
    if transfer_body.count(OLD_DEMAND) != 1:
        raise RuntimeError('Unexpected demand error path in ' + str(transfer))
    transfer.write_text(transfer_body.replace(OLD_DEMAND, NEW_DEMAND))
    uffd = output / 'criu/uffd.c'
    uffd_body = uffd.read_text()
    if uffd_body.count(OLD_IOVS) != 1:
        raise RuntimeError('Unexpected lazy IOV collector in ' + str(uffd))
    uffd.write_text(uffd_body.replace(OLD_IOVS, NEW_IOVS))
    restorer = output / 'criu/pie/restorer.c'
    restorer_body = restorer.read_text()
    if restorer_body.count(OLD_UFFD) != 1:
        raise RuntimeError('Unexpected userfaultfd registration in ' + str(restorer))
    restorer.write_text(restorer_body.replace(OLD_UFFD, NEW_UFFD))
    jobs = os.environ.get('SB_BUILD_JOBS', '12')
    subprocess.run(['make', '-C', str(output), '-j' + jobs, 'criu'], check=True)
    binary = output / 'criu/criu'
    subprocess.run(['ssh', '-oBatchMode=yes', 'knode3',
                    'mkdir -p ' + str(binary.parent)], check=True)
    subprocess.run(['rsync', '-az', str(binary), 'knode3:' + str(binary)], check=True)
    subprocess.run(['ssh', '-oBatchMode=yes', 'knode3',
                    'sha256sum ' + str(binary)], check=True)
    subprocess.run(['sha256sum', str(binary)], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['u', 'k'])
    build(parser.parse_args().mode)
