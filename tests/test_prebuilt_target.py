#!/usr/bin/python3
"""Reject guessed target metadata; infer ABI independently of the running host."""
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest
import tempfile
from unittest.mock import patch
spec=importlib.util.spec_from_file_location('prebuilt',Path(__file__).resolve().parents[1]/'tools/build_prebuilt_deb.py')
app=importlib.util.module_from_spec(spec);spec.loader.exec_module(app)
class TargetTests(unittest.TestCase):
    def test_module_abi_and_matching_headers_are_used(self):
        def query(cmd,**kwargs):
            return SimpleNamespace(returncode=0,stdout='installed\t12.34.56') if cmd[-1]=='linux-headers-12.34-test' else SimpleNamespace(returncode=1,stdout='')
        with patch.object(app.subprocess,'check_output',return_value='12.34-test SMP'),patch.object(app.subprocess,'run',side_effect=query):
            self.assertEqual(app.resolve_target(Path('module.ko')),('12.34-test','12.34.56'))
    def test_explicit_wrong_abi_is_rejected_before_metadata_lookup(self):
        with patch.object(app.subprocess,'check_output',return_value='12.34-test SMP'),patch.object(app.subprocess,'run')as query:
            with self.assertRaisesRegex(ValueError,'ABI mismatch'):app.resolve_target(Path('module.ko'),'99.0-wrong','1.0')
            query.assert_not_called()
    def test_missing_metadata_is_not_guessed(self):
        with patch.object(app.subprocess,'check_output',return_value='12.34-test SMP'),patch.object(app.subprocess,'run',return_value=SimpleNamespace(returncode=1,stdout='')):
            with self.assertRaisesRegex(ValueError,'No matching'):app.resolve_target(Path('module.ko'))
    def test_explicit_version_supports_offline_cross_build(self):
        with patch.object(app.subprocess,'check_output',return_value='12.34-test SMP'),patch.object(app.subprocess,'run')as query:
            self.assertEqual(app.resolve_target(Path('module.ko'),package_version='12.34.56'),('12.34-test','12.34.56'))
            query.assert_not_called()
class HeaderBuildTests(unittest.TestCase):
    def headers(self,root):
        path=root/'headers';(path/'include/generated').mkdir(parents=True)
        (path/'Makefile').write_text('# prepared target headers\n')
        (path/'include/generated/utsrelease.h').write_text('#define UTS_RELEASE "12.34-target"\n')
        return path
    def test_header_abi_is_read_without_running_commands(self):
        with tempfile.TemporaryDirectory()as tmp,patch.object(app.subprocess,'run')as run:
            self.assertEqual(app.header_release(self.headers(Path(tmp))),'12.34-target')
            run.assert_not_called()
    def test_header_mismatch_fails_before_build(self):
        with tempfile.TemporaryDirectory()as tmp,patch.object(app.subprocess,'run')as run:
            a=SimpleNamespace(headers=self.headers(Path(tmp)),kernel='99-wrong')
            with self.assertRaisesRegex(ValueError,'does not match'):app.build_module(a,Path(tmp)/'build')
            run.assert_not_called()
    def test_cross_build_forwards_target_paths_and_toolchain(self):
        with tempfile.TemporaryDirectory()as tmp,patch.object(app.subprocess,'run')as run,patch.object(app.subprocess,'check_output',return_value='1234'):
            root=Path(tmp);work=root/'work';work.mkdir()
            a=SimpleNamespace(headers=self.headers(root),kernel=None,cc='target-gcc',arch='x86',cross_compile='target-')
            module,kernel=app.build_module(a,work)
            self.assertEqual(kernel,'12.34-target')
            cmd=run.call_args.args[0]
            for value in ('KDIR='+str(a.headers),'KERNEL_RELEASE=12.34-target','CC=target-gcc','ARCH=x86','CROSS_COMPILE=target-'):self.assertIn(value,cmd)
            self.assertNotIn('uname',' '.join(cmd))
            self.assertEqual(module,work/'module/pci_pf_bar_recover.ko')
    def test_unprepared_headers_are_rejected(self):
        with tempfile.TemporaryDirectory()as tmp:
            root=Path(tmp);(root/'Makefile').write_text('# raw source\n')
            with self.assertRaisesRegex(ValueError,'prepared target headers'):app.header_release(root)
class BinaryVersionTests(unittest.TestCase):
    def test_new_kernel_is_a_real_package_upgrade(self):
        old=app.binary_version('3.2.1','9.1.2-4-generic')
        new=app.binary_version('3.2.1','9.1.2-5-generic')
        app.subprocess.run(['dpkg','--compare-versions',new,'gt',old],check=True)
        app.subprocess.run(['dpkg','--compare-versions',new,'gt','3.2.1'],check=True)
if __name__=='__main__':unittest.main()

class RevisionOrdering(unittest.TestCase):
 def test_binary_revision_upgrades_same_abi(self):
  import subprocess
  new=app.binary_version('2.5.1-1','7.0.0-30-generic')
  self.assertEqual(new,'2.5.1+k7.0.0.30.generic-1')
  self.assertEqual(subprocess.run(['dpkg','--compare-versions',new,'gt',app.binary_version('2.5.1','7.0.0-30-generic')]).returncode,0)
