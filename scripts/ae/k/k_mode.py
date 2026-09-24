"""K settings, read-only preflight and completion checks shared by AE and VM tests."""
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct

CAPABILITIES = 0x80104211  # _IOR('B', 17, struct sbk_capabilities), Linux x86_64
ABI_VERSION = 1
ANONYMOUS_PTE = 1
PARALLEL_PS = 2
PS_SLICE = 4
PARALLEL_EXPORT = 8
SESSION_DISPATCH = 16


@dataclass(frozen=True)
class KernelSettings:
    device: str = 'mlx5_1'
    gid: int = 3
    timeout_ms: int = 2000
    fault_workers: int = 2
    prefetch_workers: int = 1
    install_workers: int = 4
    precopy_workers: int = 4
    no_pretransfer: bool = False
    no_prefetch: bool = False
    no_hot_first: bool = False
    dense: bool = False
    ps_chunk_mb: int = 64
    export_workers: int = 1
    export_chunk_mb: int = 0

    def values(self):
        if not re.fullmatch(r'[A-Za-z0-9_.-]{1,31}', self.device):
            raise ValueError('Invalid RDMA device name')
        if not 0 <= self.gid <= 255 or not 10 <= self.timeout_ms <= 30000:
            raise ValueError('K requires GID 0..255 and timeout 10..30000 ms')
        if not all(1 <= n <= 8 for n in (self.fault_workers, self.prefetch_workers, self.install_workers)):
            raise ValueError('K lane worker counts must be 1..8')
        if not 1 <= self.precopy_workers <= 32:
            raise ValueError('PS worker count must be 1..32')
        if not 0 <= self.ps_chunk_mb <= 4096:
            raise ValueError('PS chunk span must be 0..4096 MiB; zero retains legacy spans')
        if not 1 <= self.export_workers <= 32 or not 0 <= self.export_chunk_mb <= 4096:
            raise ValueError('Final MR workers must be 1..32 and span 0..4096 MiB')
        values = {'image-rdma': True, 'u-precopy': True, 'kernel-transfer': True,
                  'kernel-device': self.device, 'kernel-gid': self.gid,
                  'kernel-timeout-ms': self.timeout_ms, 'fault-workers': self.fault_workers,
                  'prefetch-workers': self.prefetch_workers, 'install-workers': self.install_workers,
                  'precopy-workers': self.precopy_workers,
                  'kernel-ps-chunk-mb': self.ps_chunk_mb,
                  'kernel-export-workers': self.export_workers,
                  'kernel-export-chunk-mb': self.export_chunk_mb}
        for option in ('no_pretransfer', 'no_prefetch', 'no_hot_first', 'dense'):
            if getattr(self, option):
                values['kernel-dense' if option == 'dense' else option.replace('_', '-')] = True
        return values

    def config(self):
        return ''.join(key + '=' + ('yes' if value is True else str(value)) + '\n'
                       for key, value in self.values().items())

    def argv(self):
        args = []
        for key, value in self.values().items():
            args.append('--' + key)
            if value is not True:
                args.append(str(value))
        return args


def host_probe(binary, device, gid):
    """Read only: no module load, kernel installation, NIC change or container creation."""
    import fcntl
    result = {'kernel': os.uname().release, 'errors': [], 'binary': binary}
    def read(name, operation):
        try:
            result[name] = operation()
        except (OSError, ValueError) as error:
            result['errors'].append(name + ': ' + str(error))
    def digest(path):
        return hashlib.sha256(Path(path).read_bytes()).hexdigest()
    read('binary_sha256', lambda: digest(binary))
    read('installed_sha256', lambda: digest('/usr/bin/criu'))
    read('pageclient_path', lambda: shutil.which('criu') or '')
    if result.get('pageclient_path'):
        read('pageclient_sha256', lambda: digest(result['pageclient_path']))
    else:
        result['errors'].append('No criu in privileged PATH')
    read('boot_id', lambda: Path('/proc/sys/kernel/random/boot_id').read_text().strip())
    port = Path('/sys/class/infiniband') / device / 'ports/1'
    read('rdma_state', lambda: (port/'state').read_text().strip())
    read('rdma_gid', lambda: (port/'gids'/str(gid)).read_text().strip())
    read('early_prefetch', lambda: Path('/sys/module/swiftbaton_k/parameters/early_prefetch').read_text().strip())
    if Path('/sys/module/swiftbaton_k/parameters/session_dispatch').exists():
        read('session_dispatch', lambda: Path('/sys/module/swiftbaton_k/parameters/session_dispatch').read_text().strip())
    read('module_build_id_note', lambda: Path('/sys/module/swiftbaton_k/notes/.note.gnu.build-id').read_bytes().hex())
    def caps():
        fd = os.open('/dev/swiftbaton_k', os.O_RDWR | os.O_CLOEXEC)
        try:
            data = bytearray(16)
            fcntl.ioctl(fd, CAPABILITIES, data, True)
            return dict(zip(('version', 'features', 'max_regions', 'max_pages'), struct.unpack('=4I', data)))
        finally:
            os.close(fd)
    read('capabilities', caps)
    # runc launched by the daemon may resolve a different PATH from sudo.
    daemons = []
    for p in Path('/proc').iterdir():
        if not p.name.isdigit():
            continue
        try:
            name = (p/'comm').read_text().strip()
        except FileNotFoundError:
            continue  # An unrelated process can exit while /proc is enumerated.
        if name not in ('dockerd', 'containerd'):
            continue
        try:
            env = dict(row.split(b'=', 1) for row in (p/'environ').read_bytes().split(b'\0') if b'=' in row)
            resolved = shutil.which('criu', path=env.get(b'PATH', b'').decode())
            daemons.append({'pid': int(p.name), 'name': name, 'criu_path': resolved,
                            'sha256': digest(resolved) if resolved else None})
        except (OSError, ValueError) as error:
            result['errors'].append('daemon ' + p.name + ': ' + str(error))
    result['daemons'] = daemons
    return result


