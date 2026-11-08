#!/usr/bin/python3
"""Exercise real hook/wrapper logic using staged paths and fake kmod binaries."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

BASE=Path(__file__).resolve().parents[1]
class HookTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name)
        self.conf=self.root/'conf';(self.conf/'conf.d').mkdir(parents=True)
        self.dest=self.root/'image';(self.dest/'scripts/init-top').mkdir(parents=True)
        self.log=self.root/'order.log'
        self.original='#!/bin/sh\ncase ${1:-} in prereqs) echo dep;exit 0;;esac\necho udev >> "$ORDER_LOG"\n'
        (self.dest/'scripts/init-top/udev').write_text(self.original)
        (self.dest/'scripts/init-top/udev').chmod(0o755)
        stub=self.root/'functions';stub.write_text('copy_exec() { :; }\nmanual_add_modules() { echo "$1" > "$DESTDIR/module-copied"; }\n')
        fakebin=self.root/'bin';fakebin.mkdir();p=fakebin/'modinfo';p.write_text('#!/bin/sh\ncase " $* " in *" -p "*) echo cmp_reserve:bool; echo conservative_only:bool; echo ensure_id:string;; *) echo pci_pf_bar_recover;; esac\n');p.chmod(0o755)
        hook=(BASE/'initramfs/pci-pf-bar-rebalance').read_text().replace('. /usr/share/pci-pf-bar-recover/initramfs-runtime', 'echo pci_pf_bar_recover > "$DESTDIR/module-copied"').replace('/usr/share/initramfs-tools/hook-functions',str(stub)).replace('/usr/share/pci-pf-bar-recover/prebuilt-kernels',str(self.root/'prebuilt-kernels')).replace('/usr/share/pci-pf-bar-recover/',str(BASE/'lib')+'/')
        self.hook=self.root/'hook';self.hook.write_text(hook)
        self.env=dict(os.environ,CONFDIR=str(self.conf),DESTDIR=str(self.dest),version='test-kernel',PATH=str(fakebin)+':'+os.environ['PATH'],ORDER_LOG=str(self.log))
    def tearDown(self):self.tmp.cleanup()
    def build(self,enabled=False):
        if enabled:(self.conf/'conf.d/pci-pf-bar-rebalance').write_text("PFBAR_REBALANCE=experimental\nPFBAR_PORTS='0000:00:03.1'\n")
        subprocess.run(['/bin/sh',str(self.hook)],env=self.env,check=True,capture_output=True)
    def test_ensure_id_is_embedded_without_host_port_discovery(self):
        (self.conf/'conf.d/pci-pf-bar-rebalance').write_text("PFBAR_REBALANCE=experimental\nPFBAR_PORTS=''\nPFBAR_ENSURE_ID='10de:20f1'\n")
        self.build()
        self.assertEqual((self.dest/'conf/pci-pf-bar-ensure-id').read_text(),'10de:20f1\n')
    def test_invalid_or_mixed_id_config_fails_build(self):
        for ports,identity in [('0000:00:03.1','10de:20f1'),('', '10de:*')]:
            (self.conf/'conf.d/pci-pf-bar-rebalance').write_text(f"PFBAR_REBALANCE=experimental\nPFBAR_PORTS='{ports}'\nPFBAR_ENSURE_ID='{identity}'\n")
            with self.assertRaises(subprocess.CalledProcessError):self.build()
    def test_disabled_hook_leaves_image_unchanged(self):
        self.build();self.assertEqual((self.dest/'scripts/init-top/udev').read_text(),self.original)
        self.assertFalse((self.dest/'module-copied').exists())
    def test_enabled_image_contains_module_config_and_unmodified_vendor_script(self):
        self.build(True)
        self.assertEqual((self.dest/'conf/pci-pf-bar-rebalance').read_text(),'0000:00:03.1\n')
        self.assertEqual((self.dest/'module-copied').read_text().strip(),'pci_pf_bar_recover')
        self.assertEqual((self.dest/'lib/pci-pf-bar-recover/udev.original').read_text(),self.original)
        self.assertEqual((self.dest/'lib/pci-pf-bar-recover/run-early').read_bytes(),(BASE/'lib/run-early').read_bytes())
    def test_prebuilt_skips_other_abi_without_wrapping_udev(self):
        (self.root/'prebuilt-kernels').write_text('7.0.0-28-generic\n')
        self.build(True)
        self.assertEqual((self.dest/'scripts/init-top/udev').read_text(),self.original)
        self.assertFalse((self.dest/'module-copied').exists())
    def test_prebuilt_matching_abi_keeps_optin_behavior(self):
        (self.root/'prebuilt-kernels').write_text('test-kernel\n')
        self.build(True)
        self.assertTrue((self.dest/'module-copied').exists())
    def test_wrapper_orders_action_before_udev_even_when_action_fails(self):
        self.build(True)
        action=self.dest/'lib/pci-pf-bar-recover/run-early';action.write_text('#!/bin/sh\necho rebalance >> "$ORDER_LOG"\nexit 1\n');action.chmod(0o755)
        wrapper=(self.dest/'scripts/init-top/udev').read_text().replace('/lib/pci-pf-bar-recover/',str(self.dest/'lib/pci-pf-bar-recover')+'/')
        p=self.root/'wrapper';p.write_text(wrapper)
        subprocess.run(['/bin/sh',str(p)],env=self.env,check=True)
        self.assertEqual(self.log.read_text().splitlines(),['rebalance','udev'])
        output=subprocess.check_output(['/bin/sh',str(p),'prereqs'],env=self.env,text=True)
        self.assertEqual(output.strip(),'dep')
        self.assertEqual(self.log.read_text().splitlines(),['rebalance','udev'])
    def run_early(self,cmdline='',fail=False,ensure_id=None):
        proc=self.root/'cmdline';proc.write_text(cmdline)
        cfg=self.root/'ports';cfg.write_text('' if ensure_id else '0000:00:03.1,0000:00:04.0\n')
        identity=self.root/'ensure-id'
        if ensure_id:identity.write_text(ensure_id+'\n')
        mock=self.root/'bin/insmod';mock.write_text('#!/bin/sh\necho "insmod:$*" >> "$ORDER_LOG"\nexit '+('1' if fail else '0')+'\n');mock.chmod(0o755)
        rm=self.root/'bin/rmmod';rm.write_text('#!/bin/sh\necho rmmod >> "$ORDER_LOG"\n');rm.chmod(0o755)
        text=(BASE/'lib/run-early').read_text().replace('/proc/cmdline',str(proc)).replace('/conf/pci-pf-bar-rebalance',str(cfg)).replace('/conf/pci-pf-bar-ensure-id',str(identity)).replace('/sys/module/pci_pf_bar_recover',str(self.root/'absent-module'))
        p=self.root/'early';p.write_text(text)
        subprocess.run(['/bin/sh',str(p)],env=self.env,check=True,capture_output=True)
    def test_early_id_passed_as_one_module_argument(self):
        self.run_early(ensure_id='10de:20f1')
        self.assertEqual(self.log.read_text().splitlines(),['insmod:/lib/pci-pf-bar-recover/pci_pf_bar_recover.ko execute=1 ensure_id=10de:20f1','rmmod'])
    def test_invalid_early_id_never_executes(self):
        self.run_early(ensure_id='10de:*')
        self.assertFalse(self.log.exists())
    def test_kernel_bypass_does_not_load_module(self):
        self.run_early('quiet pci_pf_bar_rebalance=off')
        self.assertFalse(self.log.exists())
    def test_runtime_passes_explicit_optin_and_port_list(self):
        self.run_early()
        self.assertEqual(self.log.read_text().splitlines(),['insmod:/lib/pci-pf-bar-recover/pci_pf_bar_recover.ko execute=1 ports=0000:00:03.1,0000:00:04.0','rmmod'])
    def test_runtime_failure_returns_control_to_boot(self):
        self.run_early(fail=True)
        self.assertEqual(len(self.log.read_text().splitlines()),1)

if __name__=='__main__':unittest.main()
