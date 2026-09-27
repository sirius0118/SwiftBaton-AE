import sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'scripts'))
from migration_window import select_zero_run

def row(t,n,rate):return (1000+t,t,n,rate,.01,1 if rate else 0)
class TestWindow(unittest.TestCase):
    def test_terminal_idle_longer_than_migration(self):
        rows=[row(20,1,1),row(20.01,1,0),row(20.02,1,0),row(20.03,2,1),row(90,100,0),row(90.05,100,0)]
        selected,all_runs=select_zero_run(rows,10,20.03,(20.005,20.025))
        self.assertEqual(selected,[20.01,20.02]);self.assertFalse(all_runs[-1]['closed'])
    def test_unrelated_later_stall_retained_not_migration(self):
        rows=[row(20,1,0),row(20.02,1,0),row(20.03,2,1),row(50,10,0),row(51,10,0),row(51.01,11,1)]
        selected,runs=select_zero_run(rows,10,20.03)
        self.assertEqual(selected,[20,20.02]);self.assertEqual(len(runs),2)
    def test_no_migration_zero_is_missing(self):
        self.assertEqual(select_zero_run([row(90,1,0),row(90.1,1,0)],10,20)[0],[])
    def test_missing_cutover_does_not_guess(self):
        self.assertEqual(select_zero_run([row(20,1,0),row(20.1,2,1)],10,float('nan'))[0],[])
if __name__=='__main__':unittest.main()
