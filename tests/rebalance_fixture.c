// SPDX-License-Identifier: GPL-2.0
/* QEMU-only test setup: erase the 64 MiB BAR and shrink its existing bridge
 * chain to 32 MiB. Optional blockers exhaust all other root memory space.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/dmi.h>
#include <linux/ioport.h>
#include <linux/slab.h>
static bool block_space;
module_param(block_space, bool, 0400);
static struct pci_dev *bridges[32];
static unsigned int count;
static struct resource *old_parents[32];
static unsigned long old_flags[32];
static struct resource blocks[256];
static unsigned int nr_blocks;
static int collect(struct pci_dev *p, void *unused)
{
    if (pci_is_bridge(p) && count < ARRAY_SIZE(bridges))
        bridges[count++] = p;
    else if (!pci_is_bridge(p) && !p->dev.driver) {
        int i;
        pci_clear_master(p);
        /* Fault injection also erases idle prefetchable PF/VF descriptors,
         * allowing a consistent 32 MiB window hierarchy to be constructed.
         */
        for (i = 0; i <= PCI_IOV_RESOURCE_END; i++) {
            struct resource *r = &p->resource[i];
            if (i < PCI_IOV_RESOURCES && !(r->flags & IORESOURCE_PREFETCH))
                continue;
            if (r->child)
                return -EBUSY;
            if (r->parent)
                release_resource(r);
            r->start = r->end = r->flags = 0;
        }
    }
    return 0;
}
static void unblock(void)
{
    while (nr_blocks)
        release_resource(&blocks[--nr_blocks]);
}
static int add_block(struct resource *parent, resource_size_t start, resource_size_t end)
{
    struct resource *r;
    if (start > end)
        return 0;
    if (nr_blocks == ARRAY_SIZE(blocks))
        return -ENOMEM;
    r = &blocks[nr_blocks];
    r->name = "test-only-no-rebalance-space";
    r->start = start;
    r->end = end;
    r->flags = IORESOURCE_MEM;
    if (request_resource(parent, r))
        return -EBUSY;
    nr_blocks++;
    return 0;
}
static int exhaust(struct pci_bus *bus)
{
    struct resource *r, *child, *next;
    resource_size_t cursor;
    int ret;
    pci_bus_for_each_resource(bus, r) {
        if (!r || r == &iomem_resource || !(r->flags & IORESOURCE_MEM))
            continue;
        cursor = r->start;
        child = r->child;
        while (child) {
            next = child->sibling;
            if (cursor < child->start) {
                ret = add_block(r, cursor, child->start - 1);
                if (ret)
                    return ret;
            }
            cursor = child->end + 1;
            child = next;
        }
        ret = add_block(r, cursor, r->end);
        if (ret)
            return ret;
    }
    return 0;
}
static int __init setup(void)
{
    struct pci_dev *root, *wide;
    struct resource *r;
    struct pci_bus_region b;
    int i, ret = 0;
    if (!dmi_match(DMI_SYS_VENDOR, "QEMU"))
        return -EPERM;
    root = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(3, 1));
    wide = pci_get_device(0x1b36, 0x0005, NULL);
    if (!root || !wide || wide->dev.driver) {
        ret = -ENODEV;
        goto out;
    }
    if (wide->resource[2].parent)
        release_resource(&wide->resource[2]);
    wide->resource[2].start = wide->resource[2].end = wide->resource[2].flags = 0;
    bridges[count++] = root;
    pci_walk_bus(root->subordinate, collect, NULL);
    {
        resource_size_t base = root->resource[PCI_BRIDGE_PREF_MEM_WINDOW].start;
        for (i = 0; i < count; i++) {
            r = &bridges[i]->resource[PCI_BRIDGE_PREF_MEM_WINDOW];
            old_parents[i] = r->parent;
            old_flags[i] = r->flags;
        }
        for (i = count - 1; i >= 0; i--) {
            r = &bridges[i]->resource[PCI_BRIDGE_PREF_MEM_WINDOW];
            if (r->child) {
                ret = -EBUSY;
                goto out;
            }
            if (r->parent)
                release_resource(r);
        }
        for (i = 0; i < count; i++) {
            if (!old_parents[i])
                continue;
            r = &bridges[i]->resource[PCI_BRIDGE_PREF_MEM_WINDOW];
            r->start = base;
            r->end = base + (32ULL << 20) - 1;
            r->flags = old_flags[i];
            ret = request_resource(old_parents[i], r);
            if (ret)
                goto out;
            pcibios_resource_to_bus(bridges[i]->bus, &b, r);
            pci_write_config_dword(bridges[i], PCI_PREF_LIMIT_UPPER32, 0);
            pci_write_config_dword(bridges[i], PCI_PREF_MEMORY_BASE,
                                  ((b.start >> 16) & 0xfff0) | (b.end & 0xfff00000));
            pci_write_config_dword(bridges[i], PCI_PREF_BASE_UPPER32, upper_32_bits(b.start));
            pci_write_config_dword(bridges[i], PCI_PREF_LIMIT_UPPER32, upper_32_bits(b.end));
        }
    }
    if (block_space)
        ret = exhaust(root->bus);
out:
    pci_dev_put(wide);
    pci_dev_put(root);
    if (ret)
        unblock();
    return ret;
}
static void __exit teardown(void) { unblock(); }
module_init(setup);
module_exit(teardown);
MODULE_LICENSE("GPL");
