#!/bin/bash
# All paths below belong to a disposable container; no host /etc or /boot mounts.
set -euo pipefail
: "${TARGET_KERNEL:=7.0.0-30-generic}"
dpkg -i /opt/kernel-debs/*.deb > /out/kernel-install.log 2>&1
# Install matching target kernel packages only in the disposable builder.
dpkg -i /old/*.deb > /out/pf-old-install.log 2>&1
[ ! -e /etc/initramfs-tools/conf.d/pci-pf-bar-rebalance ]
pci-pf-bar-boot enable --ensure-id 10de:20f1 > /out/pf-enable.log 2>&1
cp /etc/initramfs-tools/conf.d/pci-pf-bar-rebalance /out/pf-optin-before
cp /var/lib/pci-pf-bar-recover/early-image-state /out/pf-state-before
dpkg -i /new/*prebuilt*.deb > /out/pf-upgrade.log 2>&1
cmp /etc/initramfs-tools/conf.d/pci-pf-bar-rebalance /out/pf-optin-before
[ "$(cat /var/lib/pci-pf-bar-recover/early-image-state)" = enabled ]
mkdir -p /work/image
unmkinitramfs "/boot/initrd.img-$TARGET_KERNEL" /work/image
find /work/image -path '*/pci-pf-bar-recover/run-early' -exec grep -q boot-records.jsonl {} \;
grep -R -q boot-records.jsonl /work/image
rm -rf /work/image
dpkg --remove pci-pf-bar-recover-prebuilt > /out/pf-remove.log 2>&1
[ ! -e /etc/initramfs-tools/conf.d/pci-pf-bar-rebalance ]
lsinitramfs "/boot/initrd.img-$TARGET_KERNEL" > /out/pf-removed-image.txt
! grep -q 'pci-pf-bar-recover/run-early' /out/pf-removed-image.txt
dpkg --purge pci-pf-bar-recover-prebuilt > /out/pf-purge.log 2>&1
printf '%s\n' 'PASS: PF prebuilt upgrade with opt-in and remove/purge cleanup; real target kernel packages' > /out/result.txt
