#!/usr/bin/python3
"""Build from an isolated source export; never install or load host modules."""
import argparse
import fnmatch
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
DIRECTORIES=('src','bin','lib','initramfs','tests','debian','tools')
FILES=('LICENSE','README.md','README.Debian','Makefile','dkms.conf','.gitignore','.gitattributes')
PATTERNS=('*.ko','*.ko.*','*.o','*.mod','*.mod.c','.*.cmd','Module.symvers','modules.order','*.pyc','*.log')

def ignore(path,names):
    rejected=[]
    for name in names:
        if name in ('__pycache__','.tmp_versions','.debhelper') or any(fnmatch.fnmatch(name,p) for p in PATTERNS):
            rejected.append(name)
        elif Path(path).name=='debian' and (name in ('files','debhelper-build-stamp') or name.endswith(('.substvars','.debhelper')) or (name.endswith('-dkms') and (Path(path)/name).is_dir())):
            rejected.append(name)
    return rejected

def field(name):
    return subprocess.check_output(['dpkg-parsechangelog','-l',str(ROOT/'debian/changelog'),'-S',name],text=True).strip()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('kind',choices=('binary','source'))
    args=p.parse_args()
    name,version=field('Source'),field('Version')
    epoch=int(field('Timestamp'))
    build=ROOT/'build';build.mkdir(exist_ok=True)
    dist=ROOT/'dist';dist.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='debian-',dir=build) as tmp:
        work=Path(tmp);source=work/(name+'-'+version)
        source.mkdir()
        for directory in DIRECTORIES:
            if (ROOT/directory).exists():shutil.copytree(ROOT/directory,source/directory,ignore=ignore)
        for filename in FILES:
            if (ROOT/filename).exists():shutil.copy2(ROOT/filename,source/filename)
        # Git does not preserve source mtimes. Normalize the complete export,
        # so working-tree, Git checkout and source-package rebuilds agree.
        for item in sorted(source.rglob('*'),reverse=True):
            os.utime(item,(epoch,epoch),follow_symlinks=False)
        os.utime(source,(epoch,epoch))
        env=dict(os.environ,SOURCE_DATE_EPOCH=str(epoch))
        if args.kind == 'source':
            upstream = version.rsplit('-', 1)[0]
            original = work / (name + '_' + upstream + '.orig.tar.xz')
            subprocess.run(['tar', '--sort=name', '--mtime=@' + str(epoch),
                            '--owner=0', '--group=0', '--numeric-owner',
                            '--exclude=' + source.name + '/debian',
                            '-cJf', str(original), '-C', str(work), source.name], check=True)
        option='-b' if args.kind=='binary' else '-S'
        subprocess.run(['dpkg-buildpackage',option,'-us','-uc'],cwd=source,env=env,check=True)
        copied=[]
        for artifact in sorted(work.iterdir()):
            if artifact.is_file():
                target=dist/artifact.name
                shutil.copy2(artifact,target);copied.append(target)
        if not copied:raise RuntimeError('Build produced no package artifacts')
        for target in copied:print(target)
    return 0

if __name__=='__main__':raise SystemExit(main())