def validate_preflight(hosts, export_workers=1):
    errors = []
    reference = hosts.get('knode2', {}).get('binary_sha256')
    for host in ('knode2', 'knode3'):
        row = hosts.get(host, {})
        errors.extend(host + ': ' + msg for msg in row.get('errors', []))
        for key in ('binary_sha256', 'installed_sha256', 'pageclient_sha256'):
            if not reference or row.get(key) != reference:
                errors.append(host + ': mismatched ' + key)
        daemons = row.get('daemons', [])
        if not {'dockerd', 'containerd'} <= {d['name'] for d in daemons}:
            errors.append(host + ': missing Docker/containerd PATH evidence')
        if any(d.get('sha256') != reference for d in daemons):
            errors.append(host + ': daemon resolves another CRIU')
        caps = row.get('capabilities', {})
        features = caps.get('features', 0)
        if caps.get('version') != ABI_VERSION or (features & (PARALLEL_PS | PS_SLICE)) != (PARALLEL_PS | PS_SLICE):
            errors.append(host + ': incompatible K ABI/PS capabilities')
        if host == 'knode2' and export_workers > 1 and not features & PARALLEL_EXPORT:
            errors.append(host + ': parallel final MR export unavailable')
        if host == 'knode3' and not features & ANONYMOUS_PTE:
            errors.append(host + ': anonymous PTE bridge unavailable')
        if row.get('rdma_state') != '4: ACTIVE':
            errors.append(host + ': RDMA port is not ACTIVE')
        gid = row.get('rdma_gid', '')
        if not gid or not gid.replace(':', '').strip('0'):
            errors.append(host + ': GID is missing or zero')
    return errors


def validate_container(info, expected_id, running):
    if info.get('Id') != expected_id or info.get('Config', {}).get('Labels', {}).get('swiftbaton.ae') != 'true':
        raise ValueError('Container identity/ownership mismatch')
    if info.get('HostConfig', {}).get('RestartPolicy', {}).get('Name') != 'no' or info.get('RestartCount') != 0:
        raise ValueError('K requires restart=no and no previous automatic restart')
    state = info.get('State', {})
    if state.get('Running') is not running or bool(state.get('Restarting')):
        raise ValueError('Unexpected container running/restarting state')
    if state.get('Dead') or state.get('OOMKilled') or state.get('Paused'):
        raise ValueError('Container is dead, OOM-killed or paused')
    if not running and state.get('Pid') != 0:
        raise ValueError('Source runtime has not retired its PID')
    if running and (not isinstance(state.get('Pid'), int) or state['Pid'] <= 0):
        raise ValueError('Running source has no valid PID')


def validate_ps_config(dump_log, chunk_mb, disabled=False):
    """Verify the executing Docker service, not just the requested config file."""
    settings = re.findall(r'SB_KERNEL PS settings chunk_mb=(\d+) workers=(\d+) budget_pages=(\d+)', dump_log)
    snapshots = re.findall(r'SB_KERNEL PS snapshot [^\r\n]* pages=(\d+)', dump_log)
    if disabled:
        if settings or snapshots:
            raise ValueError('PS disabled but source prepared snapshots')
        return {'disabled': True}
    if len(settings) != 1 or int(settings[0][0]) != chunk_mb:
        raise ValueError('Executing source PS chunk size differs from requested configuration')
    planned = re.findall(r'SB_KERNEL PS transferred regions=\d+ planned=(\d+)', dump_log)
    if len(planned) != 1 or int(planned[0]) != len(snapshots):
        raise ValueError('Missing per-region source PS span evidence')
    limit = (chunk_mb or 4096) * 256
    if any(int(pages) > limit for pages in snapshots):
        raise ValueError('Source PS snapshot exceeds configured chunk span')
    return {'chunk_mb': chunk_mb, 'snapshot_regions': len(snapshots),
            'maximum_snapshot_pages': max(map(int, snapshots), default=0)}


