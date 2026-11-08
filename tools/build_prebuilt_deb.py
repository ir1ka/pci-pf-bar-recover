#!/usr/bin/python3
"""Build a source-free, exact-ABI binary deb. Does not install or load anything."""
import argparse
from datetime import datetime, timezone
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]

def find_tool(name):
    path=shutil.which(name)
    if path:return path
    for base in ('/usr/sbin','/sbin'):
        path=Path(base)/name
        if path.is_file() and os.access(path,os.X_OK):return str(path)
    raise ValueError('Required build tool missing: '+name)

def resolve_target(module, kernel=None, package_version=None):
    """Use target metadata, never the build host's running kernel or board."""
    module_abi=subprocess.check_output([find_tool('modinfo'),'-F','vermagic',str(module)],text=True).split()[0]
    if kernel is None:kernel=module_abi
    if kernel!=module_abi:raise ValueError('Module ABI mismatch')
    if package_version is None:
        for name in ('linux-image-'+kernel,'linux-image-unsigned-'+kernel,'linux-headers-'+kernel):
            r=subprocess.run(['dpkg-query','-W','-f=${db:Status-Status}\t${Version}',name],capture_output=True,text=True)
            if r.returncode==0 and r.stdout.startswith('installed\t'):
                package_version=r.stdout.split('\t',1)[1];break
        if package_version is None:raise ValueError('No matching installed image/headers; supply --kernel-package-version')
    return kernel,package_version

def header_release(headers):
    headers=headers.resolve()
    if not (headers/'Makefile').is_file():raise ValueError('Target headers need a prepared kernel Makefile')
    uts=headers/'include/generated/utsrelease.h'
    if not uts.is_file():raise ValueError('Target headers lack include/generated/utsrelease.h; use prepared target headers')
    match=re.search(r'^#define\s+UTS_RELEASE\s+"([^"\n]+)"',uts.read_text(),re.M)
    if not match:raise ValueError('Cannot identify ABI from target headers')
    return match.group(1)

def build_module(a, work):
    headers=a.headers.resolve()
    kernel=header_release(headers)
    if a.kernel is not None and a.kernel!=kernel:raise ValueError('Requested kernel does not match target headers')
    module_dir=Path(work)/'module';module_dir.mkdir()
    for source in ('pci_pf_bar_recover.c','rebalance.inc','bar_policy.inc','Makefile'):shutil.copy2(ROOT/'src'/source,module_dir/source)
    epoch=int(subprocess.check_output(['dpkg-parsechangelog','-l',str(ROOT/'debian/changelog'),'-S','Timestamp'],text=True).strip())
    env=dict(os.environ,SOURCE_DATE_EPOCH=str(epoch))
    env.setdefault('KBUILD_BUILD_USER','builder');env.setdefault('KBUILD_BUILD_HOST','pfbar-builder')
    env.setdefault('KBUILD_BUILD_TIMESTAMP',datetime.fromtimestamp(epoch,timezone.utc).strftime('%a %b %d %H:%M:%S UTC %Y'))
    env.setdefault('KBUILD_BUILD_VERSION','1')
    cmd=['make','-C',str(module_dir),'KDIR='+str(headers),'KERNEL_RELEASE='+kernel]
    for name,value in (('CC',a.cc),('ARCH',a.arch),('CROSS_COMPILE',a.cross_compile)):
        if value:cmd.append(name+'='+value)
    subprocess.run(cmd,env=env,check=True)
    return module_dir/'pci_pf_bar_recover.ko',kernel

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kernel',help='Target ABI; validated against headers/module, never running uname')
    p.add_argument('--kernel-package-version',help='Target image package version; supply explicitly for offline builds')
    mode=p.add_mutually_exclusive_group(required=True)
    mode.add_argument('--module',type=Path,help='Package an existing exact-ABI module without compiling')
    mode.add_argument('--headers',type=Path,help='Build in an isolated directory using prepared target headers')
    p.add_argument('--cc');p.add_argument('--arch');p.add_argument('--cross-compile');p.add_argument('--strip',default='strip')
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    if a.headers:
        with tempfile.TemporaryDirectory(prefix='pfbar-target-build-')as work:
            try:a.module,a.kernel=build_module(a,work)
            except ValueError as error:p.error(str(error))
            package(a,p)
    else:package(a,p)

def binary_version(source_version, kernel):
    # Distinct ABI builds must not look like an already-installed same version.
    upstream,sep,revision=source_version.rpartition('-')
    suffix='+k'+kernel.replace('-','.').replace('+','.')
    return upstream+suffix+'-'+revision if sep else source_version+suffix

