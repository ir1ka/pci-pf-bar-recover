#!/usr/bin/python3
import contextlib
import importlib.machinery
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
p=Path(__file__).resolve().parents[1]/'bin/pci-pf-bar-recover'
loader=importlib.machinery.SourceFileLoader('launcher',str(p))
spec=importlib.util.spec_from_loader(loader.name,loader)
app=importlib.util.module_from_spec(spec);loader.exec_module(app)

class LauncherTests(unittest.TestCase):
    def setUp(self):
        patched=patch.object(app,'power_app');self.power=patched.start().return_value
        self.power.run_recovery.return_value=None;self.addCleanup(patched.stop)

    def execute(self,ports,version=None,parameters='execute:bool\nports:string\nrestored:uint\nfailed:uint\nskipped:uint\ncmp_reserve:bool\nconservative_only:bool'):
        with patch.object(app.os,'geteuid',return_value=0),patch.object(app,'command',side_effect=lambda v:v),patch.object(app.subprocess,'check_output',side_effect=['/test.ko',(version or app.platform.release())+' SMP',parameters]),patch.object(app,'read_result',return_value={'restored':1,'failed':0,'skipped':0}),patch.object(app.subprocess,'run') as run,contextlib.redirect_stdout(io.StringIO()):
            app.recover(ports)
            return [call.args[0] for call in run.call_args_list]
    def test_status_includes_selected_boundary_windows_and_excludes_other_roots(self):
        import tempfile
        with tempfile.TemporaryDirectory()as tmp:
            base=Path(tmp);devices=base/'devices';devices.mkdir()
            root=base/'pci0000:00/0000:00:03.1';ep=root/'0000:04:00.0';ep.mkdir(parents=True)
            other=base/'pci0000:00/0000:00:04.0';other.mkdir()
            for p in (root,ep,other):
                (devices/p.name).symlink_to(p)
                rows=['0 0 0']*17
                if p==root:rows[14]='100000000 1ffffffff 2200'
                else:rows[0]='83000000 83ffffff 200'
                (p/'resource').write_text('\n'.join(rows)+'\n')
            with patch.object(app,'PCI',devices):report=app.status([root.name])
            self.assertEqual([d['bdf']for d in report['devices']],[root.name,ep.name])
            self.assertEqual(report['devices'][0]['bridge_windows'][0]['size'],1<<32)
    def test_report_is_private_exclusive_and_saved_before_unload(self):
        import tempfile,json
        with tempfile.TemporaryDirectory()as tmp:
            report=Path(tmp)/'result.json'
            params='execute:bool\nports:string\nrestored:uint\nfailed:uint\nskipped:uint\ncmp_reserve:bool\nconservative_only:bool'
            def run(argv,**kwargs):
                if argv[0]=='rmmod':
                    self.assertTrue(report.exists())
                    self.assertEqual(json.loads(report.read_text())['result']['records'][0]['stage'],'commit')
            with patch.object(app.os,'geteuid',return_value=0),patch.object(app,'command',side_effect=lambda v:v),patch.object(app.subprocess,'check_output',side_effect=['/test.ko',app.platform.release()+' SMP',params]),patch.object(app,'status',return_value={'devices':[]}),patch.object(app,'read_result',return_value={'restored':1,'failed':0,'skipped':0,'records':[{'stage':'commit'}]}),patch.object(app.subprocess,'run',side_effect=run),contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(app.recover([],power_retry=False,report_path=report),0)
            self.assertEqual(report.stat().st_mode&0o777,0o600)
            with self.assertRaises(FileExistsError):app.save_report(report,{})
    def test_existing_report_refused_before_any_module_action(self):
        import tempfile
        with tempfile.TemporaryDirectory()as tmp:
            report=Path(tmp)/'report';report.write_text('keep')
            with patch.object(app.subprocess,'run')as run,self.assertRaisesRegex(RuntimeError,'Report already exists'):
                app.recover([],report_path=report)
            run.assert_not_called()
    def test_automatic_power_hold_delegates_without_loading_twice(self):
        self.power.run_recovery.return_value=0
        params='execute:bool\nports:string\nrestored:uint\nfailed:uint\nskipped:uint\ncmp_reserve:bool\nconservative_only:bool'
        with patch.object(app.os,'geteuid',return_value=0),patch.object(app,'command',side_effect=lambda v:v),patch.object(app.subprocess,'check_output',side_effect=['/test.ko',app.platform.release()+' SMP',params]),patch.object(app.subprocess,'run')as run:
            self.assertEqual(app.recover(['0000:00:03.1']),0)
            run.assert_not_called()
            self.assertTrue(self.power.run_recovery.call_args.kwargs['automatic'])
    def test_still_unavailable_is_not_retried_in_a_loop(self):
        self.power.run_recovery.return_value=None
        params='execute:bool\nports:string\nrestored:uint\nfailed:uint\nskipped:uint\ncmp_reserve:bool\nconservative_only:bool'
        with patch.object(app.os,'geteuid',return_value=0),patch.object(app,'command',side_effect=lambda v:v),patch.object(app.subprocess,'check_output',side_effect=['/test.ko',app.platform.release()+' SMP',params]),patch.object(app.subprocess,'run'),patch.object(app,'read_result',return_value={'restored':0,'failed':0,'skipped':1,'unavailable_devices':['0000:01:00.0']}),contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(app.recover(['0000:00:01.0']),2)
            self.assertEqual(self.power.run_recovery.call_count,1)
    def test_power_hold_can_be_disabled(self):
        params='execute:bool\nports:string\nrestored:uint\nfailed:uint\nskipped:uint\ncmp_reserve:bool\nconservative_only:bool'
        with patch.object(app.os,'geteuid',return_value=0),patch.object(app,'command',side_effect=lambda v:v),patch.object(app.subprocess,'check_output',side_effect=['/test.ko',app.platform.release()+' SMP',params]),patch.object(app.subprocess,'run'),patch.object(app,'read_result',return_value={'restored':1,'failed':0,'skipped':0}),contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(app.recover([],power_retry=False),0)
            self.power.run_recovery.assert_not_called()
    def test_debug_is_explicit_and_bounded(self):
        params='execute:bool\nports:string\nrestored:uint\nfailed:uint\nskipped:uint\ncmp_reserve:bool\nconservative_only:bool\ndebug:bool\ndebug_limit:uint'
        with patch.object(app.os,'geteuid',return_value=0),patch.object(app,'command',side_effect=lambda v:v),patch.object(app.subprocess,'check_output',side_effect=['/test.ko',app.platform.release()+' SMP',params]),patch.object(app.subprocess,'run')as run,patch.object(app,'read_result',return_value={'restored':1,'failed':0,'skipped':0}),contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(app.recover([],debug=True,debug_limit=17),0)
            self.assertEqual(run.call_args_list[0].args[0][-2:],['debug=1','debug_limit=17'])
    def test_invalid_debug_budget_does_not_load_module(self):
        with patch.object(app.subprocess,'run')as run:
            with self.assertRaisesRegex(RuntimeError,'debug-limit'):app.recover([],debug=True,debug_limit=0)
            run.assert_not_called()
    def test_all_skipped_is_not_success(self):
        with patch.object(app.os,'geteuid',return_value=0),patch.object(app,'command',side_effect=lambda v:v),patch.object(app.subprocess,'check_output',side_effect=['/test.ko',app.platform.release()+' SMP','execute:bool\nports:string\nrestored:uint\nfailed:uint\nskipped:uint\ncmp_reserve:bool\nconservative_only:bool']),patch.object(app.subprocess,'run')as run,patch.object(app,'read_result',return_value={'restored':0,'failed':0,'skipped':1}),contextlib.redirect_stdout(io.StringIO())as output:
            self.assertEqual(app.recover(['0000:00:03.1']),2)
            self.assertIn('No device recovery confirmed',output.getvalue())
            self.assertEqual(run.call_args_list[-1].args[0],['rmmod',app.MODULE])
    def test_partial_recovery_reports_nonzero_after_unload(self):
        params='execute:bool\nports:string\nrestored:uint\nfailed:uint\nskipped:uint\ncmp_reserve:bool\nconservative_only:bool'
        with patch.object(app.os,'geteuid',return_value=0),patch.object(app,'command',side_effect=lambda v:v),patch.object(app.subprocess,'check_output',side_effect=['/test.ko',app.platform.release()+' SMP',params]),patch.object(app.subprocess,'run')as run,patch.object(app,'read_result',return_value={'restored':1,'failed':1,'skipped':0}),contextlib.redirect_stdout(io.StringIO())as output:
            self.assertEqual(app.recover([]),3)
            self.assertIn('Partial recovery',output.getvalue())
            self.assertEqual(run.call_args_list[-1].args[0],['rmmod',app.MODULE])
    def test_unreadable_results_still_unload_module(self):
        with patch.object(app.os,'geteuid',return_value=0),patch.object(app,'command',side_effect=lambda v:v),patch.object(app.subprocess,'check_output',side_effect=['/test.ko',app.platform.release()+' SMP','execute:bool\nports:string\nrestored:uint\nfailed:uint\nskipped:uint\ncmp_reserve:bool\nconservative_only:bool']),patch.object(app.subprocess,'run')as run,patch.object(app,'read_result',side_effect=OSError('missing counter')):
            with self.assertRaises(OSError):app.recover([])
            self.assertEqual(run.call_args_list[-1].args[0],['rmmod',app.MODULE])
    def test_conservative_only_is_forwarded_to_module(self):
        params='execute:bool\nports:string\nrestored:uint\nfailed:uint\nskipped:uint\ncmp_reserve:bool\nconservative_only:bool'
        with patch.object(app.os,'geteuid',return_value=0),patch.object(app,'command',side_effect=lambda v:v),patch.object(app.subprocess,'check_output',side_effect=['/test.ko',app.platform.release()+' SMP',params]),patch.object(app.subprocess,'run')as run,patch.object(app,'read_result',return_value={'restored':1,'failed':0,'skipped':0}),contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(app.recover([],conservative_only=True),0)
            self.assertIn('conservative_only=1',run.call_args_list[0].args[0])
            self.assertTrue(self.power.run_recovery.call_args.kwargs['conservative_only'])
    def test_force_and_conservative_conflict(self):
        with self.assertRaisesRegex(RuntimeError,'conflicts'):
            app.recover(['0000:00:03.1'],rebalance=True,conservative_only=True)
    def test_rebalance_requires_explicit_ports(self):
        with self.assertRaisesRegex(RuntimeError,'explicit --ports'):
            app.recover([],rebalance=True)
    def test_forced_rebalance_is_not_enabled_by_default(self):
        self.assertNotIn('rebalance=1',self.execute([])[0])
    def test_default_all_has_explicit_execution(self):
        self.assertEqual(self.execute([]),[['modprobe','pci_pf_bar_recover','execute=1','ports='],['rmmod','pci_pf_bar_recover']])
    def test_multiple_ports_are_passed_as_list(self):
        self.assertEqual(self.execute(['0000:00:03.1','0000:00:04.0'])[0][-1],'ports=0000:00:03.1,0000:00:04.0')
    def test_wrong_abi_refused(self):
        with self.assertRaisesRegex(RuntimeError,'ABI'):
            self.execute([],version='wrong-kernel')
    def test_ungated_module_refused(self):
        with self.assertRaisesRegex(RuntimeError,'parameters'):
            self.execute([],parameters='other:bool')
    def test_bad_bdf_refused(self):
        with self.assertRaisesRegex(RuntimeError,'Invalid'):
            app.normalize_ports(['0000:00:ff.0'])
    def test_ensure_id_validation_and_conflicts(self):
        self.assertEqual(app.normalize_id('10DE:20F1'),'10de:20f1')
        for identity in ('10de:*','10de:20f1 extra','1:2'):
            with self.assertRaises(RuntimeError):app.normalize_id(identity)
        with self.assertRaisesRegex(RuntimeError,'cannot combine'):
            app.recover(['0000:00:03.1'],ensure_id='10de:20f1')
    def test_ensure_id_healthy_no_repairs_is_success(self):
        params='execute:bool\nports:string\nrestored:uint\nfailed:uint\nskipped:uint\ncmp_reserve:bool\nconservative_only:bool\nensure_id:string\nensure_matched:uint\nensure_complete:uint'
        with patch.object(app.os,'geteuid',return_value=0),patch.object(app,'command',side_effect=lambda v:v),patch.object(app.subprocess,'check_output',side_effect=['/test.ko',app.platform.release()+' SMP',params]),patch.object(app.subprocess,'run')as run,patch.object(app,'read_result',return_value={'restored':0,'failed':0,'skipped':0,'ensure_matched':2,'ensure_complete':1}),contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(app.recover([],ensure_id='10de:20f1'),0)
            self.assertIn('ensure_id=10de:20f1',run.call_args_list[0].args[0])
    def test_cmp_preparation_alone_counts_as_success_without_double_counting_repairs(self):
        params='execute:bool\nports:string\nrestored:uint\nfailed:uint\nskipped:uint\ncmp_reserve:bool\nconservative_only:bool'
        with patch.object(app.os,'geteuid',return_value=0),patch.object(app,'command',side_effect=lambda v:v),patch.object(app.subprocess,'check_output',side_effect=['/test.ko',app.platform.release()+' SMP',params]),patch.object(app.subprocess,'run'),patch.object(app,'read_result',return_value={'restored':0,'failed':0,'skipped':0,'cmp_prepared':2}),contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(app.recover([],power_retry=False),0)
    def test_old_unlocking_module_is_rejected_before_execution(self):
        params='execute:bool\nports:string\nrestored:uint\nfailed:uint\nskipped:uint\nconservative_only:bool\ncmp_unlock:bool'
        with patch.object(app.os,'geteuid',return_value=0),patch.object(app,'command',side_effect=lambda v:v),patch.object(app.subprocess,'check_output',side_effect=['/test.ko',app.platform.release()+' SMP',params]),patch.object(app.subprocess,'run')as run:
            with self.assertRaisesRegex(RuntimeError,'older module'):app.recover([],power_retry=False)
            run.assert_not_called()
    def test_recover_requires_root(self):
        with patch.object(app.os,'geteuid',return_value=1000),self.assertRaisesRegex(RuntimeError,'sudo'):
            app.recover([])
    def test_status_never_runs_recovery(self):
        with patch('sys.argv',['pci-pf-bar-recover','status']),patch.object(app,'status',return_value={}),patch.object(app,'recover') as restore,contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(app.main(),0)
            restore.assert_not_called()

if __name__=='__main__':unittest.main()
