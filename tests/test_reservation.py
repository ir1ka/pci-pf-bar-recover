"""Execute production window-planning helpers in a deterministic userspace hardware model."""
import os,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def function(text,name):
    start=text.index('static int '+name+'(')
    pos=text.index('{',start);depth=1;end=pos+1
    while depth:
        if text[end]=='{':depth+=1
        elif text[end]=='}':depth-=1
        end+=1
    return text[start:end]+'\n'

class Cmp(unittest.TestCase):
    def test_actual_window_planning_functions(self):
        text=(ROOT/'src/rebalance.inc').read_text()
        helpers='\n'.join(function(text,name)for name in ('idle_plan_extents','idle_finish_extents','idle_shrink_reservation','idle_pack_advance'))
        code=(ROOT/'tests/window_policy_fixture.c').read_text().replace('/* PRODUCTION_FUNCTIONS */',helpers)
        with tempfile.TemporaryDirectory()as tmp:
            c=Path(tmp)/'fixture.c';exe=Path(tmp)/'fixture';c.write_text(code)
            subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(ROOT/'src'),str(c),'-o',str(exe)],check=True)
            result=subprocess.run([str(exe)],check=True,capture_output=True,text=True)
            self.assertIn('PASS:',result.stdout)
    def test_no_vendor_unlock_and_transaction_order(self):
        import re
        main=(ROOT/'src/pci_pf_bar_recover.c').read_text();text=(ROOT/'src/rebalance.inc').read_text()
        self.assertIn('static bool cmp_reserve = true;',main)
        kernel='\n'.join(p.read_text()for p in (ROOT/'src').glob('*')if p.suffix in ('.c','.inc'))
        self.assertFalse(re.search(r'\b(?:ioremap|iounmap|readl|writel)\s*\(',kernel))
        self.assertNotIn('0x8000000a',kernel.lower())
        run=text[text.index('static bool rebalance_roots('):text.index('static bool rebalance_root(')]
        self.assertLess(run.index('idle_check_claims'),run.index('idle_disable'))
        allocate=function(text,'idle_allocate')
        self.assertLess(allocate.index('idle_plan_extents'),allocate.index('pci_bus_size_bridges'))
        self.assertLess(allocate.index('pci_assign_resource'),allocate.index('idle_finish_extents'))
        self.assertLess(run.index('idle_shrink_reservation'),run.index('idle_shrink_rebar'))
