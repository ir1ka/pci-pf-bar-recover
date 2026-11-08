#!/bin/bash
# Disposable container; QEMU has no disks/network/PCI passthrough.
set -euo pipefail
: "${TARGET_KERNEL:?explicit target ABI required}"
: "${KERNEL_IMAGE:=/boot/vmlinuz-$TARGET_KERNEL}"
work=/out/validation/handoff
mkdir -p "$work/fixture" "$work/root"/{bin,dev,proc,sys,modules} /work/pkg
cp /source/tests/window_fixture.c "$work/fixture/"
printf 'obj-m += window_fixture.o\n' > "$work/fixture/Makefile"
make -C "/lib/modules/$TARGET_KERNEL/build" M="$work/fixture" modules
package=$(find /out -maxdepth 1 -name '*prebuilt*.deb' -print -quit)
dpkg-deb -x "$package" /work/pkg
cp "/work/pkg/lib/modules/$TARGET_KERNEL/updates/pci-pf-bar-recover/pci_pf_bar_recover.ko" "$work/fixture/window_fixture.ko" "$work/root/modules/"
cp /bin/busybox "$work/root/bin/"
ln -sf busybox "$work/root/bin/sh"
cp /source/tests/guest-init-window "$work/root/init"
chmod 755 "$work/root/init"
cp /source/tests/run-vm-window.py "$work/"
(cd "$work/root" && find . -print0 | cpio --null -o -H newc --quiet | gzip -1) > "$work/guest-initramfs.gz"
for mode in full mixed limited floor preserve;do
 python3 "$work/run-vm-window.py" "$KERNEL_IMAGE" "$mode" > "$work/$mode.log"
done
printf '%s\n' 'PASS: full, mixed, limited, floor and firmware preserve_config on explicit target ABI' > "$work/result.txt"
