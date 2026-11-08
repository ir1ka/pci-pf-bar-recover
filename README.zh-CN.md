# PCI PF BAR Recover 用户手册

[English](README.md) | **简体中文**

**发行版验证范围：目前仅在 Debian 13 上测试过。其他发行版（包括 Ubuntu）尚未验证。

独立的 PCIe 物理 BAR 恢复工具，支持 Debian/Ubuntu amd64。无需修改或重编内核。安装包不自动恢复、不自动开启启动策略。设备正在工作时不要执行资源重排；工具对有驱动、活动 VF、总线主控等状态的设备/子树跳过或拒绝。

## 快速使用

**1. 安装 DKMS 包。** 首次公开发行不提供预编译模块包；自行构建方式见后文。

```bash
# DKMS 包，需要匹配当前内核的 headers
sudo apt install linux-headers-$(uname -r)
sudo apt install ./pci-pf-bar-recover-dkms_2.5.1-4_all.deb

```

**2. 单次自动恢复。** 默认扫描全部设备；也可按精确 vendor/device ID 恢复所有匹配卡，必要时跨 root 联合重平衡。

```bash
sudo pci-pf-bar-recover recover

# 或：同一 PEX 子树内同时为两个受支持 CMP ID 预留窗口
sudo pci-pf-bar-recover recover --ports 0000:00:03.1

# 或：按设备 ID 自动恢复，10de:20f1 为 A100 示例
sudo pci-pf-bar-recover recover --ensure-id 10de:20f1
```

**3. 启用开机自动恢复：下面两种范围选择一种。** 示例端口或设备 ID 按实际硬件替换；下次正常启动生效。

```bash
# 按 root port
sudo pci-pf-bar-boot enable --ports 0000:00:03.1

# 或：按设备 ID，自动选择所有匹配卡所在的 root 子树
sudo pci-pf-bar-boot enable --ensure-id 10de:20f1
```

`--ensure-id` 与 `--ports` 二选一；执行 `enable` 即表示显式启用开机恢复。

**4. 禁用开机自动恢复。**

```bash
sudo pci-pf-bar-boot disable
```

安装默认不启用开机恢复；以上命令不会重启。更多恢复模式、参数规则及使用限制见 [用户手册](README.Debian)。


## 两种 deb 构建形式

两种包使用同一套主线模块、wrapper 和默认关闭策略，保留 Ubuntu Noble、
精确 ABI 校验和预编译包的 initramfs 内核范围限制，不并列维护两套恢复源码。

| 构建入口 | 交付物 | 内核更新 |
| --- | --- | --- |
| `make deb-dkms`（`make deb` 为兼容别名） | `pci-pf-bar-recover-dkms_*_all.deb`，包含 DKMS 所需模块源码 | 有匹配 headers 时由 DKMS 自动编译；不自动启用恢复 |
| `make deb-prebuilt KERNEL_RELEASE=… KERNEL_PACKAGE_VERSION=… MODULE=…` | `pci-pf-bar-recover-prebuilt_*_amd64.deb`，仅模块、wrapper/hook 和 ABI 配置 | 只支持指定 ABI，新 ABI 需重新发布 |

预编译入口接受已编译的 x86-64 `.ko`，检查其 vermagic 与指定 ABI 一致。
`MODULE` 模式只打包指定模块；`KDIR` 模式先编译目标模块。输出目录默认为
`dist`，可通过 `OUTPUT_DIR` 覆盖；未指定目标 headers/ABI 或 MODULE 时拒绝构建。
回归容器脚本也可先编译精确 ABI，再调用仅打包模式。
两包拥有相同 wrapper，互相声明 Conflicts/Replaces，供用户二选一安装。
两种形式均支持单次修复与显式 early opt-in；预编译包不含源码的约束继续保留，
DKMS 包则必须携带编译所需源码。完整源码与构建工具始终进入 Git，生成物不进入 Git。

Noble 容器入口 `tools/containers/build-release.sh` 同时支持
`RELEASE_KIND=dkms|prebuilt|both`（默认 `prebuilt`）。通过 `docker run -e
RELEASE_KIND=both …` 可生成两种包；DKMS 路径还会在容器私有目录中针对目标
内核执行 `dkms add/build`，不安装或加载模块。两种包的内容清单分别验证。


## 范围、参数与行为

- 不指定范围时扫描全部设备；`--ports` 仅选择指定 root port 子树。
- `--ensure-id VENDOR:DEVICE` 自动选择匹配设备所在的 root 子树，必要时跨 root 联合重排；它与 `--ports`、`--rebalance`、`--conservative-only` 互斥。
- 默认先保守恢复，失败再对空闲子树尝试 rebalance 与标准 ReBAR 缩小。`--rebalance` 直接请求空闲事务，必须同时明确 `--ports`；`--conservative-only` 禁用重平衡及 CMP 预留，二者互斥。
- 仅恢复 PF 物理 BAR，不申请或预留 VF BAR；不修改未选择子树的 SR-IOV 能力。存在活动 VF 时不重排其子树。
- 对 CMP `10de:20c2`、`10de:2082` 默认尽力预留未来 64GiB BAR1 桥窗口，不执行厂商解锁，也不主动扩大当前 BAR。空间不足可降低预留目标；实际缩小只使用设备公布的 ReBAR 支持尺寸。后续设备 resize 是否成功仍取决于驱动和资源状态。
- 恢复前临时保持目标设备及上游设备供电；单个设备失败不终止其他设备扫描。没有足够的主桥地址空间时不保证成功。64GiB BAR1 不是必须条件。

## 启动、升级与卸载

boot 使用 initramfs-tools，在早期 udev 触发设备加载前同步执行。它并不早于内核内建驱动或所有 initramfs 脚本；已工作子树仍受保护。安装默认关闭；已 opt-in 的正常升级会刷新自身启动内容。DKMS 需要目标内核 headers；预编译包仅支持精确 ABI，不能跨内核使用。

```sh
sudo pci-pf-bar-boot disable
sudo apt remove pci-pf-bar-recover-dkms
# 或移除已选择的 pci-pf-bar-recover-prebuilt
```

移除软件包清理其启动入口；不会逆向撤销本次启动中已经完成的 PCI 资源分配。不要把卸载当作 PCI 地址回滚。

## 构建

需要 Python 3、Git、make、C 编译器、Debian debhelper、dh-sequence-dkms 和 dpkg-dev。

```sh
make check
make deb-dkms
make source
make deb-prebuilt KERNEL_RELEASE=7.0.0-30-generic \
  KDIR=/path/to/target/headers KERNEL_PACKAGE_VERSION=7.0.0-30.30~24.04.1
```

预编译目标内核、headers、编译器可显式指定，不取构建主机正在运行的 ABI。源码包和 deb 输出到 dist；构建不安装或加载驱动。Secure Boot 环境由用户的 DKMS/受信任密钥签名，不包含发布者私钥。

## 交流与维护

维护者：IriKa Qiu（[ir1ka](https://github.com/ir1ka)）。通过 [Issues](https://github.com/ir1ka/pci-pf-bar-recover/issues) 报告问题与讨论，通过 [Pull Requests](https://github.com/ir1ka/pci-pf-bar-recover/pulls) 提交改进。安全问题请先请求私密渠道，不要在公开 issue 中附上漏洞细节或密钥。

本项目采用 GPL v2；保留所有第三方原始许可证和署名。
