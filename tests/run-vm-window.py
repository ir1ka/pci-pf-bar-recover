#!/usr/bin/python3
"""Diskless root -> switch -> two downstream ports; no PCI passthrough."""
from pathlib import Path
import subprocess,sys
base=Path(__file__).resolve().parent
cmd=['qemu-system-x86_64','-machine','q35,accel=kvm','-cpu','host','-m','1024','-smp','2','-nodefaults','-no-reboot','-display','none','-serial','stdio','-monitor','none',
 '-global','q35-pcihost.pci-hole64-size=512G','-kernel',sys.argv[1],'-initrd',str(base/'guest-initramfs.gz'),'-append','console=ttyS0,115200 rdinit=/init panic=1 loglevel=4 window_test='+sys.argv[2],
 '-device','pcie-root-port,id=r,bus=pcie.0,addr=1,chassis=1','-device','x3130-upstream,id=s,bus=r,addr=0',
 '-device','xio3130-downstream,id=a,bus=s,addr=1,chassis=2','-device','xio3130-downstream,id=b,bus=s,addr=2,chassis=3',
 '-device','pci-testdev,bus=a,addr=0,membar=64M','-device','pci-testdev,bus=b,addr=0,membar=64M']
try:
 r=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
except subprocess.TimeoutExpired as error:
 print((error.stdout or b'').decode(errors='replace')if isinstance(error.stdout,bytes)else(error.stdout or ''),end='')
 print((error.stderr or b'').decode(errors='replace')if isinstance(error.stderr,bytes)else(error.stderr or ''),end='')
 raise SystemExit('WINDOW-TEST timeout; partial guest output retained')
print(r.stdout+r.stderr,end='')
if r.returncode or 'GENERIC-TEST PASS'not in r.stdout:raise SystemExit(1)