def completion_stats(pageclient_log):
    lines = re.findall(r'SB_KERNEL complete ([^\n]+)', pageclient_log)
    if len(lines) != 1:
        raise ValueError('Require exactly one K completion record')
    values = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', lines[0])}
    needed = ('regions', 'pages', 'PF', 'FT', 'BG', 'PS', 'invalid', 'errors', 'faults', 'hits', 'fault_ns', 'fault_max_ns')
    if any(k not in values for k in needed):
        raise ValueError('Incomplete K page/fault statistics')
    if values['errors'] or values['invalid'] > values['PS'] or values['pages'] <= 0 or values['regions'] <= 0:
        raise ValueError('K migration failed or has invalid accounting')
    if values['PS'] - values['invalid'] + sum(values[k] for k in ('PF', 'FT', 'BG')) != values['pages']:
        raise ValueError('K page ownership accounting mismatch')
    values['valid_PS'] = values['PS'] - values['invalid']
    return values


def validate_completion(statuses, source_info, source_id, dump_log, pageclient_log):
    if statuses != {'checkpoint': 0, 'restore': 0, 'pageclient': 0}:
        raise ValueError('All three migration commands must terminate successfully')
    validate_container(source_info, source_id, False)
    armed = re.findall(r'SB_KERNEL source exitkill armed pid=(\d+) threads=\d+ controller=\d+', dump_log)
    released = re.findall(r'SB_KERNEL source owner_released pid=(\d+) threads=\d+ state=(\d+)', dump_log)
    cured = list(re.finditer(r'SB_KERNEL source owner_cured pid=(\d+)', dump_log))
    if not armed or len(set(armed)) != len(armed) or len(released) != len(armed):
        raise ValueError('Missing/duplicate source ptrace lifecycle evidence')
    if {pid for pid, _ in released} != set(armed) or any(state != '2' for _, state in released):
        raise ValueError('Source ptrace owners did not terminate every owned process')
    first_release = dump_log.index('SB_KERNEL source owner_released ')
    if len(cured) != len(armed) or {m[1] for m in cured} != set(armed) or any(m.start() > first_release for m in cured):
        raise ValueError('All parasites must be cured before any source owner releases')
    return completion_stats(pageclient_log)


def validate_export_config(dump_log, workers, chunk_mb):
    rows = re.findall(r'SB_KERNEL final_export pid=(\d+) workers=(\d+) chunk_mb=(\d+) effective_pages=(\d+) peak=(\d+) regions=(\d+) result=(-?\d+)', dump_log)
    if not rows:
        raise ValueError('Missing final export runtime evidence')
    result = []
    for raw in rows:
        pid, actual_workers, actual_chunk, span, peak, count, status = map(int, raw)
        if status or actual_workers != workers or actual_chunk != chunk_mb or not 1 <= peak <= min(workers, count):
            raise ValueError('Failed or mismatched final export configuration')
        if not (chunk_mb or 4096)*256 <= span <= 1 << 20:
            raise ValueError('Invalid adaptive final export span')
        result.append(dict(pid=pid,workers=actual_workers,chunk_mb=actual_chunk,effective_pages=span,peak=peak,regions=count))
    return result


def validate_dispatch(pageclient_log, enabled, prefetch_workers, background_workers):
    rows = re.findall(r'SB_KERNEL dispatch ([^\n]+)', pageclient_log)
    if not enabled:
        if rows != ['disabled']:
            raise ValueError('Legacy session must explicitly report dispatcher disabled')
        return {'enabled': False}
    lanes = {}
    required = {'lane','workers','peak','active','submitted','quanta','completed','queued','queue_peak'}
    for row in rows:
        d = {k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',row)}
        if set(d) != required or d['lane'] in lanes or d['lane'] not in (1,2):
            raise ValueError('Incomplete/duplicate session dispatcher evidence')
        expected = prefetch_workers if d['lane']==1 else background_workers
        if d['workers'] != expected or not 0 <= d['peak'] <= expected or d['active'] or d['queued'] or d['submitted'] != d['completed'] or d['quanta'] < d['completed']:
            raise ValueError('Session worker budget/drain mismatch')
        lanes[d['lane']] = d
    if set(lanes) != {1,2}:
        raise ValueError('Missing FT/BG session dispatcher evidence')
    return {'enabled': True, 'lanes': lanes}
