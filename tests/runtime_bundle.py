#!/usr/bin/python3
"""Test build-time decompression and real BusyBox-only module command syntax.
No kernel module is inserted. insmod is exercised with a missing module only.
"""
import os,shutil,subprocess,tempfile,json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MODULE='pci_pf_bar_recover'
assert os.geteuid()==0,'Use disposable root container'
with tempfile.TemporaryDirectory(prefix='system-runtime-')as tmp:
 base=Path(tmp);image=base/'image';(image/'dev').mkdir(parents=True);(image/'dev/null').touch();(image/'bin').mkdir(parents=True);(image/'sbin').mkdir()
 shutil.copyfile(shutil.which('busybox'),image/'bin/busybox');(image/'bin/busybox').chmod(0o755)
 for app in ('sh','cat','grep','sed','mkdir','chmod'):(image/'bin'/app).symlink_to('busybox')
 for app in ('insmod','rmmod','modprobe'):(image/'sbin'/app).symlink_to('/bin/busybox')
 before={str(p.relative_to(image)):p.readlink()if p.is_symlink()else p.read_bytes()for p in image.rglob('*')if not p.is_dir()}
 fake=base/'tools';fake.mkdir();info=fake/'modinfo';info.write_text('#!/bin/sh\ncase "$4" in depends) printf "%s" "${TEST_DEPENDS:-}";; filename) echo "$TEST_MODULE";; *) exit 1;; esac\n');info.chmod(0o755)
 data=b'signed module bytes\x00signature-preserved';plain=base/'fixture.ko';plain.write_bytes(data)
 cases=[]
 for tool,suffix in ((None,''),('xz','.xz'),('zstd','.zst'),('gzip','.gz')):
  source=plain
  if tool:
   source=base/('fixture.ko'+suffix)
   with source.open('wb')as out:subprocess.run([tool,'-c',str(plain)],stdout=out,check=True)
  env=dict(os.environ,PATH=str(fake)+':'+os.environ['PATH'],DESTDIR=str(image),version='target-not-host',TEST_MODULE=str(source))
  subprocess.run(['sh',str(ROOT/'lib/initramfs-runtime')],env=env,check=True)
  assert(image/'lib'/'pci-pf-bar-recover'/(MODULE+'.ko')).read_bytes()==data
  cases.append(suffix or '.ko')
 env['TEST_DEPENDS']='unexpected_dependency'
 assert subprocess.run(['sh',str(ROOT/'lib/initramfs-runtime')],env=env,capture_output=True).returncode!=0
 assert all((image/p).readlink()==value if(image/p).is_symlink()else(image/p).read_bytes()==value for p,value in before.items())
 assert not(image/'lib'/'pci-pf-bar-recover'/'runtime').exists()
 for app in ('insmod','rmmod'):
  p=subprocess.run(['chroot',str(image),'/sbin/'+app,'/missing.ko'if app=='insmod'else'__nonexistent_test_module__'],capture_output=True,text=True)
  assert p.returncode!=0 and 'unrecognized option'not in p.stderr
 # Run every command used by early scripts in a BusyBox-only chroot.
 subprocess.run(['chroot',str(image),'/bin/sh','-c','PATH=/sbin:/bin; for app in insmod rmmod cat grep sed mkdir chmod; do command -v "$app" >/dev/null || exit 1; done'],check=True)
 print(json.dumps(dict(project='pci-pf-bar-recover',system_busybox_only=True,no_private_tools=True,global_commands_preserved=True,decompression_cases=cases,unexpected_dependencies_refused=True,module_insertion=False),indent=2))
