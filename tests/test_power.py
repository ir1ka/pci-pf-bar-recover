#!/usr/bin/python3
"""Temporary runtime-PM holds must preserve scope and survive failed repairs."""
import contextlib
import importlib.machinery
import importlib.util
import io
import json
from pathlib import Path
import signal
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
loader=importlib.machinery.SourceFileLoader('power',str(Path(__file__).resolve().parents[1]/'bin/pci-pf-bar-power'))
spec=importlib.util.spec_from_loader(loader.name,loader);app=importlib.util.module_from_spec(spec);loader.exec_module(app)
class PowerTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.base=Path(self.tmp.name)
        self.pci=self.base/'bus/devices';self.pci.mkdir(parents=True)
        for n,v in [('PCI',self.pci),('RUNTIME',self.base/'run'),('STATE',self.base/'run/state.json'),('LOCK',self.base/'run/lock')]:
            p=patch.object(app,n,v);p.start();self.addCleanup(p.stop)
        p=patch.object(app.os,'geteuid',return_value=0);p.start();self.addCleanup(p.stop)
        self.root=self.device('0000:00:01.0',self.base/'devices/pci0000:00',True)
        self.ep=self.device('0000:01:00.0',self.root,False)
        self.other=self.device('0000:00:02.0',self.base/'devices/pci0000:00',True)
        self.child=SimpleNamespace(pid=1234,poll=lambda:2,wait=lambda:2)
    def device(self,bdf,parent,bridge):
        d=parent/bdf;(d/'power').mkdir(parents=True)
        for name,value in [('vendor','0x1234'),('device','0x5678'),('class','0x060400'if bridge else'0x030200'),('power/control','auto'),('power/runtime_status','suspended'if bridge else'unsupported'),('power_state','D0')]:
            (d/name).write_text(value+'\n')
        (self.pci/bdf).symlink_to(d,target_is_directory=True);return d
    def run_power(self,wait=None,**kwargs):
        if wait:self.child.wait=wait
        with patch.object(app.subprocess,'Popen',return_value=self.child)as popen,contextlib.redirect_stdout(io.StringIO()):
            result=app.run_recovery(['0000:00:01.0'],**kwargs)
        return result,popen
    def test_ensure_id_holds_only_roots_containing_matches(self):
        (self.ep/'vendor').write_text('0x10de\n');(self.ep/'device').write_text('0x20f1\n')
        def wait():
            self.assertEqual((self.root/'power/control').read_text().strip(),'on')
            self.assertEqual((self.other/'power/control').read_text().strip(),'auto');return 2
        self.child.wait=wait
        with patch.object(app,'recovery_app',return_value=SimpleNamespace(normalize_id=lambda x:x)),patch.object(app.subprocess,'Popen',return_value=self.child)as child,contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(app.run_recovery([],ensure_id='10de:20f1',automatic=True),2)
        self.assertIn('--ensure-id',child.call_args.args[0]);self.assertNotIn('--ports',child.call_args.args[0])
    def test_ensure_id_no_matches_never_holds_everything(self):
        with patch.object(app,'recovery_app',return_value=SimpleNamespace(normalize_id=lambda x:x)),patch.object(app.subprocess,'Popen')as child,contextlib.redirect_stdout(io.StringIO()):
            self.assertIsNone(app.run_recovery([],ensure_id='10de:20f1',automatic=True))
        child.assert_not_called();self.assertFalse(app.STATE.exists())
    def test_conservative_only_survives_power_child(self):
        rc,popen=self.run_power(conservative_only=True)
        self.assertEqual(rc,2)
        self.assertIn('--conservative-only',popen.call_args.args[0])
    def test_failed_repair_restores_original_policies_and_scope(self):
        def wait():
            self.assertEqual((self.root/'power/control').read_text().strip(),'on')
            self.assertEqual((self.ep/'power/control').read_text().strip(),'on')
            self.assertEqual((self.other/'power/control').read_text().strip(),'auto');return 2
        rc,popen=self.run_power(wait)
        self.assertEqual(rc,2);self.assertFalse(app.STATE.exists())
        self.assertEqual((self.root/'power/control').read_text().strip(),'auto')
        self.assertEqual((self.ep/'power/control').read_text().strip(),'auto')
        self.assertIn('--no-power-retry',popen.call_args.args[0])
        self.assertTrue(popen.call_args.kwargs['start_new_session'])
        self.assertEqual(len(popen.call_args.kwargs['pass_fds']),1)
    def test_write_applied_then_error_is_still_restored(self):
        real=Path.write_text
        def fail(path,value,*args,**kwargs):
            result=real(path,value,*args,**kwargs)
            if path.resolve()==self.root/'power/control'and value=='on\n':raise OSError('simulated partial write')
            return result
        with patch.object(Path,'write_text',new=fail),patch.object(app.subprocess,'Popen')as child,contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError,'partial write'):app.run_recovery(['0000:00:01.0'])
        child.assert_not_called();self.assertEqual((self.root/'power/control').read_text().strip(),'auto');self.assertFalse(app.STATE.exists())
    def test_automatic_preparation_failure_restores_then_allows_guarded_scan(self):
        real=Path.write_text
        def fail(path,value,*args,**kwargs):
            result=real(path,value,*args,**kwargs)
            if path.resolve()==self.root/'power/control'and value=='on\n':raise OSError('prepare failed')
            return result
        with patch.object(Path,'write_text',new=fail):
            rc,child=self.run_power(automatic=True)
        self.assertIsNone(rc);child.assert_not_called()
        self.assertFalse(app.STATE.exists());self.assertEqual((self.root/'power/control').read_text().strip(),'auto')
    def test_restore_error_keeps_journal_for_explicit_restore(self):
        real=Path.write_text
        def fail(path,value,*args,**kwargs):
            if path.resolve()==self.root/'power/control'and value=='auto\n':raise OSError('restore blocked')
            return real(path,value,*args,**kwargs)
        with patch.object(Path,'write_text',new=fail),contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError,'journal retained'):self.run_power()
        self.assertTrue(app.STATE.exists())
        with patch('sys.argv',['pci-pf-bar-power','restore']),contextlib.redirect_stdout(io.StringIO()):self.assertEqual(app.main(),0)
        self.assertFalse(app.STATE.exists());self.assertEqual((self.root/'power/control').read_text().strip(),'auto')
    def test_automatic_hold_pins_active_but_auto_managed_path(self):
        (self.root/'power/runtime_status').write_text('active\n')
        rc,_=self.run_power(automatic=True);self.assertEqual(rc,2)
    def test_already_on_active_path_does_not_retry(self):
        for d in (self.root,self.ep):(d/'power/control').write_text('on\n');(d/'power/runtime_status').write_text('active\n')
        rc,popen=self.run_power(automatic=True);self.assertIsNone(rc);popen.assert_not_called()
    def test_concurrent_power_operation_is_rejected(self):
        with app.locked(),self.assertRaisesRegex(RuntimeError,'still running'):
            with app.locked():pass
    def test_termination_is_deferred_until_repair_finishes(self):
        def wait():
            handler=signal.getsignal(signal.SIGTERM);self.assertTrue(callable(handler));handler(signal.SIGTERM,None)
            self.assertEqual((self.root/'power/control').read_text().strip(),'on');return 0
        rc,_=self.run_power(wait);self.assertEqual(rc,143)
        self.assertEqual((self.root/'power/control').read_text().strip(),'auto')
    def test_real_child_inherits_lock_and_sees_held_power(self):
        bindir=self.base/'bin';bindir.mkdir();child=bindir/'pci-pf-bar-recover'
        child.write_text("#!/usr/bin/python3\nimport os,sys\nfrom pathlib import Path\n"+
            "assert '--no-power-retry' in sys.argv\n"+
            f"assert Path({str(self.root/'power/control')!r}).read_text().strip()=='on'\n"+
            f"assert Path({str(self.ep/'power/control')!r}).read_text().strip()=='on'\n"+
            f"inode=Path({str(app.LOCK)!r}).stat().st_ino\n"+
            "inherited=False\nfor fd in os.listdir('/proc/self/fd'):\n try:\n  if int(fd)>2 and os.fstat(int(fd)).st_ino==inode:inherited=True\n except OSError:pass\nassert inherited\nsys.exit(2)\n")
        child.chmod(0o755)
        with patch.object(app,'__file__',str(bindir/'pci-pf-bar-power')),contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(app.run_recovery(['0000:00:01.0']),2)
        self.assertFalse(app.STATE.exists())
        self.assertEqual((self.root/'power/control').read_text().strip(),'auto')
    def test_report_keeps_unmodified_before_snapshot(self):
        log=self.base/'report.json';self.run_power(log=log)
        before=json.loads(log.read_text())['events'][0]['devices']
        self.assertTrue(all(not e['attempted']and not e['restored']for e in before))
if __name__=='__main__':unittest.main()
