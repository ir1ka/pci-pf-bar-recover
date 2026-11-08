#!/bin/bash
# Target Ubuntu kernel in diskless QEMU/KVM; no host PCI passthrough.
set -euo pipefail
: "${TARGET_KERNEL:=7.0.0-28-generic}"
[ -c /dev/kvm ]
work=/out/validation/vm
mkdir -p "$work" /work/fixture /work/package
cp /source/tests/{fault_fixture.c,rebalance_fixture.c,empty_bridge_fixture.c,rebar_fixture.c,cohort_fixture.c,window_fixture.c} /work/fixture/
printf 'obj-m += fault_fixture.o rebalance_fixture.o empty_bridge_fixture.o rebar_fixture.o cohort_fixture.o window_fixture.o\n' > /work/fixture/Makefile
make -C /lib/modules/"$TARGET_KERNEL"/build M=/work/fixture modules
mkdir -p "$work/root"/{bin,dev,proc,sys,modules}
cp /bin/busybox "$work/root/bin/"
ln -sf busybox "$work/root/bin/sh"
for mod in i2c-algo-bit dca igb; do
    file=$(modinfo -k "$TARGET_KERNEL" -n "$mod")
    case "$file" in
        *.xz) xz -dc "$file" > "$work/root/modules/$mod.ko";;
        *.zst) zstd -dc "$file" > "$work/root/modules/$mod.ko";;
        *.gz) gzip -dc "$file" > "$work/root/modules/$mod.ko";;
        *) cp "$file" "$work/root/modules/$mod.ko";;
    esac
done
dpkg-deb -x /out/pci-pf-bar-recover-prebuilt_*.deb /work/package
cp /work/package/lib/modules/"$TARGET_KERNEL"/updates/pci-pf-bar-recover/pci_pf_bar_recover.ko "$work/root/modules/"
cp /work/fixture/*.ko "$work/root/modules/"
cp /source/tests/run-vm.py "$work/"
pack() { (cd "$work/root" && find . -print0 | cpio --null -o -H newc --quiet | gzip -1) > "$work/guest-initramfs.gz"; }
cp /source/tests/guest-init "$work/root/init"
chmod 755 "$work/root/init"
pack
python3 "$work/run-vm.py" "/boot/vmlinuz-$TARGET_KERNEL" control > "$work/recovery.log"
cp /source/tests/guest-init-early "$work/root/init"
mkdir -p "$work/root/lib/pci-pf-bar-recover" "$work/root/scripts/init-top" "$work/root/sbin"
cp /source/lib/early-udev-wrapper "$work/root/scripts/init-top/udev"
cp /source/lib/run-early "$work/root/lib/pci-pf-bar-recover/run-early"
cp /source/tests/test-udev-original "$work/root/lib/pci-pf-bar-recover/udev.original"
cp "$work/root/modules/pci_pf_bar_recover.ko" "$work/root/lib/pci-pf-bar-recover/pci_pf_bar_recover.ko"
ln -sf /bin/busybox "$work/root/sbin/rmmod"
chmod 755 "$work/root/init" "$work/root/scripts/init-top/udev" "$work/root/lib/pci-pf-bar-recover/"*
pack
for mode in disabled enabled bypass; do
    extra="pfbar_early_test=$mode"
    if [ "$mode" = bypass ]; then extra="$extra pci_pf_bar_rebalance=off"; fi
    PFBAR_VM_EXTRA_CMDLINE="$extra" python3 "$work/run-vm.py" "/boot/vmlinuz-$TARGET_KERNEL" control > "$work/early-$mode.log"
done
cp /source/tests/guest-init-rebar "$work/root/init"
pack
for mode in success multi exhausted mismatch writefail early conservative busy forced global; do
    PFBAR_VM_EXTRA_CMDLINE="pfbar_rebar_test=$mode" python3 "$work/run-vm.py" "/boot/vmlinuz-$TARGET_KERNEL" control > "$work/rebar-$mode.log"
done
cp /source/tests/guest-init-cohort "$work/root/init"
cp /source/tests/run-vm-cohort.py "$work/"
pack
for mode in success healthy mismatch busy nomatch early; do
    PFBAR_VM_EXTRA_CMDLINE="pfbar_cohort_test=$mode" python3 "$work/run-vm-cohort.py" "/boot/vmlinuz-$TARGET_KERNEL" > "$work/cohort-$mode.log"
done
printf '%s\n' 'PASS: target Ubuntu kernel recovery, rollback, SR-IOV preservation early disabled/enabled/bypass, and ReBAR shrink/rollback/early VM tests' > "$work/result.txt"

cp /source/tests/guest-init-window "$work/root/init"
cp /source/tests/run-vm-window.py "$work/"
pack
for mode in full mixed limited floor preserve;do
 python3 "$work/run-vm-window.py" "/boot/vmlinuz-$TARGET_KERNEL" "$mode" > "$work/window-$mode.log"
done
printf '%s\n' 'PASS: true small BAR resource extents, multi-level 64GiB reservations, later kernel resize of both cards, mixed BAR0 and limited-aperture fallback' > "$work/window-result.txt"
