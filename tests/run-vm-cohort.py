#!/usr/bin/python3
"""Two target root ports plus an unrelated active SR-IOV root; QEMU only."""
from pathlib import Path
import os,subprocess,sys
base=Path(__file__).resolve().parent
cmd=['qemu-system-x86_64','-machine','q35,accel=kvm','-cpu','host','-m','1024','-smp','2',
     '-nodefaults','-no-reboot','-display','none','-serial','stdio','-monitor','none',
     '-kernel',sys.argv[1],'-initrd',str(base/'guest-initramfs.gz'),
     '-append','console=ttyS0,115200 rdinit=/init panic=1 loglevel=4 '+os.environ.get('PFBAR_VM_EXTRA_CMDLINE','')]
for i in (1,2,3):
    cmd+=['-device',f'pcie-root-port,id=r{i},bus=pcie.0,addr={i},chassis={i}']
for i in (1,2):cmd+=['-device',f'pci-testdev,id=t{i},bus=r{i},addr=0,membar=64M']
cmd+=['-device','igb,id=outside,bus=r3,addr=0,rombar=0,netdev=n0','-netdev','user,id=n0,restrict=on']
r=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=60)
output=r.stdout.decode(errors='replace');print(output,end='')
if r.returncode or 'GENERIC-TEST PASS'not in output:raise SystemExit(1)
