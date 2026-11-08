#!/usr/bin/python3
"""Generate a read-only preinst guard against unmanaged payload collisions."""
import argparse,os,re
from pathlib import Path

def text(stage,allowed,modules=(),replacements=(),installed_modules=()):
 paths=sorted('/'+str(p.relative_to(stage))for p in stage.rglob('*')if(p.is_file()or p.is_symlink())and p.relative_to(stage).parts[0]!='DEBIAN'and not str(p.relative_to(stage)).startswith('etc/'))
 if any(any(c in p for c in '\n\r\t*?[]')for p in paths):raise ValueError('Unsupported payload filename for ownership guard')
 if any(not all(c.isalnum()or c in '-+.*'for c in p)for p in allowed+list(modules)):raise ValueError('Invalid package/module name')
 modules_shell=' '.join('/var/lib/dkms/'+name+'/*/source'for name in modules)
 conditional=''
 for package,operator,version in replacements:
  if not re.fullmatch(r'[a-z0-9][a-z0-9+.-]*',package)or operator not in ('lt','le','eq','ge','gt')or not re.fullmatch(r'[0-9][A-Za-z0-9.+:~\-]*',version):raise ValueError('Invalid versioned replacement')
  conditional+=""" guard_version=$(dpkg-query -W -f='${Version}' %s 2>/dev/null || :)
 if [ -n "$guard_version" ] && dpkg --compare-versions "$guard_version" %s '%s'; then
  guard_status=$(dpkg-query -W -f='${db:Status-Status}' %s 2>/dev/null || :)
  case "$guard_status" in installed|unpacked|half-installed|half-configured|triggers-awaited|triggers-pending)
   dpkg-query -L %s >> "$guard_tmp/owned" 2>/dev/null || :;;
  esac
 fi
"""%(package,operator,version,package,package)
 if any(not re.fullmatch(r'[A-Za-z0-9_-]+',m)for m in installed_modules):raise ValueError('Invalid module filename')
 slots=''
 if installed_modules:
  slots=""" for guard_binary in %s; do
  [ -e "$guard_binary" ] || [ -L "$guard_binary" ] || continue
  if [ -L "$guard_binary" ] || [ ! -f "$guard_binary" ] || [ "$(stat -c %%h "$guard_binary")" != 1 ]; then
   printf '%%s\n' "$guard_binary" >> "$guard_tmp/conflicts"; continue
  fi
  case "$guard_binary" in *.ko|*.ko.xz|*.ko.zst|*.ko.gz) ;; *) continue;; esac
  guard_normal=$(normalize_path "$guard_binary")
  guard_query=0
  dpkg-query -S "$guard_binary" "$guard_normal" > "$guard_tmp/binary-owner" 2>/dev/null || guard_query=$?
  if [ -s "$guard_tmp/binary-owner" ]; then
   guard_owners=$(sed 's#: /.*$##' "$guard_tmp/binary-owner" | tr ',\n' '  ')
   for guard_owner in $guard_owners; do
    guard_owner=${guard_owner%%%%:*}
    case "$guard_owner" in %s) ;; *) printf '%%s\n' "$guard_binary" >> "$guard_tmp/conflicts";; esac
   done
  elif [ "$guard_query" -gt 1 ]; then
   printf '%%s\n' "$guard_binary" >> "$guard_tmp/conflicts"
  else
   guard_kernel=${guard_binary#/lib/modules/};guard_kernel=${guard_kernel%%%%/*}
   guard_base=${guard_binary##*/};guard_base=${guard_base%%%%.ko*};guard_attributed=no
   while IFS= read -r guard_registration; do
    for guard_build in "$guard_registration/$guard_kernel/"*/module/"$guard_base.ko"*; do
     [ ! -f "$guard_build" ] || guard_attributed=yes
    done
   done < "$guard_tmp/known-dkms"
   [ "$guard_attributed" = yes ] || printf '%%s\n' "$guard_binary" >> "$guard_tmp/conflicts"
  fi
 done
"""%(' '.join('/lib/modules/*/updates/dkms/'+m+'.ko*'for m in installed_modules),'|'.join(allowed))
 return '''# BEGIN project payload ownership guard
case ${1:-} in install|upgrade)
 guard_tmp=$(mktemp -d /tmp/deb-ownership.XXXXXX)
 trap 'rm -rf "$guard_tmp"' EXIT HUP INT TERM
 # dpkg conffiles under /etc use dpkg's normal preserve/prompt semantics.
 : > "$guard_tmp/owned"
 for guard_package in '''+' '.join(allowed)+'''; do
  guard_status=$(dpkg-query -W -f='${db:Status-Status}' "$guard_package" 2>/dev/null || :)
  case "$guard_status" in installed|unpacked|half-installed|half-configured|triggers-awaited|triggers-pending)
   dpkg-query -L "$guard_package" >> "$guard_tmp/owned" 2>/dev/null || :;;
  esac
 done
'''+conditional+''' normalize_path() {
  case "$1" in /bin/*|/sbin/*|/lib/*|/lib64/*) printf '/usr%s\\n' "$1";; *) printf '%s\\n' "$1";; esac
 }
 while IFS= read -r guard_path; do normalize_path "$guard_path"; done < "$guard_tmp/owned" > "$guard_tmp/normalized"
 : > "$guard_tmp/conflicts"
 : > "$guard_tmp/known-dkms"
 check_owned() {
  if [ -e "$1" ] || [ -L "$1" ]; then
   guard_normal=$(normalize_path "$1")
   if ! grep -Fxq -- "$guard_normal" "$guard_tmp/normalized"; then printf '%s\\n' "$1" >> "$guard_tmp/conflicts"; fi
  fi
 }
 while IFS= read -r guard_path; do check_owned "$guard_path"; done <<'PROJECT_PAYLOAD_PATHS'
'''+ '\n'.join(paths)+'''
PROJECT_PAYLOAD_PATHS
'''+(''' for guard_source in '''+modules_shell+'''; do
  [ -e "$guard_source" ] || [ -L "$guard_source" ] || continue
  guard_registration=${guard_source%/source}
  guard_source=$(readlink -f "$guard_source")
  [ -f "$guard_source/dkms.conf" ] || { echo "E: incomplete DKMS source; preserved: $guard_source" >&2; exit 1; }
  check_owned "$guard_source/dkms.conf"
  guard_normal=$(normalize_path "$guard_source/dkms.conf")
  if grep -Fxq -- "$guard_normal" "$guard_tmp/normalized"; then printf '%s\n' "$guard_registration" >> "$guard_tmp/known-dkms"; fi
 done
'''if modules else '')+slots+''' if [ -s "$guard_tmp/conflicts" ]; then
  echo 'E: refusing files/registrations not owned by this package or its declared replacements:' >&2
  cat "$guard_tmp/conflicts" >&2
  echo 'Existing files were preserved. Review/migrate them before installing.' >&2
  exit 1
 fi
 rm -rf "$guard_tmp"
 trap - EXIT HUP INT TERM
 ;; esac
# END project payload ownership guard
'''

