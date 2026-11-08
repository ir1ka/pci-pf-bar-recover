#!/usr/bin/env python3
"""Export a clean Git tree plus pinned submodules as a reproducible source archive."""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import tarfile


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args])


def collect(root, prefix='', revisions=None):
    if git(root, 'status', '--porcelain', '--untracked-files=normal').strip():
        raise ValueError(f'Dirty checkout: {root}; commit or remove pending files first')
    revision = git(root, 'rev-parse', 'HEAD').decode().strip()
    revisions[prefix or '.'] = revision
    result = {}
    entries = git(root, 'ls-tree', '-rz', '--full-tree', 'HEAD').split(b'\0')
    for entry in entries:
        if not entry:
            continue
        metadata, raw_name = entry.split(b'\t', 1)
        mode, kind, oid = metadata.decode().split()
        name = raw_name.decode()
        path = prefix + name
        if kind == 'commit':
            child = root / name
            if not (child / '.git').exists():
                raise ValueError(f'Initialize pinned submodule: {path}')
            if git(child, 'rev-parse', 'HEAD').decode().strip() != oid:
                raise ValueError(f'Submodule revision mismatch: {path}')
            result.update(collect(child, path + '/', revisions))
        elif kind == 'blob':
            data = git(root, 'cat-file', 'blob', oid)
            result[path] = (mode, data)
        else:
            raise ValueError(f'Unsupported Git entry: {path}')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9.+~_-]*', args.version):
        parser.error('Invalid archive version')
    root = Path(__file__).resolve().parents[1]
    project = json.loads((root / 'release.json').read_text())['name']
    revisions = {}
    files = collect(root, revisions=revisions)
    epoch = int(git(root, 'show', '-s', '--format=%ct', 'HEAD'))
    manifest = {'project': project, 'version': args.version, 'source_date_epoch': epoch,
                'revisions': revisions, 'files': {n: hashlib.sha256(v[1]).hexdigest()
                                                for n, v in sorted(files.items())}}
    files['SOURCE-MANIFEST.json'] = ('100644', (json.dumps(manifest, indent=2) + '\n').encode())
    args.output.mkdir(parents=True, exist_ok=True)
    prefix = project + '-' + args.version
    output = args.output / (prefix + '.tar.gz')
    # Never overwrite a prior release artifact.
    with output.open('xb') as raw:
        with gzip.GzipFile(filename='', mode='wb', fileobj=raw, mtime=epoch) as zipped:
            with tarfile.open(fileobj=zipped, mode='w', format=tarfile.PAX_FORMAT) as archive:
                for name, (mode, data) in sorted(files.items()):
                    info = tarfile.TarInfo(prefix + '/' + name)
                    info.mtime = epoch
                    info.mode = 0o755 if mode == '100755' else 0o644
                    if mode == '120000':
                        info.type = tarfile.SYMTYPE
                        info.linkname = data.decode()
                        archive.addfile(info)
                    else:
                        info.size = len(data)
                        archive.addfile(info, io.BytesIO(data))
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_name(output.name + '.sha256').write_text(digest + '  ' + output.name + '\n')
    print(output)


if __name__ == '__main__':
    main()
