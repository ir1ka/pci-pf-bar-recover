#!/usr/bin/python3
import contextlib
import importlib.machinery
import importlib.util
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

p=Path(__file__).resolve().parents[1]/'bin/pci-pf-bar-boot'
l=importlib.machinery.SourceFileLoader('boot_option',str(p))
s=importlib.util.spec_from_loader(l.name,l);app=importlib.util.module_from_spec(s);l.exec_module(app)

class OptInTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory()
        self.config=Path(self.tmp.name)/'conf.d/rebalance'
        self.patch=patch.object(app,'CONFIG',self.config);self.patch.start()
        self.state_patch=patch.object(app,'STATE',Path(self.tmp.name)/'state');self.state_patch.start()
        self.stdout=contextlib.redirect_stdout(io.StringIO());self.stdout.__enter__()
    def tearDown(self):
        self.stdout.__exit__(None,None,None);self.patch.stop();self.state_patch.stop();self.tmp.cleanup()
    def test_ensure_id_scope_does_not_need_live_ports(self):
        with patch.object(app.shutil,'which',return_value='/update-initramfs'),patch.object(app,'refresh')as refresh:
            app.enable([],ensure_id='10DE:20F1')
        self.assertIn("PFBAR_ENSURE_ID='10de:20f1'",self.config.read_text())
        self.assertIn("PFBAR_PORTS=''",self.config.read_text());refresh.assert_called_once()
    def test_ensure_id_rejects_invalid_or_mixed_scope(self):
        for values,identity in [([], '10de:*'),(['0000:00:03.1'],'10de:20f1')]:
            with self.assertRaises(RuntimeError):app.enable(values,ensure_id=identity)
        self.assertFalse(self.config.exists())
    def test_default_is_disabled(self):
        self.assertFalse(self.config.exists())
        with patch.object(app,'refresh') as r:
            app.disable();r.assert_not_called()
    def test_enable_requires_explicit_ports(self):
        with patch.object(app,'validate_ports',return_value=[]),self.assertRaisesRegex(RuntimeError,'explicit'):
            app.enable([])
        self.assertFalse(self.config.exists())
    def test_enable_writes_optin_then_refreshes(self):
        with patch.object(app,'validate_ports',return_value=['0000:00:03.1']),patch.object(app.shutil,'which',return_value='/update-initramfs'),patch.object(app,'refresh') as r:
            app.enable(['0000:00:03.1'])
            self.assertIn('PFBAR_REBALANCE=experimental',self.config.read_text())
            r.assert_called_once()
    def test_failed_enable_rolls_back_config_and_rebuilds(self):
        with patch.object(app,'validate_ports',return_value=['0000:00:03.1']),patch.object(app.shutil,'which',return_value='/update-initramfs'),patch.object(app,'refresh',side_effect=[RuntimeError('failed'),None]) as r,self.assertRaisesRegex(RuntimeError,'original config'):
            app.enable(['0000:00:03.1'])
        self.assertFalse(self.config.exists());self.assertEqual(r.call_count,2)
    def test_disable_failure_does_not_restore_enabled_config(self):
        self.config.parent.mkdir();self.config.write_text(app.MARKER+'PFBAR_REBALANCE=experimental\n')
        with patch.object(app,'refresh',side_effect=RuntimeError('failed')),self.assertRaisesRegex(RuntimeError,'old image'):
            app.disable()
        self.assertFalse(self.config.exists())
        self.assertTrue(app.STATE.exists())
        with patch.object(app,'refresh') as r:
            app.disable();r.assert_called_once()
        self.assertFalse(app.STATE.exists())
    def test_unmanaged_config_refused(self):
        self.config.parent.mkdir();self.config.write_text('custom\n')
        with self.assertRaisesRegex(RuntimeError,'not tool-managed'):
            app.disable()
        self.assertEqual(self.config.read_text(),'custom\n')
    def test_enable_cli_without_experimental_flag(self):
        with patch('sys.argv',['pci-pf-bar-boot','enable','--ensure-id','10de:20f1']),patch.object(app.os,'geteuid',return_value=0),patch.object(app,'enable')as enable:
            self.assertEqual(app.main(),0)
            enable.assert_called_once_with([],'10de:20f1')
    def test_old_experimental_flag_is_accepted_for_compatibility(self):
        with patch('sys.argv',['pci-pf-bar-boot','enable','--experimental','--ports','0000:00:03.1']),patch.object(app.os,'geteuid',return_value=0),patch.object(app,'enable')as enable:
            self.assertEqual(app.main(),0)
            enable.assert_called_once_with(['0000:00:03.1'],None)
    def test_help_does_not_advertise_retired_flag(self):
        with patch('sys.argv',['pci-pf-bar-boot','enable','--help']),contextlib.redirect_stdout(io.StringIO())as output,self.assertRaises(SystemExit):app.main()
        self.assertNotIn('--experimental',output.getvalue())


    def test_symlink_config_never_changes_administrator_target(self):
        target=Path(self.tmp.name)/'administrator';target.write_text(app.MARKER+'PFBAR_REBALANCE=experimental\n')
        self.config.parent.mkdir();self.config.symlink_to(target)
        before=target.read_bytes()
        with patch.object(app,'refresh')as refresh,self.assertRaisesRegex(RuntimeError,'symlink'):app.disable()
        refresh.assert_not_called();self.assertEqual(target.read_bytes(),before)
    def test_unmanaged_state_is_preserved(self):
        app.STATE.write_text('administrator notes')
        with self.assertRaisesRegex(RuntimeError,'Unmanaged image state'):app.disable()
        self.assertEqual(app.STATE.read_text(),'administrator notes')



    def test_foreign_package_configuration_is_refused(self):
        self.config.parent.mkdir();self.config.write_text(app.MARKER+'PFBAR_REBALANCE=experimental\n')
        from unittest.mock import Mock
        query=Mock();query.communicate.return_value=('configuration-package: '+str(self.config)+'\n',);query.returncode=0
        with patch.object(app.subprocess,'Popen')as popen,patch.object(app,'refresh')as refresh:
            popen.return_value.__enter__.return_value=query
            with self.assertRaisesRegex(RuntimeError,'another package'):app.disable()
        refresh.assert_not_called();self.assertTrue(self.config.exists())

if __name__=='__main__':unittest.main()
