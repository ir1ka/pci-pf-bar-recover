#!/usr/bin/env python3
"""Boot a diskless test VM; never pass any host PCI hardware through."""
import subprocess
import os
import sys
from pathlib import Path

base = Path(__file__).resolve().parent
kernel, mode = sys.argv[1:]
append = 'console=ttyS0,115200 rdinit=/init panic=1 loglevel=4 pci=pcie_scan_all' + ' ' + os.environ.get('PFBAR_VM_EXTRA_CMDLINE', '')
if mode == 'policy':
    append += ' pci_sriov_off_under=0000:00:03.1'
elif mode != 'control':
    raise SystemExit('mode must be policy or control')
cmd = ['qemu-system-x86_64', '-machine', 'q35,accel=kvm', '-cpu', 'host',
       '-m', '1024', '-smp', '2', '-nodefaults', '-no-reboot', '-display', 'none',
       '-serial', 'stdio', '-monitor', 'none', '-kernel', kernel,
       '-initrd', str(base/'guest-initramfs.gz'), '-append', append,
       '-device', 'pcie-root-port,id=dummy,bus=pcie.0,addr=3.0,multifunction=on,chassis=4',
       '-device', 'pcie-root-port,id=blocked,bus=pcie.0,addr=3.1,chassis=5',
       '-device', 'x3130-upstream,id=sw1,bus=blocked,addr=0',
       '-device', 'xio3130-downstream,id=near,bus=sw1,addr=1,chassis=1',
       '-device', 'xio3130-downstream,id=deep,bus=sw1,addr=2,chassis=2',
       '-device', 'x3130-upstream,id=sw2,bus=deep,addr=0',
       '-device', 'xio3130-downstream,id=leaf,bus=sw2,addr=1,chassis=3',
       '-device', 'pcie-root-port,id=outside,bus=pcie.0,addr=4,chassis=6',
       '-device', 'pcie-root-port,id=empty,bus=pcie.0,addr=5,chassis=7',
       '-device', 'igb,id=pf-near,bus=near,addr=0,netdev=n0,rombar=0',
       '-device', 'igb,id=pf-deep,bus=leaf,addr=0,netdev=n1,rombar=0',
       '-device', 'igb,id=pf-outside,bus=outside,addr=0,netdev=n2,rombar=0']
cmd += ['-device', 'pcie-root-port,id=rebar,bus=pcie.0,addr=6,chassis=8',
        '-device', 'pci-testdev,id=resize,bus=rebar,addr=0,membar=64M']
cmd += ['-device','igb,id=protected,bus=dummy,addr=0,netdev=n3,rombar=0', '-device','pci-testdev,id=wide,bus=near,addr=1,membar=64M']
for i in range(4):
    cmd += ['-netdev', f'user,id=n{i},restrict=on']
with (base/'serial-live.log').open('w+') as serial:
    result = subprocess.run(cmd, stdout=serial, stderr=subprocess.STDOUT, timeout=60)
    serial.seek(0)
    output = serial.read()
print(output, end='')
if result.returncode or 'GENERIC-TEST PASS' not in output:
    raise SystemExit(1)
