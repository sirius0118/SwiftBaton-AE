from pathlib import Path
import json,subprocess,sys,unittest
R=Path(__file__).resolve().parents[2];sys.path.insert(0,str(R/'scripts/ae/k'))
from k_mode import KernelSettings,validate_preflight,validate_ps_arm_config,completion_stats

def logs(enabled):
    s=f'SB_KERNEL ps_arm_mode role=source enabled={int(enabled)}\n'
    d=f'SB_KERNEL ps_arm_mode role=destination enabled={int(enabled)}\n'
    if enabled:
        s+='SB_KERNEL ps_layout role=source regions=3 pages=24 pids=1 skipped_pids=0 skipped_ranges=0 elapsed_us=5\n'
        d+='SB_KERNEL ps_layout role=destination regions=3 pages=24 elapsed_us=10\n'
        d+='SB_KERNEL ps_arm_reuse prepared=3 prepared_pages=24 reused=2 reused_pages=16 discarded=1 discarded_pages=8 final=4 result=0\n'
    return s,d
class Settings(unittest.TestCase):
    def test_retired_page_accounting(self):
        complete='SB_KERNEL complete regions=1 pages=8 PF=2 FT=1 BG=2 errors=0 fault_ns=10 fault_max_ns=5 PS=3 invalid=1 faults=1 hits=0 retired_unfetched=1\n'
        audit='SB_KERNEL deficit_audit result=0 completed=7 pages=8 retired_unfetched=1\n'
        self.assertEqual(completion_stats(audit+complete)['retired_unfetched'],1)
        for bad in (complete, audit.replace('result=0','result=-71')+complete,
                    audit.replace('completed=7','completed=8')+complete,
                    audit+complete.replace('retired_unfetched=1','retired_unfetched=0')):
            with self.assertRaises(ValueError): completion_stats(bad)
    def test_forward(self):
        self.assertIn('--kernel-ps-arm',KernelSettings(ps_arm=True).argv())
        self.assertIn('kernel-ps-arm=yes\n',KernelSettings(ps_arm=True).config())
        self.assertNotIn('--kernel-ps-arm',KernelSettings().argv())
        cmd=[sys.executable,str(R/'scripts/run.py')]
        p=json.loads(subprocess.check_output(cmd+['K','--kernel-ps-arm'],text=True))
        self.assertFalse(p['mutates_hosts']);self.assertIn('--kernel-ps-arm',p['command'])
        self.assertNotEqual(subprocess.run(cmd+['U','--kernel-ps-arm'],capture_output=True).returncode,0)
    def test_evidence(self):
        s,d=logs(True);self.assertEqual(validate_ps_arm_config(s,d,True)['reused_pages'],16)
        self.assertFalse(validate_ps_arm_config(*logs(False),False)['enabled'])
        for bad in [d.replace('enabled=1','enabled=0'),d+d,d.replace('reused=2','reused=3'),d.replace('pages=24','pages=23',1),d.replace('result=0','result=-12'),d.replace('final=4','final=1')]:
            with self.assertRaises(ValueError):validate_ps_arm_config(s,bad,True)
        with self.assertRaises(ValueError):validate_ps_arm_config(s,d,False)
        with self.assertRaises(ValueError):validate_ps_arm_config('',d,True)
    def test_capabilities(self):
        hosts={h:dict(binary_sha256='a',installed_sha256='a',pageclient_sha256='a',daemons=[dict(name=n,sha256='a') for n in ('dockerd','containerd')],capabilities=dict(version=1,features=511),rdma_state='4: ACTIVE',rdma_gid='1234') for h in ('node2','node3')}
        self.assertEqual(validate_preflight(hosts,16,True,True),[])
        hosts['node2']['capabilities']['features']=127
        self.assertEqual(validate_preflight(hosts,16,True,True),[])
        for features in (127,255,383):
            hosts['node3']['capabilities']['features']=features
            self.assertTrue(any('PS ARM preparation unavailable' in e for e in validate_preflight(hosts,16,True,True)))
            self.assertEqual(validate_preflight(hosts,16,True,False),[])
if __name__=='__main__':unittest.main()
