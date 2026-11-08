import importlib.util,os,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('file_guard',ROOT/'tools/file_guard.py');guard=importlib.util.module_from_spec(spec);spec.loader.exec_module(guard)
class FileOwnership(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup);self.root=Path(self.temp.name)
  self.stage=self.root/'stage';(self.stage/'usr/bin').mkdir(parents=True)
  (self.stage/'usr/bin/fixture').write_text('new package bytes')
  self.target=self.root/'existing';self.fake=self.root/'fake';self.fake.mkdir()
  q=self.fake/'dpkg-query';q.write_text('#!/bin/sh\nif [ "$1" = -W ]; then echo installed; elif [ "$1" = -S ]; then exit 1; else printf "%s\\n" "${OWNED:-}"; fi\n');q.chmod(0o755)
 def run_guard(self,owned=False,action='install',modules=()):
  script=guard.text(self.stage,['own-package','declared-conflict'],modules).replace('/usr/bin/fixture',str(self.target)).replace('/var/lib/dkms/',str(self.root/'dkms')+'/')
  return subprocess.run(['sh','-s','--',action],input=script,text=True,capture_output=True,env=dict(os.environ,PATH=str(self.fake)+':'+os.environ['PATH'],OWNED=str(self.target)if owned else''))
 def test_unowned_file_preserved_and_install_refused(self):
  self.target.write_text('administrator content');r=self.run_guard()
  self.assertNotEqual(r.returncode,0);self.assertEqual(self.target.read_text(),'administrator content')
 def test_owned_or_declared_replacement_allowed_without_guard_mutation(self):
  self.target.write_text('old package bytes');self.assertEqual(self.run_guard(True).returncode,0)
  self.assertEqual(self.target.read_text(),'old package bytes')
 def test_absent_new_file_allowed(self):self.assertEqual(self.run_guard().returncode,0)
 def test_unowned_symlink_preserved(self):
  self.target.symlink_to(self.root/'administrator-target');self.assertNotEqual(self.run_guard().returncode,0)
  self.assertTrue(self.target.is_symlink())
 def test_abort_does_not_inspect_or_modify(self):
  self.target.write_text('administrator');self.assertEqual(self.run_guard(action='abort-upgrade').returncode,0)
 def test_etc_files_left_to_dpkg_conffile_semantics(self):
  (self.stage/'etc').mkdir();(self.stage/'etc/example.conf').write_text('config')
  self.assertNotIn('/etc/example.conf',guard.text(self.stage,['own-package']))
 def test_manual_dkms_source_refused_without_deletion(self):
  source=self.root/'manual';source.mkdir();(source/'dkms.conf').write_text('PACKAGE_NAME="fixture"')
  link=self.root/'dkms/fixture/1/source';link.parent.mkdir(parents=True);link.symlink_to(source)
  self.assertNotEqual(self.run_guard(modules=['fixture']).returncode,0)
  self.assertEqual((source/'dkms.conf').read_text(),'PACKAGE_NAME="fixture"')

 def test_incomplete_dkms_registration_is_preserved_and_refused(self):
  source=self.root/'manual';source.mkdir()
  link=self.root/'dkms/fixture/1/source';link.parent.mkdir(parents=True);link.symlink_to(source)
  self.assertNotEqual(self.run_guard(modules=['fixture']).returncode,0)
  self.assertTrue(link.is_symlink())

 def test_generation_is_idempotent_and_preserves_original_preinst(self):
  import sys
  debian=self.root/'debian';stage=debian/'own-package/usr/bin';stage.mkdir(parents=True)
  (stage/'fixture').write_text('code')
  (debian/'control').write_text('Package: own-package\nArchitecture: all\nConflicts: declared-conflict\n')
  preinst=debian/'own-package.preinst';preinst.write_text('#!/bin/sh\nset -eu\n: original-native-check\n')
  cmd=[sys.executable,str(ROOT/'tools/file_guard.py'),'--debian',str(debian)]
  subprocess.run(cmd,check=True);first=preinst.read_bytes();subprocess.run(cmd,check=True)
  self.assertEqual(first,preinst.read_bytes())
  self.assertIn(b'original-native-check',first);self.assertEqual(first.count(b'#DEBHELPER#'),1)

 def test_versioned_replaces_allows_only_declared_old_versions(self):
  query=self.fake/'dpkg-query'
  query.write_text('''#!/bin/sh
if [ "$1" = -W ]; then
 case "$2" in '-f=${Version}') echo "${OLD_VERSION}";; *) echo installed;; esac
elif [ "$2" = old-provider ]; then printf '%s\\n' "$OWNED"; fi
''');query.chmod(0o755)
  self.target.write_text('old provider code')
  script=guard.text(self.stage,['own-package'],replacements=[('old-provider','lt','2')]).replace('/usr/bin/fixture',str(self.target))
  def run(version):return subprocess.run(['sh','-s','--','upgrade'],input=script,text=True,capture_output=True,env=dict(os.environ,PATH=str(self.fake)+':'+os.environ['PATH'],OWNED=str(self.target),OLD_VERSION=version))
  self.assertEqual(run('1').returncode,0)
  self.assertNotEqual(run('3').returncode,0)
  self.assertEqual(self.target.read_text(),'old provider code')

 def module_guard(self,registered=False,foreign_binary_owner=False,redirected=False):
  module=self.root/'modules/k/updates/dkms/fixture.ko';module.parent.mkdir(parents=True)
  if redirected:
   target=self.root/'administrator-module';target.write_text('existing module');module.symlink_to(target)
  else:module.write_text('existing module')
  owned=''
  if registered:
   source=self.root/'provider';source.mkdir();(source/'dkms.conf').write_text('PACKAGE_NAME="fixture"')
   link=self.root/'dkms/fixture/1/source';link.parent.mkdir(parents=True);link.symlink_to(source)
   built=link.parent/'k/x86_64/module/fixture.ko';built.parent.mkdir(parents=True);built.write_text('registered build')
   owned=str(source/'dkms.conf')
  if foreign_binary_owner:
   query=self.fake/'dpkg-query';query.write_text('#!/bin/sh\nif [ "$1" = -W ];then echo installed;elif [ "$1" = -S ];then echo "unrelated: module";else printf "%s\\n" "$OWNED";fi\n');query.chmod(0o755)
  script=guard.text(self.stage,['own-package'],modules=['fixture'],installed_modules=['fixture']).replace('/usr/bin/fixture',str(self.target)).replace('/var/lib/dkms/',str(self.root/'dkms')+'/').replace('/lib/modules/',str(self.root/'modules')+'/')
  result=subprocess.run(['sh','-s','--','install'],input=script,text=True,capture_output=True,env=dict(os.environ,PATH=str(self.fake)+':'+os.environ['PATH'],OWNED=owned))
  self.assertEqual(module.read_text(),'existing module')
  return result
 def test_manual_module_binary_is_not_overwritten(self):self.assertNotEqual(self.module_guard().returncode,0)
 def test_known_dkms_generated_module_slot_is_allowed(self):
  r=self.module_guard(registered=True);self.assertEqual(r.returncode,0,r.stderr)
 def test_other_package_module_binary_is_refused_even_with_known_dkms(self):self.assertNotEqual(self.module_guard(registered=True,foreign_binary_owner=True).returncode,0)

 def test_redirected_module_slot_refused_even_for_known_provider(self):
  self.assertNotEqual(self.module_guard(registered=True,redirected=True).returncode,0)
  self.assertTrue((self.root/'modules/k/updates/dkms/fixture.ko').is_symlink())