def main():
 p=argparse.ArgumentParser();p.add_argument('--debian',type=Path,required=True);p.add_argument('--dkms-provider',action='append',default=[]);p.add_argument('--module-file',action='append',default=[]);a=p.parse_args();providers=dict(item.split('=',1)for item in a.dkms_provider);module_files=dict(item.split('=',1)for item in a.module_file)
 import subprocess
 control=a.debian/'control'
 for stanza in control.read_text().split('\n\n'):
  fields=dict(line.split(': ',1)for line in stanza.splitlines()if ': 'in line and not line.startswith(' '))
  if 'Package'not in fields:continue
  name=fields['Package'];stage=a.debian/name
  if not stage.is_dir():continue
  allowed=[name]+[v.strip().split()[0]for v in fields.get('Conflicts','').split(',')if v.strip()]
  modules=[providers[name]]if name in providers else[]
  target=a.debian/(name+'.preinst');previous=target.read_text()if target.exists()else'#!/bin/sh\nset -e\n'
  body=previous.partition('\n')[2]
  if '# BEGIN project payload ownership guard'in body:
   start=body.index('# BEGIN project payload ownership guard');end=body.index('# END project payload ownership guard',start)+len('# END project payload ownership guard')
   body=body[:start]+body[end:].lstrip('\n')
  while body.startswith('set -e\n'):body=body[len('set -e\n'):]
  body=body.replace('#DEBHELPER#\n','')
  target.write_text('#!/bin/sh\nset -e\n'+text(stage,allowed,modules,installed_modules=[module_files[name]]if name in module_files else[])+'#DEBHELPER#\n'+body);target.chmod(0o755)
if __name__=='__main__':main()
