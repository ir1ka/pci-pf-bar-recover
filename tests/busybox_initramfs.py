#!/usr/bin/python3
"""Root-only disposable chroot; real BusyBox shell/applets, no module insertion.

The loader invokes real BusyBox modprobe -D against an isolated dependency tree.
Synthetic sysfs records test userspace ordering/error paths, not kernel recovery.
"""
import json,os,shutil,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
BUSYBOX=shutil.which('busybox')

def check():
 assert os.geteuid()==0,'Run only in a disposable root container'
 assert BUSYBOX,'busybox-static required'
 cases=[]
 with tempfile.TemporaryDirectory(prefix='busybox-initramfs-')as temp:
  base=Path(temp)
  def run(name,ports='0000:00:03.1',bypass=False,enabled=True,identity='',loaded='',failure=False,partial=False,snapshot=True,included=''):
   root=base/name;root.mkdir()
   for d in ('bin','sbin','dev','proc','sys/module','conf','run','etc/modprobe.d','scripts/init-top',LIB):
    (root/d).mkdir(parents=True,exist_ok=True)
   shutil.copyfile(BUSYBOX,root/'bin/busybox');(root/'bin/busybox').chmod(0o755)
   for app in ('sh','cat','grep','sed','mkdir','chmod','rm'):
    (root/'bin'/app).symlink_to('busybox')
   (root/'dev/null').touch()  # regular file suffices for redirected diagnostics
   k=os.uname().release;mods=root/'lib/modules'/k;mods.mkdir(parents=True)
   (mods/'modules.dep').write_text(MODULE+'.ko:\n');(mods/(MODULE+'.ko')).touch()
   (root/'proc/cmdline').write_text(BYPASS if bypass else '')
   module=root/'sys/module'/MODULE;params=module/'parameters'
   def script(path,text):
    p=root/path;p.write_text('#!/bin/sh\n'+text+'\n');p.chmod(0o755)
   script('sbin/insmod','echo "load:$*" >> /trace\n[ "$1" = /lib/pci-pf-bar-recover/pci_pf_bar_recover.ko ] || exit 2\nshift\n/bin/busybox modprobe -D pci_pf_bar_recover "$@" >> /dry-run || exit 2\n[ ! -f /fail ] || exit 1\nmkdir -p /sys/module/'+MODULE+'/parameters\n'+AFTER_LOAD)
   script('sbin/rmmod','echo "unload:$*" >> /trace; rm -rf /sys/module/'+MODULE)
   if failure:(root/'fail').touch()
   if partial:(root/'partial').touch()
   if not snapshot:(root/'no-snapshot').touch()
   if loaded:
    params.mkdir(parents=True);(params/'execute').write_text(loaded)
   setup(root,enabled,ports,identity,included)
   (root/LIB/'run-early').write_text((ROOT/'lib/run-early').read_text().replace('if insmod ', 'if /sbin/insmod ').replace('if ! insmod ', 'if ! /sbin/insmod ').replace('rmmod '+MODULE, '/sbin/rmmod '+MODULE));(root/LIB/'run-early').chmod(0o755)
   shutil.copyfile(ROOT/'lib/early-udev-wrapper',root/'scripts/init-top/udev');(root/'scripts/init-top/udev').chmod(0o755)
   script(LIB+'/udev.original','case "${1:-}" in prereqs) echo vendor-dependency; exit 0;; esac; echo "udev:$*" >> /trace')
   env={'PATH':'/bin:/sbin','LC_ALL':'C'}
   prereqs=subprocess.run(['chroot',str(root),'/bin/sh','/scripts/init-top/udev','prereqs'],env=dict(os.environ,**env),capture_output=True,text=True)
   assert prereqs.returncode==0 and prereqs.stdout.strip()=='vendor-dependency'
   assert not (root/'trace').exists(),'prereqs triggered execution'
   p=subprocess.run(['chroot',str(root),'/bin/sh','/scripts/init-top/udev','start'],env=dict(os.environ,**env),capture_output=True,text=True)
   assert p.returncode==0,(name,p.stdout,p.stderr)
   trace=(root/'trace').read_text().splitlines()
   assert trace[-1]=='udev:start',(name,trace)
   assert 'not found'not in p.stderr and 'unrecognized option'not in p.stderr,(name,p.stderr)
   cases.append({'case':name,'trace':trace,'stderr':p.stderr,'dry_run':(root/'dry-run').read_text()if(root/'dry-run').exists()else''})
   return root,trace
  exercise(run)
 print(json.dumps({'project':PROJECT,'busybox':subprocess.check_output([BUSYBOX,'sh','-c','busybox | head -1'],text=True).strip(),'real_busybox_modprobe_dependency_dry_run':True,'module_insertion':False,'cases':cases},indent=2))

PROJECT='pci-pf-bar-recover'
MODULE='pci_pf_bar_recover';LIB='lib/pci-pf-bar-recover';BYPASS='pci_pf_bar_rebalance=off'
AFTER_LOAD="printf '{\"restored\":1}\\n' > /sys/module/pci_pf_bar_recover/parameters/result_records; echo 0 > /sys/module/pci_pf_bar_recover/parameters/result_dropped"
def setup(root,enabled,ports,identity,included):
 if enabled:
  (root/'conf/pci-pf-bar-rebalance').write_text(ports)
  if identity:(root/'conf/pci-pf-bar-ensure-id').write_text(identity)
def exercise(run):
 root,trace=run('ports')
 assert trace==['load:/lib/pci-pf-bar-recover/pci_pf_bar_recover.ko execute=1 ports=0000:00:03.1','unload:pci_pf_bar_recover','udev:start']
 assert (root/'run/pci-pf-bar-recover/boot-records.jsonl').is_file()
 assert (root/'run/pci-pf-bar-recover/boot-records.jsonl').stat().st_mode&0o777==0o600
 _,trace=run('ensure-id',ports='',identity='10de:20f1');assert 'ensure_id=10de:20f1'in trace[0]
 for name,kwargs in [('disabled',{'enabled':False}),('bypass',{'bypass':True}),('invalid-id',{'ports':'','identity':'10de:*'}),('active',{'loaded':'Y'})]:
  _,trace=run(name,**kwargs);assert trace==['udev:start']
 _,trace=run('idle',loaded='N');assert trace[0]=='unload:pci_pf_bar_recover'and trace[1].startswith('load:'),trace
 _,trace=run('load-failure',failure=True);assert len(trace)==2 and trace[0].startswith('load:')

if __name__=='__main__':check()
