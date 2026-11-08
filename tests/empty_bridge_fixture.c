// SPDX-License-Identifier: GPL-2.0
/* QEMU-only: suppress optional hotplug reserves on an otherwise empty port,
 * matching a physical slot with no downstream I/O BAR demand. */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/dmi.h>
static struct pci_dev *port;
static bool was_hotplug;
static int __init setup(void)
{
    if (!dmi_match(DMI_SYS_VENDOR, "QEMU"))
        return -EPERM;
    port = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(5, 0));
    if (!port || !port->subordinate || !list_empty(&port->subordinate->devices)) {
        pci_dev_put(port);
        return -ENODEV;
    }
    was_hotplug = port->is_hotplug_bridge;
    port->is_hotplug_bridge = false;
    return 0;
}
static void __exit teardown(void)
{
    port->is_hotplug_bridge = was_hotplug;
    pci_dev_put(port);
}
module_init(setup);
module_exit(teardown);
MODULE_LICENSE("GPL");
