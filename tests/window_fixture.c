// SPDX-License-Identifier: GPL-2.0
/* QEMU-only config/mask model. Never used with hardware or DMA/MMIO traffic.
 * PF sees only a 64MiB physical BAR1 capability. A later callback mimics
 * NVIDIA's release/resize/reassign path after unlocking capabilities. */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/dmi.h>
#include <linux/version.h>
static bool limited, mixed, floor_budget, preserve;
module_param(preserve, bool, 0400);
module_param(floor_budget, bool, 0400);
module_param(limited, bool, 0400);
module_param(mixed, bool, 0400);
struct mock {
    struct pci_dev *dev;
    struct pci_ops *old_ops, ops;
    u32 ctrl, low1, high1, low3, high3;
    bool probe1, probe3, unlocked;
};
static struct mock models[2];
static unsigned int control_writes;
module_param(control_writes, uint, 0444);
static char root_bdf[16], ep0[16], ep1[16];
module_param_string(root_bdf, root_bdf, sizeof(root_bdf), 0444);
module_param_string(ep0, ep0, sizeof(ep0), 0444);
module_param_string(ep1, ep1, sizeof(ep1), 0444);
static struct resource pool, blocks[256];
static unsigned int nr_blocks;
static struct mock *model(struct pci_bus *bus) {
    int i;
    for (i = 0; i < 2; i++) if (models[i].dev && models[i].dev->bus == bus) return &models[i];
    return NULL;
}
static int cfg_read(struct pci_bus *bus, unsigned int fn, int where, int size, u32 *value)
{
    struct mock *m = model(bus);
    u32 v;
    u64 mask1 = ~((1ULL << (((m->ctrl >> 8) & 31) + 20)) - 1);
    u64 mask3 = ~((1ULL << 25) - 1);
    if (fn != m->dev->devfn) return m->old_ops->read(bus, fn, where, size, value);
    switch (where & ~3) {
    case PCI_VENDOR_ID: v = ((u32)m->dev->device << 16) | m->dev->vendor; break;
    case 0x100: v = PCI_EXT_CAP_ID_REBAR | BIT(16); break;
    case 0x104: v = (m->unlocked ? GENMASK(16, 6) : BIT(6)) << 4; break;
    case 0x108: v = m->ctrl; break;
    case PCI_BASE_ADDRESS_1: v = (m->probe1 ? lower_32_bits(mask1) : m->low1) | 12; break;
    case PCI_BASE_ADDRESS_2: v = m->probe1 ? upper_32_bits(mask1) : m->high1; break;
    case PCI_BASE_ADDRESS_3: v = (m->probe3 ? lower_32_bits(mask3) : m->low3) | 12; break;
    case PCI_BASE_ADDRESS_4: v = m->probe3 ? upper_32_bits(mask3) : m->high3; break;
    default: return m->old_ops->read(bus, fn, where, size, value);
    }
    *value = (v >> ((where & 3) * 8)) & (size == 4 ? ~0U : (1U << (size * 8)) - 1);
    return 0;
}
static int cfg_write(struct pci_bus *bus, unsigned int fn, int where, int size, u32 value)
{
    struct mock *m = model(bus);
    if (fn == m->dev->devfn && size == 4) {
        switch (where) {
        case 0x108: m->ctrl = value; control_writes++; return 0;
        case PCI_BASE_ADDRESS_1: m->probe1 = value == ~0U; if (!m->probe1) m->low1 = value & ~15U; return 0;
        case PCI_BASE_ADDRESS_2: if (value != ~0U) m->high1 = value; return 0;
        case PCI_BASE_ADDRESS_3: m->probe3 = value == ~0U; if (!m->probe3) m->low3 = value & ~15U; return 0;
        case PCI_BASE_ADDRESS_4: if (value != ~0U) m->high3 = value; return 0;
        }
    }
    return m->old_ops->write(bus, fn, where, size, value);
}
static int block(struct resource *parent, resource_size_t a, resource_size_t b)
{
    struct resource *r;
    if (a > b) return 0;
    if (nr_blocks == ARRAY_SIZE(blocks)) return -ENOMEM;
    r = &blocks[nr_blocks];r->start = a;r->end = b;r->flags = IORESOURCE_MEM;r->name = "qemu-window-budget-guard";
    if (request_resource(parent, r)) return -EBUSY;
    nr_blocks++;return 0;
}
static int budget(struct pci_bus *bus)
{
    struct resource *r, *c;
    resource_size_t cursor;
    int i, ret;
    pci_bus_for_each_resource(bus, r, i)
        if (r && (r->flags & IORESOURCE_MEM) && r->start >= (1ULL << 32)) {
            ret = allocate_resource(r, &pool, floor_budget ? 256ULL << 20 : 80ULL << 30, r->start, r->end, 64ULL << 30, NULL, NULL);
            if (!ret) break;
        }
    if (!pool.parent) return -ENOSPC;
    pci_bus_for_each_resource(bus, r, i) {
        if (!r || r == &iomem_resource || !(r->flags & IORESOURCE_MEM)) continue;
        cursor = r->start;
        for (c = r->child; c; c = c->sibling) {
            if (cursor < c->start) { ret = block(r, cursor, c->start - 1); if (ret) return ret; }
            cursor = c->end + 1;
        }
        ret = block(r, cursor, r->end);if (ret) return ret;
    }
    release_resource(&pool);return 0;
}
static int driver_resize(const char *val, const struct kernel_param *kp)
{
    unsigned int which;
    struct mock *m;
    int ret, requested = 16, old;
    u16 cmd;
    if (kstrtouint(val, 0, &which) || which > 1) return -EINVAL;
    m = &models[which];m->unlocked = true;
    pci_read_config_word(m->dev, PCI_COMMAND, &cmd);
    pci_write_config_word(m->dev, PCI_COMMAND, cmd & ~PCI_COMMAND_MEMORY);
    old = (m->ctrl >> 8) & 31;
    pci_release_resource(m->dev, 1);pci_release_resource(m->dev, 3);
retry:
    ret = pci_resize_resource(m->dev, 1, requested, 0);
    if (limited && ret == -ENOSPC && requested > old) {
        requested--;
        goto retry;
    }
    pci_assign_unassigned_bus_resources(m->dev->bus);
    if (limited && (!m->dev->resource[1].parent || !m->dev->resource[3].parent) && requested != old) {
        requested = old;
        goto retry;
    }
    pci_write_config_word(m->dev, PCI_COMMAND, cmd);
    pci_info(m->dev, "WINDOW-TEST driver resize rc=%d BAR1=%pR BAR3=%pR\n", ret, &m->dev->resource[1], &m->dev->resource[3]);
    if (ret || !m->dev->resource[1].parent || !m->dev->resource[3].parent ||
        (!limited && pci_resource_len(m->dev, 1) != (1ULL << 36)) ||
        (limited && pci_resource_len(m->dev, 1) < (1ULL << 26))) return -ENOSPC;
    return 0;
}
static const struct kernel_param_ops resize_ops = {.set = driver_resize};
module_param_cb(driver_resize, &resize_ops, NULL, 0200);
static int __init setup(void)
{
    struct pci_dev *d = NULL, *root;
    int i = 0, j, ret;
    if (!dmi_match(DMI_SYS_VENDOR, "QEMU")) return -EPERM;
    pool.name = "qemu-window-free-pool";pool.flags = IORESOURCE_MEM;
    for_each_pci_dev(d) if (d->vendor == 0x1b36 && d->device == 5 && i < 2) models[i++].dev = pci_dev_get(d);
    if (i != 2) return -ENODEV;
    for (i = 0; i < 2; i++) {
        struct mock *m = &models[i];
        struct resource source = m->dev->resource[2];
        if (m->dev->driver || !source.parent) return -EBUSY;
        pci_clear_master(m->dev);
        for (j = 1; j <= 4; j++) if (m->dev->resource[j].parent) release_resource(&m->dev->resource[j]);
        memset(&m->dev->resource[1], 0, sizeof(struct resource) * 4);
        source.parent = source.child = source.sibling = NULL;
        m->dev->resource[1] = source;
        /* Reclaim the old BAR2 range under its old bridge window as BAR1. */
        {
            struct resource *parent = m->dev->bus->resource[2];
            if (!parent || !(parent->flags & IORESOURCE_PREFETCH)) return -EINVAL;
            if (request_resource(parent, &m->dev->resource[1])) return -EBUSY;
        }
        m->low1 = lower_32_bits(source.start);m->high1 = upper_32_bits(source.start);
        m->ctrl = BIT(5) | (6 << 8) | 1;
        m->dev->vendor = 0x10de;m->dev->device = i ? 0x2082 : 0x20c2;
        m->dev->cfg_size = PCI_CFG_SPACE_EXP_SIZE;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
        m->dev->rebar_cap = 0x100;
#endif
        m->old_ops = m->dev->bus->ops;m->ops = *m->old_ops;
        m->ops.read = cfg_read;m->ops.write = cfg_write;m->dev->bus->ops = &m->ops;
        if (mixed && i && m->dev->resource[0].parent) {
            release_resource(&m->dev->resource[0]);
            m->dev->resource[0].start = m->dev->resource[0].end = m->dev->resource[0].flags = 0;
        }
    }
    strscpy(ep0, pci_name(models[0].dev), sizeof(ep0));strscpy(ep1, pci_name(models[1].dev), sizeof(ep1));
    root = models[0].dev;
    while (pci_upstream_bridge(root)) root = pci_upstream_bridge(root);
    strscpy(root_bdf, pci_name(root), sizeof(root_bdf));
    if (limited) {ret = budget(root->bus);if (ret) return ret;}
    if (preserve) pci_find_host_bridge(models[0].dev->bus)->preserve_config = true;
    return 0;
}
static void __exit teardown(void) { /* VM exits with model active; never restores real MMIO decoding. */ }
module_init(setup);module_exit(teardown);MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("QEMU-only CMP window reservation and later PCI resize model");
