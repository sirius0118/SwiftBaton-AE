"""Regression: the K adapter previously discarded validation-workers."""
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'scripts/ae/k'))
from k_mode import KernelSettings, validate_catalog_config
class Settings(unittest.TestCase):
    def test_options_reach_config_and_cli(self):
        for count in (1, 16, 32):
            k = KernelSettings(validation_workers=count)
            self.assertIn('validation-workers='+str(count)+'\n', k.config())
            args=k.argv()
            self.assertEqual(args[args.index('--validation-workers')+1], str(count))
        for count in (0, 33):
            with self.assertRaises(ValueError): KernelSettings(validation_workers=count).values()
    def test_requested_is_not_executed(self):
        line='SB_KERNEL final_catalog regions=414 workers=1 validation_us=21 prepare_us=2300 seal_us=61 result=0'
        with self.assertRaises(ValueError): validate_catalog_config(line, 16)
        with self.assertRaises(ValueError): validate_catalog_config('', 1)
        with self.assertRaises(ValueError): validate_catalog_config(line+'\n'+line, 1)
        with self.assertRaises(ValueError): validate_catalog_config(line.replace('result=0','result=-12'), 1)
        self.assertEqual(validate_catalog_config(line, 1)['workers'], 1)
if __name__=='__main__': unittest.main()
