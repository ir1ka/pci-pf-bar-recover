#!/bin/bash
# Diskless QEMU/KVM test, never uses host PCI passthrough or host module loading.
set -euo pipefail
base=$(cd -- "$(dirname -- "$0")" && pwd)
source_dir=$(dirname "$base")
work=${TEST_DIR:-$(mktemp -d /var/tmp/pci-pf-bar-test.XXXXXX)}
mkdir -p "$work/module" "$work/fixture" "$work/root"/{bin,dev,proc,sys,modules}
cp "$source_dir/src/pci_pf_bar_recover.c" "$source_dir/src/rebalance.inc" "$source_dir/src/bar_policy.inc" "$source_dir/src/Makefile" "$work/module/"
make -C "$work/module"
cp "$base/fault_fixture.c" "$work/fixture/"
cp "$base/rebalance_fixture.c" "$base/empty_bridge_fixture.c" "$work/fixture/"
printf 'obj-m += fault_fixture.o rebalance_fixture.o empty_bridge_fixture.o\n' > "$work/fixture/Makefile"
make -C "/lib/modules/$(uname -r)/build" M="$work/fixture" modules
cp /bin/busybox "$work/root/bin/"
[ -z "$(ldd /bin/busybox 2>/dev/null || true)" ] || file /bin/busybox | grep -q 'statically linked' || { echo 'Need busybox-static' >&2; exit 1; }
ln -sf busybox "$work/root/bin/sh"
for mod in i2c-algo-bit dca igb; do
    file=$(/usr/sbin/modinfo -n "$mod")
    case "$file" in
        *.xz) xz -dc "$file" > "$work/root/modules/$mod.ko";;
        *.zst) zstd -dc "$file" > "$work/root/modules/$mod.ko";;
        *.gz) gzip -dc "$file" > "$work/root/modules/$mod.ko";;
        *) cp "$file" "$work/root/modules/$mod.ko";;
    esac
done
cp "$work/module/pci_pf_bar_recover.ko" "$work/fixture/fault_fixture.ko" "$work/fixture/rebalance_fixture.ko" "$work/fixture/empty_bridge_fixture.ko" "$work/root/modules/"
cp "$base/guest-init" "$work/root/init"
chmod 755 "$work/root/init"
(cd "$work/root" && find . -print0 | cpio --null -o -H newc --quiet | gzip -1) > "$work/guest-initramfs.gz"
cp "$base/run-vm.py" "$work/"
sudo -n python3 "$work/run-vm.py" "/boot/vmlinuz-$(uname -r)" control
echo "Test artifacts: $work"

# Exercise the actual optional early wrapper and runtime script in the guest.
cp "$base/guest-init-early" "$work/root/init"
mkdir -p "$work/root/lib/pci-pf-bar-recover" "$work/root/scripts/init-top" "$work/root/sbin"
cp "$source_dir/lib/early-udev-wrapper" "$work/root/scripts/init-top/udev"
cp "$source_dir/lib/run-early" "$work/root/lib/pci-pf-bar-recover/run-early"
cp "$base/test-udev-original" "$work/root/lib/pci-pf-bar-recover/udev.original"
cp "$work/root/modules/pci_pf_bar_recover.ko" "$work/root/lib/pci-pf-bar-recover/pci_pf_bar_recover.ko"
ln -sf /bin/busybox "$work/root/sbin/rmmod"
chmod 755 "$work/root/init" "$work/root/scripts/init-top/udev" "$work/root/lib/pci-pf-bar-recover/"*
(cd "$work/root" && find . -print0 | cpio --null -o -H newc --quiet | gzip -1) > "$work/guest-initramfs.gz"
for mode in disabled enabled bypass; do
    extra="pfbar_early_test=$mode"
    if [ "$mode" = bypass ]; then extra="$extra pci_pf_bar_rebalance=off"; fi
    sudo -n env PFBAR_VM_EXTRA_CMDLINE="$extra" python3 "$work/run-vm.py" "/boot/vmlinuz-$(uname -r)" control
done
