#!/usr/bin/env python3
"""One opt-in source PS MR trial; always restore the Node2 source module."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shlex
import subprocess
import tempfile
import time

P = Path(__file__).resolve().parents[3]
R = Path('/home/k8s/SwiftBaton-AE-downtime-20260927')
W = Path(str(R) + '-work')
OLD = Path('/home/k8s/exper/zxz/live_migration/SwiftBaton-K/build/module-ofed-optimized/swiftbaton_k.ko')
NEW = W / 'prearm-source-ofed/module/swiftbaton_k.ko'
OLD_SHA = 'a45f6f60bd56d55f3b0ce481ede1ba3ef61c0974df223e2ce800628618711e8a'
TARGET_OLD = '/var/tmp/swiftbaton-k-check/swiftbaton_k.ko'
TARGET_NEW = '/var/tmp/swiftbaton-arm-plan-20260928/candidate.ko'
TARGET_OLD_SHA = '4fcd86e6413ef7a239d4f121e3617f137d5182a617046ffa7686ecb2b7a85231'
TARGET_NEW_SHA = '89546673304a80df4a87ddddba9f0c0c9573b0a493f82bd9c750530a8fa17921'
CRIU_OLD = '/home/k8s/exper/zxz/live_migration/SwiftBaton-K/criu-integration/criu/criu'
parser = argparse.ArgumentParser()
parser.add_argument('profile', choices=['smoke', 'redis'])
parser.add_argument('--precopy-limit-mb', type=int, choices=range(1, 65537))
parser.add_argument('--all-ps-ranges', action='store_true')
args = parser.parse_args()
budget_label = '-ps%d' % args.precopy_limit_mb if args.precopy_limit_mb else ''
if args.all_ps_ranges: budget_label += '-all'
state_file = W / ('prearm-trial-' + args.profile + budget_label + '-' +
                  time.strftime('%Y%m%d_%H%M%S') + '.json')
log = state_file.with_suffix('.log')
state = {'profile': args.profile, 'precopy_limit_mb': args.precopy_limit_mb,
         'all_ps_ranges': args.all_ps_ranges,
         'source_module': str(NEW), 'old_module': str(OLD),
         'log': str(log), 'switch_attempted': False, 'success': False}


def save():
    state_file.write_text(json.dumps(state, indent=2) + '\n')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def loaded_note():
    return Path('/sys/module/swiftbaton_k/notes/.note.gnu.build-id').read_bytes()


def file_note(path):
    with tempfile.TemporaryDirectory() as temp:
        p = Path(temp)
        subprocess.run(['objcopy', '--dump-section', '.note.gnu.build-id=' + str(p / 'note'),
                        str(path), str(p / 'copy.ko')], check=True)
        return (p / 'note').read_bytes()


def verify_idle():
    assert not subprocess.check_output(['sudo', '-n', 'docker', 'ps', '-aq',
        '--filter', 'label=swiftbaton.ae=true'], text=True).strip()
    assert subprocess.run(['pgrep', '-x', 'criu'], stdout=subprocess.DEVNULL).returncode == 1
    assert Path('/sys/module/swiftbaton_k/refcnt').read_text().strip() == '0'


def switch(path, params):
    verify_idle()
    subprocess.run(['sudo', '-n', 'rmmod', 'swiftbaton_k'], check=True)
    subprocess.run(['sudo', '-n', 'insmod', str(path)] +
                   [key + '=' + value for key, value in params.items()], check=True)
    assert loaded_note() == file_note(path)
    verify_idle()


REMOTE = r'''from pathlib import Path
import hashlib,json,subprocess,sys,tempfile,os
mode,path,expected,params=sys.argv[1:]
params=json.loads(params)
assert os.uname().release=='5.15.167-swiftbaton-k1'
assert hashlib.sha256(Path(path).read_bytes()).hexdigest()==expected
def note(path):
 with tempfile.TemporaryDirectory() as temp:
  out=Path(temp)
  subprocess.run(['objcopy','--dump-section','.note.gnu.build-id='+str(out/'note'),path,str(out/'copy.ko')],check=True)
  return (out/'note').read_bytes()
def idle():
 if Path('/sys/module/swiftbaton_k').exists():
  assert Path('/sys/module/swiftbaton_k/refcnt').read_text().strip()=='0'
 assert not subprocess.check_output(['docker','ps','-aq','--filter','label=swiftbaton.ae=true'],text=True).strip()
 assert subprocess.run(['pgrep','-x','criu'],stdout=subprocess.DEVNULL).returncode==1
if mode=='probe':
 idle()
 assert Path('/sys/module/swiftbaton_k/notes/.note.gnu.build-id').read_bytes()==note(path)
 print(json.dumps({'boot_id':Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
                   'params':{p.name:p.read_text().strip() for p in Path('/sys/module/swiftbaton_k/parameters').iterdir()}}))
elif mode=='switch':
 idle()
 if Path('/sys/module/swiftbaton_k').exists():
  subprocess.run(['rmmod','swiftbaton_k'],check=True)
 subprocess.run(['insmod',path]+[k+'='+v for k,v in params.items()],check=True)
 idle()
 assert Path('/sys/module/swiftbaton_k/notes/.note.gnu.build-id').read_bytes()==note(path)
 print(json.dumps({'loaded':path,'boot_id':Path('/proc/sys/kernel/random/boot_id').read_text().strip()}))
else: raise ValueError(mode)
'''


def target(mode, path, digest, params=None):
    command = ['sudo', '-n', 'python3', '-c', REMOTE, mode, path, digest,
               json.dumps(params or {})]
    return json.loads(subprocess.check_output(['ssh', '-oBatchMode=yes', 'knode3',
                                               shlex.join(command)], text=True, timeout=45))


def restore_criu():
    code = '''from pathlib import Path
import os,subprocess,sys,uuid
candidate,original=sys.argv[1:]
p=Path('/usr/bin/criu')
assert p.is_symlink() and Path(original).is_file()
current=os.readlink(p)
assert current in (candidate,original)
if current==original: sys.exit(0)
assert subprocess.run(['pgrep','-x','criu'],stdout=subprocess.DEVNULL).returncode==1
assert not subprocess.check_output(['docker','ps','-aq','--filter','label=swiftbaton.ae=true'],text=True).strip()
tmp=p.parent/('.swiftbaton-criu-recover-'+uuid.uuid4().hex)
os.symlink(original,tmp)
os.replace(tmp,p)
assert os.readlink(p)==original
'''
    candidate = str(P / 'build/criu-K/criu/criu')
    for host in ('knode3', 'knode2'):
        command = ['sudo', '-n', 'python3', '-c', code, candidate, CRIU_OLD]
        if host != 'knode2':
            command = ['ssh', '-oBatchMode=yes', host, shlex.join(command)]
        subprocess.run(command, check=True, timeout=45)


assert os.uname().release == '5.15.167'
assert P != R and NEW.is_file() and OLD.is_file() and sha(OLD) == OLD_SHA
new_sha = sha(NEW)
assert new_sha == '259275dcdb92713c390565f118079759326e5439426a617a3d13e896bb6832b0'
verify_idle()
assert loaded_note() == file_note(OLD)
target_before = target('probe', TARGET_OLD, TARGET_OLD_SHA)
assert os.readlink('/usr/bin/criu') == CRIU_OLD
assert subprocess.check_output(['ssh', '-oBatchMode=yes', 'knode3',
                                'readlink /usr/bin/criu'], text=True).strip() == CRIU_OLD
old_params = {p.name: p.read_text().strip() for p in Path('/sys/module/swiftbaton_k/parameters').iterdir()}
boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
state.update(new_sha256=new_sha, old_sha256=OLD_SHA, boot_id=boot,
             old_params=old_params, old_note_sha256=hashlib.sha256(loaded_note()).hexdigest(),
             target_before=target_before, target_switch_attempted=False)
save()
try:
    state['target_switch_attempted'] = True
    save()
    state['target_candidate'] = target('switch', TARGET_NEW, TARGET_NEW_SHA,
                                       target_before['params'])
    save()
    state['switch_attempted'] = True
    save()
    switch(NEW, old_params)
    state['candidate_loaded'] = True
    save()
    env = dict(os.environ, SB_AE_WORK_ROOT=str(W), TZ='Asia/Shanghai')
    common = ['python3', str(P / 'scripts/run.py'), 'K', '--profile', args.profile,
              '--network-lock', 'nftables', '--vma-cache', '--buffered-cutover',
              '--validation-workers', '16', '--kernel-catalog-workers', '16',
              '--kernel-export-workers', '16', '--kernel-export-chunk-mb', '64',
              '--kernel-ps-arm', '--kernel-ps-mr']
    if args.precopy_limit_mb:
        common += ['--precopy-limit-mb', str(args.precopy_limit_mb)]
    if args.all_ps_ranges:
        common += ['--kernel-ps-mr-all']
    with log.open('w') as output:
        check = subprocess.run(common + ['--check'], env=env, stdout=output,
                               stderr=subprocess.STDOUT)
        state['preflight_rc'] = check.returncode
        save()
        if check.returncode:
            raise RuntimeError('Preflight failed: ' + str(log))
        run = subprocess.run(common + ['--execute'], env=env, stdout=output,
                             stderr=subprocess.STDOUT)
        state['run_rc'] = run.returncode
        save()
        if run.returncode:
            raise RuntimeError('Migration failed: ' + str(log))
    state['success'] = True
finally:
    errors = []
    if state.get('run_rc') and log.is_file():
        paths = [line[6:].strip() for line in log.read_text(errors='replace').splitlines()
                 if line.startswith('STATE=')]
        if paths:
            try:
                cleanup = subprocess.run(['python3', str(P / 'scripts/ae/k/cleanup_ae.py'),
                                          paths[-1]], capture_output=True, text=True)
                state['cleanup_retry_rc'] = cleanup.returncode
                state['cleanup_retry_log'] = cleanup.stdout + cleanup.stderr
                if cleanup.returncode:
                    errors.append('cleanup retry failed')
            except Exception as error:
                errors.append('cleanup retry: ' + str(error))
    try:
        restore_criu()
        state['criu_restored'] = True
    except Exception as error:
        errors.append('CRIU: ' + str(error))
        state['criu_restore_error'] = str(error)
    save()
    if state['switch_attempted']:
        try:
            if Path('/sys/module/swiftbaton_k').exists():
                verify_idle()
                subprocess.run(['sudo', '-n', 'rmmod', 'swiftbaton_k'], check=True)
            subprocess.run(['sudo', '-n', 'insmod', str(OLD)] +
                           [key + '=' + value for key, value in old_params.items()], check=True)
            state['restored'] = (loaded_note() == file_note(OLD) and
                                 Path('/proc/sys/kernel/random/boot_id').read_text().strip() == boot)
            verify_idle()
        except Exception as error:
            state['restore_error'] = str(error)
            state['success'] = False
            errors.append('source: ' + str(error))
        save()
    if state['target_switch_attempted']:
        try:
            state['target_restored'] = target('switch', TARGET_OLD, TARGET_OLD_SHA,
                                               target_before['params'])['boot_id'] == target_before['boot_id']
        except Exception as error:
            state['target_restore_error'] = str(error)
            state['success'] = False
            errors.append('target: ' + str(error))
        save()
    if errors or not state.get('restored') or not state.get('target_restored'):
        state['success'] = False
        save()
        raise RuntimeError('Module restoration failed; inspect ' + str(state_file) + ': ' + ', '.join(errors))
print(json.dumps(state, indent=2))