def package(a,p):
    module=a.module.resolve()
    try:a.kernel,a.kernel_package_version=resolve_target(module,a.kernel,a.kernel_package_version)
    except ValueError as error:p.error(str(error))
    if not re.fullmatch(r'[0-9][0-9A-Za-z.+~-]*',a.kernel) or not re.fullmatch(r'[0-9][0-9A-Za-z.+:~\-]*',a.kernel_package_version):p.error('Invalid kernel/version')
    def field(n):return subprocess.check_output(['dpkg-parsechangelog','-l',str(ROOT/'debian/changelog'),'-S',n],text=True).strip()
    version=binary_version(field('Version'),a.kernel);epoch=int(field('Timestamp'))
    header=module.read_bytes()[:20]
    if header[:6]!=b'\x7fELF\x02\x01' or int.from_bytes(header[18:20],'little')!=62:raise SystemExit('Expected uncompressed x86-64 ELF module')
    a.output.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='prebuilt-')as tmp:
        stage=Path(tmp)/'package';stage.mkdir()
        def copy(src,dst,mode):
            target=stage/dst;target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,target);target.chmod(mode)
        copy(module,f'lib/modules/{a.kernel}/updates/pci-pf-bar-recover/pci_pf_bar_recover.ko',0o644)
        subprocess.run([a.strip,'--strip-debug',str(stage/f'lib/modules/{a.kernel}/updates/pci-pf-bar-recover/pci_pf_bar_recover.ko')],check=True)
        for name in ('pci-pf-bar-recover','pci-pf-bar-boot','pci-pf-bar-power'):copy(ROOT/'bin'/name,'usr/sbin/'+name,0o755)
        copy(ROOT/'initramfs/pci-pf-bar-rebalance','usr/share/initramfs-tools/hooks/pci-pf-bar-rebalance',0o755)
        copy(ROOT/'lib/initramfs-runtime','usr/share/pci-pf-bar-recover/initramfs-runtime',0o644)
        for name in ('early-udev-wrapper','run-early'):copy(ROOT/'lib'/name,'usr/share/pci-pf-bar-recover/'+name,0o755)
        (stage/'usr/share/pci-pf-bar-recover/prebuilt-kernels').write_text(a.kernel+'\n')
        for src,name in [('README.md','README.md'),('README.Debian','README.Debian'),('LICENSE','COPYING'),('debian/copyright','copyright')]:
            copy(ROOT/src,'usr/share/doc/pci-pf-bar-recover-prebuilt/'+name,0o644)
        control=stage/'DEBIAN';control.mkdir()
        from file_guard import text as ownership_guard
        guard=ownership_guard(stage,['pci-pf-bar-recover-prebuilt','pci-pf-bar-recover-dkms'],['pci-pf-bar-recover'])
        (control/'preinst').write_text('#!/bin/sh\nset -e\n'+guard)
        (control/'preinst').chmod(0o755)
        image=a.kernel_package_version
        (control/'control').write_text(f'''Package: pci-pf-bar-recover-prebuilt
Version: {version}
Architecture: amd64
Maintainer: IriKa Qiu <qiujie.jq@gmail.com>
Homepage: https://github.com/ir1ka/pci-pf-bar-recover
Section: kernel
Priority: optional
Depends: python3, kmod, xz-utils, zstd, gzip, coreutils, sed, grep, findutils, gawk | mawk, linux-image-{a.kernel} (= {image}) | linux-image-unsigned-{a.kernel} (= {image})
Recommends: initramfs-tools
Conflicts: pci-pf-bar-recover-dkms
Replaces: pci-pf-bar-recover-dkms
Description: precompiled physical PCI BAR recovery for Linux {a.kernel}
 Exact-ABI recovery module and explicit wrappers, without kernel sources or DKMS.
 Installation does not load the module or enable early recovery.
 Other kernel ABIs need a separately compiled package. No kernel is rebuilt.
''')
        scripts={
          'postinst':f'''#!/bin/sh
set -e
if [ "${{1:-}}" = configure ]; then
    depmod -a {a.kernel}
    if [ -f /etc/initramfs-tools/conf.d/pci-pf-bar-rebalance ]; then
        update-initramfs -u -k all
    fi
fi
''',
          'prerm':'''#!/bin/sh
set -e
if [ "${1:-}" = remove ] && { [ -f /etc/initramfs-tools/conf.d/pci-pf-bar-rebalance ] || [ -f /var/lib/pci-pf-bar-recover/early-image-state ]; }; then
    /usr/sbin/pci-pf-bar-boot disable
fi
''',
          'postrm':f'''#!/bin/sh
set -e
if [ "${{1:-}}" = remove ] || [ "${{1:-}}" = purge ]; then
    [ ! -d /lib/modules/{a.kernel} ] || depmod -a {a.kernel}
fi
'''}
        for name,text in scripts.items():(control/name).write_text(text);(control/name).chmod(0o755)
        for f in stage.rglob('*'):os.utime(f,(epoch,epoch),follow_symlinks=False)
        os.utime(stage,(epoch,epoch))
        out=a.output/f'pci-pf-bar-recover-prebuilt_{version}_amd64.deb'
        subprocess.run(['dpkg-deb','--root-owner-group','--build',str(stage),str(out)],env=dict(os.environ,SOURCE_DATE_EPOCH=str(epoch)),check=True)
        print(out)
if __name__=='__main__':main()
