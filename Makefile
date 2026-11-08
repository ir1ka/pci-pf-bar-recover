.PHONY: help check deb deb-dkms deb-prebuilt source integration
KDIR ?= $(if $(KERNEL_RELEASE),/lib/modules/$(KERNEL_RELEASE)/build,)
OUTPUT_DIR ?= dist
help:
	@printf '%s\n' 'make check        Unit tests only; no host module loading' 'make deb-dkms     Build DKMS .deb into dist/; make deb is an alias' 'make deb-prebuilt Build against KDIR/target ABI, or package existing MODULE; no running-kernel inference' 'make source       Build complete Debian source package into dist/' 'make integration  Optional QEMU/KVM integration tests (sudo required)'

check:
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_*.py'

deb: deb-dkms

deb-dkms:
	python3 tools/build_debian.py binary

deb-prebuilt:
	@test -n "$(MODULE)$(KDIR)" || { echo 'Set KDIR (prepared target headers), KERNEL_RELEASE, or MODULE explicitly' >&2; exit 2; }
	@test -z "$(MODULE)" -o -z "$(KDIR)" -o "$(origin KDIR)" != "command line" || { echo 'Choose MODULE or explicit KDIR, not both' >&2; exit 2; }
	python3 tools/build_prebuilt_deb.py $(if $(MODULE),--module "$(MODULE)",--headers "$(KDIR)") $(if $(KERNEL_RELEASE),--kernel "$(KERNEL_RELEASE)") $(if $(KERNEL_PACKAGE_VERSION),--kernel-package-version "$(KERNEL_PACKAGE_VERSION)") $(if $(filter default undefined,$(origin CC)),,--cc "$(CC)") $(if $(ARCH),--arch "$(ARCH)") $(if $(CROSS_COMPILE),--cross-compile "$(CROSS_COMPILE)") $(if $(STRIP),--strip "$(STRIP)") --output "$(OUTPUT_DIR)"

source:
	python3 tools/build_debian.py source

integration:
	tests/run-integration.sh

# Run inside a disposable container as root; no module insertion.
.PHONY: check-initramfs
check-initramfs:
	python3 tests/busybox_initramfs.py
	python3 tests/runtime_bundle.py
