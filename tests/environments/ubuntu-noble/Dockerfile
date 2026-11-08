# Build tools only: no host /lib/modules, /boot, GPU or PCI devices are mounted.
ARG BASE_IMAGE=ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852
FROM ${BASE_IMAGE}
ARG KERNEL_RELEASE=7.0.0-28-generic
ARG KERNEL_PACKAGE_VERSION=7.0.0-28.28~24.04.1
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential debhelper dh-dkms dkms python3 git kmod pahole ca-certificates \
    initramfs-tools busybox-static cpio zstd qemu-system-x86 \
    linux-headers-${KERNEL_RELEASE}=${KERNEL_PACKAGE_VERSION} \
    && dpkg-query -W > /opt/builder-packages.txt
# Extract target boot files/modules for diskless VM validation; no kernel postinst.
RUN mkdir -p /opt/kernel-debs && cd /opt/kernel-debs \
    && apt-get download linux-image-unsigned-${KERNEL_RELEASE}=${KERNEL_PACKAGE_VERSION} \
       linux-modules-${KERNEL_RELEASE}=${KERNEL_PACKAGE_VERSION} \
    && mkdir -p /opt/kernel-root /boot /usr/lib/modules \
    && for package in *.deb; do dpkg-deb -x "$package" /opt/kernel-root; done \
    && cp -a /opt/kernel-root/boot/. /boot/ \
    && if [ -d /opt/kernel-root/lib/modules ]; then cp -a /opt/kernel-root/lib/modules/. /usr/lib/modules/; fi \
    && if [ -d /opt/kernel-root/usr/lib/modules ]; then cp -a /opt/kernel-root/usr/lib/modules/. /usr/lib/modules/; fi \
    && rm -rf /opt/kernel-root \
    && ln -sfn /usr/src/linux-headers-${KERNEL_RELEASE} /lib/modules/${KERNEL_RELEASE}/build \
    && depmod ${KERNEL_RELEASE}
ENV TARGET_KERNEL=${KERNEL_RELEASE}
WORKDIR /work
