#!/bin/bash
# Disposable container only; no host /boot, /lib/modules or PCI devices mounted.
set -euo pipefail
# Containers share the host uname; discover installed target headers instead.
if [ -z "${TARGET_KERNEL:-}" ]; then
    mapfile -t targets < <(for entry in /lib/modules/*; do [ ! -f "$entry/build/Makefile" ] || basename "$entry"; done)
    [ "${#targets[@]}" = 1 ] || { echo 'Set TARGET_KERNEL when headers are absent or ambiguous' >&2; exit 2; }
    TARGET_KERNEL=${targets[0]}
fi
if [ -z "${TARGET_KERNEL_PACKAGE_VERSION:-}" ]; then
    TARGET_KERNEL_PACKAGE_VERSION=$(dpkg-query -W -f='${Version}' "linux-headers-$TARGET_KERNEL")
fi
: "${RELEASE_KIND:=prebuilt}"
case "$RELEASE_KIND" in dkms|prebuilt|both) ;; *) echo "RELEASE_KIND must be dkms, prebuilt or both" >&2; exit 2;; esac
[ -d /source/src ] && [ -d /out ]
export PYTHONDONTWRITEBYTECODE=1
python3 - <<'PY'
import importlib.util,shutil
from pathlib import Path
s=importlib.util.spec_from_file_location('export','/source/tools/build_debian.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
for name in m.DIRECTORIES:
    shutil.copytree(Path('/source')/name,Path('/work')/name,ignore=m.ignore)
for name in m.FILES:
    shutil.copy2(Path('/source')/name,Path('/work')/name)
PY
cd /work
make check
mkdir -p /out/validation
if [ "$RELEASE_KIND" = dkms ] || [ "$RELEASE_KIND" = both ]; then
    make deb-dkms
    cp dist/pci-pf-bar-recover-dkms_*.deb /out/
    version=$(dpkg-parsechangelog -SVersion)
    mkdir -p /work/dkms-package /work/dkms-tree /work/dkms-install
    dpkg-deb -x dist/pci-pf-bar-recover-dkms_*.deb /work/dkms-package
    dkms add -m pci-pf-bar-recover -v "$version" --sourcetree /work/dkms-package/usr/src --dkmstree /work/dkms-tree --installtree /work/dkms-install
    dkms build -m pci-pf-bar-recover -v "$version" -k "$TARGET_KERNEL" --kernelsourcedir "/lib/modules/$TARGET_KERNEL/build" --sourcetree /work/dkms-package/usr/src --dkmstree /work/dkms-tree --installtree /work/dkms-install > /out/validation/dkms-build.log 2>&1
    dkms status --dkmstree /work/dkms-tree > /out/validation/dkms-status.txt
    dpkg-deb --contents /out/pci-pf-bar-recover-dkms_*.deb > /out/validation/dkms-package-files.txt
fi
if [ "$RELEASE_KIND" = dkms ]; then
    dpkg-query -W > /out/validation/builder-packages.txt
    cp /etc/os-release /out/validation/builder-os-release
    printf '%s\n' 'PASS: DKMS deb and isolated exact-ABI DKMS build; no installation or loading' > /out/validation/build-result.txt
    exit 0
fi
export SOURCE_DATE_EPOCH=$(dpkg-parsechangelog -STimestamp)
export KBUILD_BUILD_USER=builder KBUILD_BUILD_HOST=pfbar-builder KBUILD_BUILD_VERSION=1
export KBUILD_BUILD_TIMESTAMP=$(date -u -d "@$SOURCE_DATE_EPOCH" '+%a %b %d %T UTC %Y')
make -C src KERNEL_RELEASE="$TARGET_KERNEL"
mkdir -p /out/validation
build_deb() { make deb-prebuilt KERNEL_RELEASE="$TARGET_KERNEL" KERNEL_PACKAGE_VERSION="$TARGET_KERNEL_PACKAGE_VERSION" MODULE=src/pci_pf_bar_recover.ko OUTPUT_DIR=/out; }
build_deb
sha256sum /out/pci-pf-bar-recover-prebuilt_*.deb > /out/validation/deb-first.sha256
build_deb
sha256sum /out/pci-pf-bar-recover-prebuilt_*.deb > /out/validation/deb-second.sha256
cmp /out/validation/deb-first.sha256 /out/validation/deb-second.sha256
# Stage package files and run only our configure script in the container.
# Full dpkg and platform initramfs checks belong to tests/environments/.
dpkg --unpack /out/pci-pf-bar-recover-prebuilt_*.deb
depmod "$TARGET_KERNEL"
# Invoke our configuration script, without claiming dpkg dependencies configured.
/var/lib/dpkg/info/pci-pf-bar-recover-prebuilt.postinst configure
modinfo -k "$TARGET_KERNEL" pci_pf_bar_recover > /out/validation/module-info.txt
[ ! -d "/usr/src/pci-pf-bar-recover-$(dpkg-parsechangelog -SVersion)" ]
[ ! -e /etc/initramfs-tools/conf.d/pci-pf-bar-rebalance ]
[ ! -e /var/lib/pci-pf-bar-recover/early-image-state ]
dpkg-deb --contents /out/pci-pf-bar-recover-prebuilt_*.deb > /out/validation/package-files.txt
mkinitramfs -o /out/validation/default-initramfs.gz "$TARGET_KERNEL"
lsinitramfs /out/validation/default-initramfs.gz > /out/validation/default-initramfs-files.txt
if grep -Eq 'conf/pci-pf-bar-rebalance|lib/pci-pf-bar-recover/run-early' /out/validation/default-initramfs-files.txt; then
    echo 'Default initramfs unexpectedly enabled recovery' >&2; exit 1
fi
dpkg-query -W > /out/validation/builder-packages.txt
cp /etc/os-release /out/validation/builder-os-release
if [ -d /opt/kernel-debs ]; then cp --update=none /opt/kernel-debs/*.deb /out/validation/; fi
printf '%s\n' 'PASS: reproducible source-free deb, exact target ABI build, staged package configure, default-off initramfs' > /out/validation/build-result.txt
