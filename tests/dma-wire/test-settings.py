from pathlib import Path
import json, subprocess, sys, unittest
R=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(R/'scripts/ae/k'))
from k_mode import KernelSettings, validate_dma_config, validate_preflight

def log(role,enabled):
    s=f'SB_KERNEL connected role={role} device=mlx5_1 PF=2 FT=1 BG=4 pretransfer=enabled dma_mr={int(enabled)}\n'
    if enabled:
        s+=f'SB_KERNEL dma_maps phase=ps role={role} regions=4 pages=128 bytes=1024 elapsed_us=21\n'
        s+=f'SB_KERNEL dma_maps phase=final role={role} regions=8 pages=256 bytes=2048 elapsed_us=31\n'
    return s
class Settings(unittest.TestCase):
    def test_forwarding(self):
        k=KernelSettings(dma_mr=True)
        self.assertIn('kernel-dma-mr=yes\n',k.config())
        self.assertIn('--kernel-dma-mr',k.argv())
        self.assertNotIn('--kernel-dma-mr',KernelSettings().argv())
        command=[sys.executable,str(R/'scripts/run.py')]
        preview=json.loads(subprocess.check_output(command+['K','--kernel-dma-mr'],text=True))
        self.assertIn('--kernel-dma-mr',preview['command'])
        self.assertFalse(preview['mutates_hosts'])
        self.assertNotEqual(subprocess.run(command+['U','--kernel-dma-mr'],capture_output=True).returncode,0)
    def test_executed_evidence(self):
        self.assertTrue(validate_dma_config(log('source',True),log('destination',True),True)['enabled'])
        self.assertFalse(validate_dma_config(log('source',False),log('destination',False),False)['enabled'])
        for source,dest,enabled in [
            ('',log('destination',True),True),
            (log('source',False),log('destination',False),True),
            (log('source',True),log('destination',True),False),
            (log('source',True)*2,log('destination',True),True),
            (log('source',True),log('destination',True).replace('pages=256','pages=257'),True),
            (log('source',True),log('destination',True).replace('regions=8','regions=7'),True),
            (log('source',True),log('destination',True).split('phase=final')[0],True)]:
            with self.assertRaises(ValueError):validate_dma_config(source,dest,enabled)
    def test_capability_preflight(self):
        hosts={}
        for host in ('knode2','knode3'):
            hosts[host]={'binary_sha256':'abc','installed_sha256':'abc','pageclient_sha256':'abc',
                'daemons':[{'name':n,'sha256':'abc'} for n in ('dockerd','containerd')],
                'capabilities':{'version':1,'features':127},'rdma_state':'4: ACTIVE','rdma_gid':'1234'}
        self.assertEqual(validate_preflight(hosts,16,True),[])
        for host in hosts:
            hosts[host]['capabilities']['features']-=64
            self.assertTrue(any('DMA MR transport unavailable' in e for e in validate_preflight(hosts,16,True)))
            self.assertEqual(validate_preflight(hosts,16,False),[])
            hosts[host]['capabilities']['features']+=64
if __name__=='__main__':unittest.main()
