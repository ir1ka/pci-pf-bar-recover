/* SPDX-License-Identifier: GPL-2.0 */
/* Advisory address-window demand only. No vendor MMIO, capability unlock or
 * device expansion. The eventual driver owns the actual large BAR resize. */
static resource_size_t bar_reservation_goal(struct pci_dev *dev, int bar,
                                           resource_size_t actual)
{
    if (!cmp_reserve || dev->vendor != 0x10de || dev->is_virtfn ||
        pci_is_bridge(dev) || (dev->device != 0x20c2 && dev->device != 0x2082) ||
        bar != 1 || !in_scope(dev))
        return 0;
    return max_t(resource_size_t, actual, 1ULL << 36);
}

/* Reservation hints use the ReBAR power-of-two granularity, not a claim about
 * hidden hardware capabilities. Hardware shrinking uses advertised bits only. */
static resource_size_t bar_reservation_next(resource_size_t planned,
                                           resource_size_t actual)
{
    if (planned <= actual)
        return 0;
    return max_t(resource_size_t, actual, planned >> 1);
}
