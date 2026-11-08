# PCI PF BAR Recover

**English** | [Chinese](README.zh-CN.md)

**Distribution validation:** Tested only on Debian 13. Other distributions, including Ubuntu, have not been validated.

Recover PCIe physical-function BAR resources on Debian/Ubuntu amd64 without rebuilding the kernel. Installing the package does not run recovery or enable boot recovery. Resource rebalancing requires an idle subtree: devices with bound drivers, active VFs, bus mastering or other unsafe state are skipped or refused.

## Quick start

Install the DKMS package and matching kernel headers. The initial public release does not ship precompiled modules.

```sh
sudo apt install linux-headers-$(uname -r)
sudo apt install ./pci-pf-bar-recover-dkms_2.5.1-4_all.deb
```

Run automatic recovery once. Choose the scope appropriate for your system:

```sh
sudo pci-pf-bar-recover recover
# Or restrict recovery to a root-port subtree:
sudo pci-pf-bar-recover recover --ports 0000:00:03.1
# Or recover every matching vendor/device ID, across root ports if needed:
sudo pci-pf-bar-recover recover --ensure-id 10de:20f1
```

Explicitly enable recovery on subsequent boots using either ports or an ID:

```sh
sudo pci-pf-bar-boot enable --ports 0000:00:03.1
# Alternative:
sudo pci-pf-bar-boot enable --ensure-id 10de:20f1
```

Replace the example addresses/IDs with your hardware. `enable` opts into boot recovery; it does not reboot the machine or immediately repair devices. Disable future boot recovery with:

```sh
sudo pci-pf-bar-boot disable
```

## Scope and options

- Without a selector, recovery scans all devices. `--ports` limits it to the specified root-port subtrees.
- `--ensure-id VENDOR:DEVICE` selects all root subtrees containing matching devices. It can jointly rebalance those subtrees when necessary. It is mutually exclusive with `--ports`, `--rebalance` and `--conservative-only`.
- Automatic recovery first tries conservative assignment, then idle-subtree rebalancing and standard ReBAR size reduction if necessary. `--rebalance` directly requests an idle transaction and requires explicit `--ports`. `--conservative-only` disables that fallback and CMP window reservation; these two options are mutually exclusive.
- Only PF physical BARs are recovered. VF BAR space is neither allocated nor reserved. Unselected subtrees retain their SR-IOV capabilities; subtrees with active VFs are not rebalanced.
- For CMP IDs `10de:20c2` and `10de:2082`, recovery attempts to reserve bridge-window space for a future 64GiB BAR1. It does not perform vendor unlocking or enlarge the current physical BAR. Reservation can shrink when space is limited; physical BAR reductions use only sizes advertised by the device's ReBAR capability. A later driver resize is still conditional on the driver and available resources.
- Target devices and their ancestors are temporarily kept powered during recovery. An individual device failure does not abort the remaining scan. Recovery is best effort; insufficient host-bridge address space can prevent success. A 64GiB BAR1 is not required.

## Boot, upgrades and removal

Boot integration uses initramfs-tools and runs synchronously before early udev triggers device loading. It is not earlier than built-in kernel drivers or every initramfs script. Subtrees already in use remain protected.

Installation defaults to disabled. Normal package upgrades preserve an existing opt-in and refresh this project's boot content. DKMS requires headers for the target kernel. Precompiled packages, if built locally, require their exact ABI.

```sh
sudo pci-pf-bar-boot disable
sudo apt remove pci-pf-bar-recover-dkms
# Remove pci-pf-bar-recover-prebuilt instead if that is what you installed.
```

Removal cleans up the package's boot integration. It does not undo PCI resource assignments already made during the current boot; uninstalling is not PCI address rollback.

## Build

Dependencies: Python 3, Git, make, a C compiler, Debian debhelper, dh-sequence-dkms and dpkg-dev. Module compilation also needs matching target headers.

```sh
make check
make deb-dkms                 # make deb is an alias
make source
# Optional local precompiled build, not shipped in the initial release:
make deb-prebuilt KERNEL_RELEASE=7.0.0-30-generic \
  KDIR=/path/to/target/headers KERNEL_PACKAGE_VERSION=7.0.0-30.30~24.04.1
```

| Build | Contents | Kernel updates |
| --- | --- | --- |
| `deb-dkms` | Module source and wrappers | DKMS rebuilds with matching headers; recovery is not automatically enabled |
| `deb-prebuilt` | Compiled module, wrappers/hooks and ABI configuration | Exact ABI only; rebuild for each new ABI |

Both variants share the same implementation and opt-in behavior and declare mutual Conflicts/Replaces. The precompiled package contains no module source. `MODULE=/path/to/module.ko` packages an existing x86-64 module after checking vermagic; `KDIR` builds against explicit target headers first. The target ABI, headers and compiler can be specified without using the build host's running kernel. Missing target information is rejected. Output defaults to `dist`, with `OUTPUT_DIR` available for precompiled builds.

The Noble container entry `tools/containers/build-release.sh` accepts `RELEASE_KIND=dkms|prebuilt|both` (default `prebuilt`). Its DKMS path builds in container-private directories without installing or loading modules on the host. Secure Boot signing uses the target system's trusted DKMS key; no publisher private key is supplied.

## Contact and license

Maintained by IriKa Qiu ([ir1ka](https://github.com/ir1ka)). Use [Issues](https://github.com/ir1ka/pci-pf-bar-recover/issues) for discussion and bug reports, and [Pull Requests](https://github.com/ir1ka/pci-pf-bar-recover/pulls) for contributions. For security issues, request a private channel without posting vulnerability details or keys publicly.

GPL v2; preserve all third-party license and attribution notices.
