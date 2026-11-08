#!/usr/bin/python3
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
BASE=Path(__file__).resolve().parents[1]
class MaintainerTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name)
        self.cfg=self.root/'config';self.state=self.root/'state';self.log=self.root/'log'
        bin=self.root/'bin';bin.mkdir()
        for name in ('update-initramfs','pci-pf-bar-boot'):
            p=bin/name;p.write_text('#!/bin/sh\necho "'+name+':$*" >> "$TEST_LOG"\n');p.chmod(0o755)
        self.env=dict(os.environ,PATH=str(bin)+':'+os.environ['PATH'],TEST_LOG=str(self.log))
    def tearDown(self):self.tmp.cleanup()
    def run_script(self,name,arg):
        text=(BASE/'debian'/('pci-pf-bar-recover-dkms.'+name)).read_text().replace('/etc/initramfs-tools/conf.d/pci-pf-bar-rebalance',str(self.cfg)).replace('/var/lib/pci-pf-bar-recover/early-image-state',str(self.state)).replace('/usr/sbin/pci-pf-bar-boot',str(self.root/'bin/pci-pf-bar-boot'))
        p=self.root/name;p.write_text(text)
        subprocess.run(['/bin/sh',str(p),arg],env=self.env,check=True)
    def test_fresh_install_does_not_activate_or_rebuild(self):
        self.run_script('postinst','configure')
        self.assertFalse(self.cfg.exists());self.assertFalse(self.state.exists());self.assertFalse(self.log.exists())
    def test_enabled_upgrade_rebuilds(self):
        self.cfg.write_text('enabled')
        self.run_script('postinst','configure')
        self.assertEqual(self.log.read_text().strip(),'update-initramfs:-u -k all')
    def test_remove_disabled_does_not_run_boot_tool(self):
        self.run_script('prerm','remove');self.assertFalse(self.log.exists())
    def test_remove_enabled_disables_before_dkms(self):
        self.cfg.write_text('enabled');self.run_script('prerm','remove')
        self.assertEqual(self.log.read_text().strip(),'pci-pf-bar-boot:disable')
    def test_partial_disable_is_cleaned_on_remove(self):
        self.state.write_text('disable-rebuild-needed');self.run_script('prerm','remove')
        self.assertEqual(self.log.read_text().strip(),'pci-pf-bar-boot:disable')
if __name__=='__main__':unittest.main()
