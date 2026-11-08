// SPDX-License-Identifier: GPL-2.0
/* TEST ONLY: run inside a disposable QEMU guest, never on a physical host. */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/dmi.h>
#include <linux/ioport.h>
static struct resource blocker;
static bool blocked;
static struct pci_dev *bridges[32];
static unsigned int bridge_count;
static int remember_bridge(struct pci_dev *p, void *unused)
{
    if (pci_is_bridge(p) && bridge_count < ARRAY_SIZE(bridges))
        bridges[bridge_count++] = p;
    return 0;
}
static int __init inject(void)
{
    unsigned int buses[] = {4, 7, 8};
    int i, j;
    struct pci_dev *p;
    struct resource *parent = NULL;
    if (!dmi_match(DMI_SYS_VENDOR, "QEMU"))
        return -EPERM;
    for (i = 0; i < 3; i++) {
        p = pci_get_domain_bus_and_slot(0, buses[i], 0);
        if (!p || p->vendor != 0x8086 || p->device != 0x10c9 || p->dev.driver) {
            pci_dev_put(p);
            return -EINVAL;
        }
        pci_clear_master(p);
        if (buses[i] == 7)
            parent = p->resource[0].parent;
        for (j = 0; j < PCI_STD_NUM_BARS; j++) {
            struct resource *r = &p->resource[j];
            if (r->parent)
                release_resource(r);
            r->start = r->end = r->flags = 0;
        }
        if (buses[i] == 7 || buses[i] == 4) {
            for (j = PCI_IOV_RESOURCES; j <= PCI_IOV_RESOURCE_END; j++) {
                struct resource *r = &p->resource[j];
                if (r->parent)
                    release_resource(r);
                r->start = r->end = r->flags = 0;
            }
        }
        if (buses[i] == 7) {
            for (j = PCI_ROM_RESOURCE; j <= PCI_IOV_RESOURCE_END; j++) {
                struct resource *r = &p->resource[j];
                if (r->parent)
                    release_resource(r);
                r->start = r->end = r->flags = 0;
            }
        }
        pci_dev_put(p);
    }
    if (!parent)
        return -EINVAL;
    blocker.name = "test-only-exhausted-window";
    blocker.start = parent->start;
    blocker.end = parent->end;
    blocker.flags = parent->flags & (IORESOURCE_MEM | IORESOURCE_PREFETCH);
    if (request_resource(parent, &blocker))
        return -EBUSY;
    blocked = true;
    p = pci_get_device(0x1b36, 0x0005, NULL);
    if (!p || p->dev.driver) {
        pci_dev_put(p);
        release_resource(&blocker);
        blocked = false;
        return -EINVAL;
    }
    if (p->resource[2].parent)
        release_resource(&p->resource[2]);
    p->resource[2].start = p->resource[2].end = p->resource[2].flags = 0;
    pci_dev_put(p);
    p = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(3, 1));
    if (!p) {
        release_resource(&blocker);
        blocked = false;
        return -ENODEV;
    }
    bridges[bridge_count++] = p;
    pci_walk_bus(p->subordinate, remember_bridge, NULL);
    for (i = bridge_count - 1; i >= 0; i--) {
        struct resource *r = &bridges[i]->resource[PCI_BRIDGE_PREF_MEM_WINDOW];
        if (r->child) {
            pci_dev_put(p);
            release_resource(&blocker);
            blocked = false;
            return -EBUSY;
        }
        if (r->parent)
            release_resource(r);
        r->start = r->end = r->flags = 0;
        pci_write_config_dword(bridges[i], PCI_PREF_LIMIT_UPPER32, 0);
        pci_write_config_dword(bridges[i], PCI_PREF_MEMORY_BASE, 0x0000fff0);
        pci_write_config_dword(bridges[i], PCI_PREF_BASE_UPPER32, 0);
    }
    pci_dev_put(p);
    return 0;
}
static void __exit undo(void)
{
    if (blocked)
        release_resource(&blocker);
}
module_init(inject);
module_exit(undo);
MODULE_LICENSE("GPL");
