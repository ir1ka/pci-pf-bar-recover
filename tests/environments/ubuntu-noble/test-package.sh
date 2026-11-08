#!/bin/bash
# Fully configure packages only in a disposable container, never on the host.
set -euo pipefail
: "${TARGET_KERNEL:=7.0.0-28-generic}"
dpkg --force-unsafe-io -i /opt/kernel-debs/*.deb
dpkg --force-unsafe-io -i /out/pci-pf-bar-recover-prebuilt_*.deb
dpkg-query -W -f='${db:Status-Abbrev} ${Package} ${Version}\n' pci-pf-bar-recover-prebuilt > /out/validation/package-installed.txt
[ ! -e /etc/initramfs-tools/conf.d/pci-pf-bar-rebalance ]
[ ! -e /var/lib/pci-pf-bar-recover/early-image-state ]
# Only this disposable filesystem is opted in, to validate Ubuntu's real hook.
mkdir -p /etc/initramfs-tools/conf.d
printf "# Managed by pci-pf-bar-boot; experimental opt-in\nPFBAR_REBALANCE=experimental\nPFBAR_PORTS='0000:00:01.0,0000:00:1c.6'\n" > /etc/initramfs-tools/conf.d/pci-pf-bar-rebalance
mkinitramfs -o /out/validation/enabled-initramfs.gz "$TARGET_KERNEL"
lsinitramfs /out/validation/enabled-initramfs.gz > /out/validation/enabled-initramfs-files.txt
grep -Fxq 'conf/pci-pf-bar-rebalance' /out/validation/enabled-initramfs-files.txt
grep -Fxq 'usr/lib/pci-pf-bar-recover/run-early' /out/validation/enabled-initramfs-files.txt || grep -Fxq 'lib/pci-pf-bar-recover/run-early' /out/validation/enabled-initramfs-files.txt
# Reconfigure/upgrade with an existing ID opt-in. postinst must preserve it
# and rebuild the real target initramfs with the cross-root selector.
/usr/sbin/pci-pf-bar-boot enable --ensure-id 10de:20f1
dpkg --force-unsafe-io -i /out/pci-pf-bar-recover-prebuilt_*.deb
lsinitramfs "/boot/initrd.img-$TARGET_KERNEL" > /out/validation/ensure-id-initramfs-files.txt
grep -Fxq 'conf/pci-pf-bar-ensure-id' /out/validation/ensure-id-initramfs-files.txt
grep -Fq "PFBAR_ENSURE_ID='10de:20f1'" /etc/initramfs-tools/conf.d/pci-pf-bar-rebalance
rm /etc/initramfs-tools/conf.d/pci-pf-bar-rebalance
printf '%s\n' 'PASS: real dpkg dependency/configuration check, fresh install disabled, real Ubuntu opt-in hook, existing ID opt-in preserved on reconfigure and embedded in rebuilt initramfs' > /out/validation/package-test-result.txt
