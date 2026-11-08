// SPDX-License-Identifier: GPL-2.0
/* Diskless QEMU only: two pci-testdev PFs on different roots. Config/mask
 * model only, not physical resized MMIO decoding. No DMA or MMIO traffic. */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/dmi.h>
static bool healthy, mismatch, busy;
module_param(healthy, bool, 0400);
module_param(mismatch, bool, 0400);
module_param(busy, bool, 0400);
struct mock {
    struct pci_dev *ep, *root;
    struct pci_ops *old_ops, ops;
    int cfg_size;
    bool hotplug, probing;
    u32 ctrl, address, high;
};
static struct mock mocks[2];
static struct resource pool, scratch, blocks[256];
static unsigned int nr_blocks;
static struct mock *get_mock(struct pci_bus *bus)
{
    int i;
    for (i = 0; i < 2; i++)
        if (mocks[i].ep && mocks[i].ep->bus == bus) return &mocks[i];
    return NULL;
}
static int cfg_read(struct pci_bus *bus, unsigned int fn, int where, int size, u32 *value)
{
    struct mock *m = get_mock(bus);
    u32 v;
    if (fn != m->ep->devfn) return m->old_ops->read(bus, fn, where, size, value);
    switch (where & ~3) {
    case 0x100: v = PCI_EXT_CAP_ID_REBAR | BIT(16); break;
    case 0x104: v = (BIT(4) | BIT(5) | BIT(6)) << 4; break;
    case 0x108: v = m->ctrl; break;
    case PCI_BASE_ADDRESS_2:
        v = m->probing ? (~((1U << ((mismatch ? 6 : (m->ctrl >> 8) & 31) + 20)) - 1) & ~15U) | 12 : m->address | 12;
        break;
    case PCI_BASE_ADDRESS_3: v = m->probing ? ~0U : m->high; break;
    default: return m->old_ops->read(bus, fn, where, size, value);
    }
    *value = (v >> ((where & 3) * 8)) & (size == 4 ? ~0U : (1U << (size * 8)) - 1);
    return 0;
}
static int cfg_write(struct pci_bus *bus, unsigned int fn, int where, int size, u32 value)
{
    struct mock *m = get_mock(bus);
    if (fn == m->ep->devfn && size == 4) {
        if (where == 0x108) { m->ctrl = value; return 0; }
        if (where == PCI_BASE_ADDRESS_2) {
            m->probing = value == ~0U;
            if (!m->probing) m->address = value & ~15U;
            return 0;
        }
        if (where == PCI_BASE_ADDRESS_3) {
            if (value != ~0U) m->high = value;
            return 0;
        }
    }
    return m->old_ops->write(bus, fn, where, size, value);
}
static int add_block(struct resource *parent, resource_size_t start, resource_size_t end)
{
    struct resource *r;
    if (start > end) return 0;
    if (nr_blocks == ARRAY_SIZE(blocks)) return -ENOMEM;
    r = &blocks[nr_blocks];
    r->start = start; r->end = end; r->flags = IORESOURCE_MEM;
    r->name = "qemu-only-cohort-reservation";
    if (request_resource(parent, r)) return -EBUSY;
    nr_blocks++;
    return 0;
}
static int exhaust(struct pci_bus *bus)
{
    struct resource *r, *child, *next;
    resource_size_t cursor;
    int i, ret;
    pci_bus_for_each_resource(bus, r, i) {
        if (!r || r == &iomem_resource || !(r->flags & IORESOURCE_MEM)) continue;
        cursor = r->start;
        for (child = r->child; child; child = next) {
            next = child->sibling;
            if (cursor < child->start) {
                ret = add_block(r, cursor, child->start - 1);
                if (ret) return ret;
            }
            cursor = child->end + 1;
        }
        ret = add_block(r, cursor, r->end);
        if (ret) return ret;
    }
    return 0;
}
static int bind_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
    return dev == mocks[0].ep ? pci_enable_device(dev) : -ENODEV;
}
static void bind_remove(struct pci_dev *dev) { pci_disable_device(dev); }
static const struct pci_device_id ids[] = {{ PCI_DEVICE(0x1b36, 0x0005) },{}};
static struct pci_driver busy_driver = {
    .name = "qemu-only-cohort-busy", .id_table = ids,
    .probe = bind_probe, .remove = bind_remove,
};
static int __init setup(void)
{
    struct resource *r, *parent, *high_parent = NULL;
    resource_size_t start;
    int i, j, ret;
    if (!dmi_match(DMI_SYS_VENDOR, "QEMU")) return -EPERM;
    for (i = 0; i < 2; i++) {
        struct mock *m = &mocks[i];
        m->root = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(i + 1, 0));
        m->ep = pci_get_domain_bus_and_slot(0, i + 1, 0);
        if (!m->root || !m->ep || m->ep->vendor != 0x1b36 || m->ep->dev.driver) return -ENODEV;
        pci_clear_master(m->ep);
        /* Move BAR0 into a small low MMIO window; do not leave a second large
         * low-address hole that could accidentally satisfy prefetch fallback. */
        for (j = 0; j <= 2; j += 2) {
            r = &m->ep->resource[j];
            if (r->child) return -EBUSY;
            if (r->parent) release_resource(r);
            if (j == 2) r->start = r->end = r->flags = 0;
        }
        r = &m->root->resource[PCI_BRIDGE_MEM_WINDOW];
        parent = r->parent; start = r->start;
        if (!parent || r->child) return -EINVAL;
        release_resource(r);
        r->end = start + (1ULL << 20) - 1;
        if (request_resource(parent, r)) return -EBUSY;
        m->ep->resource[0].start = start;
        m->ep->resource[0].end = start + 4095;
        if (request_resource(r, &m->ep->resource[0])) return -EBUSY;
        pci_write_config_dword(m->ep, PCI_BASE_ADDRESS_0, lower_32_bits(start));
        r = &m->root->resource[PCI_BRIDGE_PREF_MEM_WINDOW];
        if (r->child) return -EBUSY;
        if (r->parent) release_resource(r);
        r->start = r->end = r->flags = 0;
    }
    pci_bus_for_each_resource(mocks[0].root->bus, r, i)
        if (r && (r->flags & IORESOURCE_MEM) && r->start >= (1ULL << 32)) { high_parent = r; break; }
    if (!high_parent) return -ENOSPC;
    pool.name = "qemu-only-cohort-pool"; pool.flags = IORESOURCE_MEM;
    ret = allocate_resource(high_parent, &pool, (healthy ? 128ULL : 80ULL) << 20,
                            high_parent->start, high_parent->end, 64ULL << 20, NULL, NULL);
    if (ret) return ret;
    start = pool.start; release_resource(&pool);
    for (i = 0; i < 2; i++) {
        struct mock *m = &mocks[i];
        r = &m->root->resource[PCI_BRIDGE_PREF_MEM_WINDOW];
        r->start = start + (i ? 64ULL << 20 : 0);
        r->end = r->start + ((i && !healthy ? 16ULL : 64ULL) << 20) - 1;
        r->flags = IORESOURCE_MEM | IORESOURCE_MEM_64 | IORESOURCE_PREFETCH | PCI_PREF_RANGE_TYPE_64;
        if (request_resource(high_parent, r)) return -EBUSY;
        if (!i || healthy) {
            struct resource *bar = &m->ep->resource[2];
            bar->start = r->start; bar->end = bar->start + (64ULL << 20) - 1;
            bar->flags = IORESOURCE_MEM | IORESOURCE_MEM_64 | IORESOURCE_PREFETCH | 12;
            if (request_resource(r, bar)) return -EBUSY;
            m->address = lower_32_bits(bar->start); m->high = upper_32_bits(bar->start);
        }
        m->ctrl = BIT(5) | (6 << 8) | 2;
        m->cfg_size = m->ep->cfg_size; m->ep->cfg_size = PCI_CFG_SPACE_EXP_SIZE;
        m->hotplug = m->root->is_hotplug_bridge; m->root->is_hotplug_bridge = false;
        m->old_ops = m->ep->bus->ops; m->ops = *m->old_ops;
        m->ops.read = cfg_read; m->ops.write = cfg_write; m->ep->bus->ops = &m->ops;
    }
    ret = exhaust(mocks[0].root->bus);
    if (ret) return ret;
    if (busy) { ret = pci_register_driver(&busy_driver); if (ret) return ret; }
    return 0;
}
static int get_controls(char *buf, const struct kernel_param *kp)
{
    return scnprintf(buf, PAGE_SIZE, "%u %u\n", mocks[0].ctrl, mocks[1].ctrl);
}
static const struct kernel_param_ops control_ops = {.get = get_controls};
module_param_cb(controls, &control_ops, NULL, 0444);
static void __exit teardown(void)
{
    int i;
    if (busy) pci_unregister_driver(&busy_driver);
    for (i = 0; i < 2; i++) {
        struct mock *m = &mocks[i];
        m->ep->bus->ops = m->old_ops; m->ep->cfg_size = m->cfg_size;
        m->root->is_hotplug_bridge = m->hotplug;
        pci_dev_put(m->ep); pci_dev_put(m->root);
    }
    while (nr_blocks) release_resource(&blocks[--nr_blocks]);
}
module_init(setup);
module_exit(teardown);
MODULE_LICENSE("GPL");
