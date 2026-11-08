// SPDX-License-Identifier: GPL-2.0
/* One-shot PF BAR recovery: conservative first, then optional idle-subtree
 * fallback. Never move resources of driver-bound, bus-mastering or VF-active
 * endpoints; idle fallback rechecks and locks the entire affected tree.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/ioport.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/list_sort.h>
#include <linux/device/driver.h>
#include <linux/vgaarb.h>
#include <linux/ktime.h>

/* Bounded NDJSON, readable before unload; dropped records are explicit. */
static char result_records[4000];
static unsigned int result_dropped;
module_param_string(result_records, result_records, sizeof(result_records), 0444);
module_param(result_dropped, uint, 0444);
static void pf_result(struct pci_dev *dev, const char *kind, const char *stage,
                      int rc, resource_size_t actual, resource_size_t goal)
{
    char record[256];
    size_t used = strlen(result_records);
    int n = scnprintf(record, sizeof(record),
        "{\"bdf\":\"%s\",\"kind\":\"%s\",\"stage\":\"%s\",\"rc\":%d,\"actual\":%llu,\"goal\":%llu}\n",
        pci_name(dev), kind, stage, rc,
        (unsigned long long)actual, (unsigned long long)goal);
    if (used + n >= sizeof(result_records)) { result_dropped++; return; }
    memcpy(result_records + used, record, n + 1);
}

static bool execute;
module_param(execute, bool, 0400);
MODULE_PARM_DESC(execute, "Explicitly execute recovery; default false is inert");
static bool debug;
module_param(debug, bool, 0400);
MODULE_PARM_DESC(debug, "Opt-in bounded diagnostic tracing; does not relax safety checks");
static unsigned int debug_limit = 2048;
module_param(debug_limit, uint, 0400);
MODULE_PARM_DESC(debug_limit, "Maximum detailed trace records, 1..16384");
static unsigned int debug_records, debug_dropped;
static u64 debug_run;
static char *ensure_id = "";
module_param(ensure_id, charp, 0400);
MODULE_PARM_DESC(ensure_id, "Opt-in all-matching PF recovery across their root ports, vvvv:dddd");
static unsigned int ensure_vendor, ensure_device, ensure_matched, ensure_complete;
module_param_named(ensure_matched, ensure_matched, uint, 0444);
module_param_named(ensure_complete, ensure_complete, uint, 0444);
static bool ensure_matches(struct pci_dev *dev)
{
    return dev->vendor == ensure_vendor && dev->device == ensure_device && !dev->is_virtfn;
}
static unsigned int cmp_reserved_count;
module_param_named(cmp_reserved, cmp_reserved_count, uint, 0444);
MODULE_PARM_DESC(cmp_reserved, "CMP BAR window plans committed; actual BAR size is unchanged unless standard shrinking was needed");
module_param_named(cmp_prepared, cmp_reserved_count, uint, 0444);
MODULE_PARM_DESC(cmp_prepared, "Legacy counter alias for cmp_reserved; does not mean hardware was unlocked");
static bool cmp_reserve = true;
module_param(cmp_reserve, bool, 0400);
MODULE_PARM_DESC(cmp_reserve, "Default-on advisory 64GiB BAR1 windows for NVIDIA 10de:20c2/2082 only; never unlock/expand devices");
module_param_named(cmp_unlock, cmp_reserve, bool, 0400);
MODULE_PARM_DESC(cmp_unlock, "Legacy alias for cmp_reserve only; vendor unlocking has been removed");
static bool conservative_only;
module_param(conservative_only, bool, 0400);
MODULE_PARM_DESC(conservative_only, "Disable automatic idle-subtree rebalance/ReBAR fallback");
static bool rebalance;
module_param(rebalance, bool, 0400);
MODULE_PARM_DESC(rebalance, "EXPERIMENTAL idle-subtree transaction; requires explicit ports");
static char *ports = "";
module_param(ports, charp, 0400);
MODULE_PARM_DESC(ports, "Comma-separated root port BDFs; empty selects all devices");
static struct pci_dev *selected_ports[64];
static int nr_ports;
static unsigned int restored_count, failed_count, skipped_count;
static char unavailable_devices[4096];
module_param_string(unavailable_devices, unavailable_devices, sizeof(unavailable_devices), 0444);
MODULE_PARM_DESC(unavailable_devices, "Read-only list of conservative candidates that failed presence checks");
module_param_named(restored, restored_count, uint, 0444);
module_param_named(failed, failed_count, uint, 0444);
module_param_named(skipped, skipped_count, uint, 0444);

struct bar_save {
    struct resource old;
    u32 lo, hi;
    resource_size_t size;
    bool changed, wide, attempted;
};
struct candidate {
    struct list_head link;
    struct pci_dev *dev;
    struct bar_save bars[PCI_STD_NUM_BARS];
    bool excluded;
};
struct window_save {
    struct list_head link;
    struct pci_dev *dev;
    struct resource old[3];
};
struct group {
    struct list_head link, devices, windows;
    struct pci_dev *root;
    struct pci_bus *bus;
    int window_error;
    unsigned int failed_candidates;
};
static LIST_HEAD(groups);

/* Trace arguments are not evaluated when disabled. No additional PCI reads or
 * writes are performed for tracing: resources and existing operation results only.
 */
static bool trace_allowed(void)
{
    if (!debug)
        return false;
    if (debug_records >= debug_limit) {
        debug_dropped++;
        return false;
    }
    debug_records++;
    return true;
}
#define pf_trace(dev, fmt, ...) do { \
    if (trace_allowed()) \
        pci_info(dev, "pf-bar-recover: debug run=%llu " fmt, \
                 (unsigned long long)debug_run, ##__VA_ARGS__); \
} while (0)

static void trace_resources(struct pci_dev *dev, const char *stage)
{
    int i;
    if (!debug)
        return;
    for (i = 0; i < PCI_NUM_RESOURCES; i++) {
        struct resource *r = &dev->resource[i];
        if (r->flags || r->parent || (i < PCI_STD_NUM_BARS))
            pf_trace(dev, "stage=%s resource=%d value=%pR flags=%#lx assigned=%u child=%u\n",
                     stage, i, r, r->flags, !!r->parent, !!r->child);
    }
}

static void trace_apertures(struct pci_dev *root)
{
    struct resource *r;
    int i;
    if (!debug || !root)
        return;
    pci_bus_for_each_resource(root->bus, r, i) {
        if (r)
            pf_trace(root, "stage=parent-aperture index=%d value=%pR flags=%#lx\n", i, r, r->flags);
    }
}

static void note_unavailable(struct pci_dev *dev)
{
    size_t used = strlen(unavailable_devices);
    if (used + strlen(pci_name(dev)) + 2 < sizeof(unavailable_devices))
        scnprintf(unavailable_devices + used, sizeof(unavailable_devices) - used,
                  "%s%s", used ? "," : "", pci_name(dev));
    else
        pci_warn_once(dev, "pf-bar-recover: unavailable-device list truncated; no automatic retry for omitted devices\n");
}

static struct pci_dev *top_bridge(struct pci_dev *dev)
{
    struct pci_bus *bus = dev->bus;
    struct pci_dev *top = NULL;
    while (bus && bus->self) {
        top = bus->self;
        bus = bus->parent;
    }
    return top;
}

static int parse_ports(void)
{
    char *text, *cursor, *token;
    unsigned int domain, bus, slot, func;
    int n, ret = 0;
    struct pci_dev *dev;
    if (!*ports)
        return 0;
    text = kstrdup(ports, GFP_KERNEL);
    if (!text)
        return -ENOMEM;
    cursor = text;
    while ((token = strsep(&cursor, ","))) {
        n = 0;
        if (nr_ports == ARRAY_SIZE(selected_ports) ||
            sscanf(token, "%x:%x:%x.%x%n", &domain, &bus, &slot, &func, &n) != 4 ||
            token[n] || domain > 0xffff || bus > 255 || slot > 31 || func > 7) {
            ret = -EINVAL;
            break;
        }
        dev = pci_get_domain_bus_and_slot(domain, bus, PCI_DEVFN(slot, func));
        if (!dev || !pci_is_pcie(dev) ||
            pci_pcie_type(dev) != PCI_EXP_TYPE_ROOT_PORT || !dev->subordinate) {
            pci_dev_put(dev);
            ret = -EINVAL;
            break;
        }
        selected_ports[nr_ports++] = dev;
    }
    kfree(text);
    return ret;
}

static int parse_ensure_id(void)
{
    struct pci_dev *dev = NULL, *root;
    int n = 0, i;
    if (!*ensure_id)
        return 0;
    if (*ports || rebalance || conservative_only || strlen(ensure_id) != 9 ||
        strspn(ensure_id, "0123456789abcdefABCDEF") != 4 ||
        ensure_id[4] != ':' || strspn(ensure_id + 5, "0123456789abcdefABCDEF") != 4 ||
        sscanf(ensure_id, "%4x:%4x%n", &ensure_vendor, &ensure_device, &n) != 2 || n != 9)
        return -EINVAL;
    for_each_pci_dev(dev) {
        if (!ensure_matches(dev))
            continue;
        ensure_matched++;
        root = top_bridge(dev);
        if (!root || !root->subordinate) {
            pci_warn(dev, "ensure-id: target has no supported root port; no changes\n");
            pci_dev_put(dev);
            return -EOPNOTSUPP;
        }
        for (i = 0; i < nr_ports; i++)
            if (selected_ports[i] == root)
                break;
        if (i < nr_ports)
            continue;
        if (nr_ports == ARRAY_SIZE(selected_ports)) {
            pci_dev_put(dev);
            return -E2BIG;
        }
        selected_ports[nr_ports++] = pci_dev_get(root);
    }
    pr_info("pci_pf_bar_recover: ensure-id=%s matched=%u roots=%d\n", ensure_id, ensure_matched, nr_ports);
    return 0;
}

static bool in_scope(struct pci_dev *dev)
{
    int i;
    struct pci_dev *root;
    if (*ensure_id && !ensure_matches(dev))
        return false;
    if (!nr_ports)
        return !*ensure_id;
    root = top_bridge(dev);
    for (i = 0; i < nr_ports; i++)
        if (selected_ports[i] == root)
            return true;
    return false;
}

/* Probe only an unclaimed BAR while the endpoint decoders are disabled.
 * All config writes used by sizing are restored before returning.
 */
static int probe_bar(struct pci_dev *dev, int bar, struct bar_save *save)
{
    u32 lo, hi = 0, lowmask, highmask = 0;
    u64 mask, size;
    unsigned long flags;
    int offset = PCI_BASE_ADDRESS_0 + 4 * bar, ret;
    struct resource *res = &dev->resource[bar];
    if (pci_read_config_dword(dev, offset, &lo))
        return -EIO;
    pf_trace(dev, "stage=probe-input bar=%d raw_lo=%#010x descriptor=%pR assigned=%u\n", bar, lo, res, !!res->parent);
    save->wide = !(lo & PCI_BASE_ADDRESS_SPACE_IO) &&
                 (lo & PCI_BASE_ADDRESS_MEM_TYPE_MASK) == PCI_BASE_ADDRESS_MEM_TYPE_64;
    if (res->parent || (res->flags & IORESOURCE_PCI_FIXED))
        return 0;
    if (res->child)
        return -EBUSY;
    if (save->wide && (bar == (dev->hdr_type == PCI_HEADER_TYPE_BRIDGE ? 1 : PCI_STD_NUM_BARS - 1) ||
        pci_read_config_dword(dev, offset + 4, &hi)))
        return -EIO;
    save->lo = lo;
    save->hi = hi;
    ret = pci_write_config_dword(dev, offset, ~0U);
    if (save->wide)
        ret |= pci_write_config_dword(dev, offset + 4, ~0U);
    ret |= pci_read_config_dword(dev, offset, &lowmask);
    if (save->wide)
        ret |= pci_read_config_dword(dev, offset + 4, &highmask);
    if (save->wide)
        ret |= pci_write_config_dword(dev, offset + 4, hi);
    ret |= pci_write_config_dword(dev, offset, lo);
    pf_trace(dev, "stage=probe-restore bar=%d rc=%d wide=%u saved_lo=%#010x saved_hi=%#010x\n", bar, ret, save->wide, lo, hi);
    if (ret)
        return -EIO;
    pf_trace(dev, "stage=probe-mask bar=%d low=%#010x high=%#010x\n", bar, lowmask, highmask);
    if (!(lowmask & (lo & PCI_BASE_ADDRESS_SPACE_IO ? PCI_BASE_ADDRESS_IO_MASK : PCI_BASE_ADDRESS_MEM_MASK)) && !highmask)
        return 0; /* Unimplemented BAR. */
    if (lo & PCI_BASE_ADDRESS_SPACE_IO) {
        mask = lowmask & PCI_BASE_ADDRESS_IO_MASK;
        size = ((~mask) & 0xffffffffULL) + 1;
        flags = IORESOURCE_IO | (lo & ~PCI_BASE_ADDRESS_IO_MASK);
    } else {
        mask = lowmask & PCI_BASE_ADDRESS_MEM_MASK;
        if (save->wide)
            mask |= (u64)highmask << 32;
        size = save->wide ? ~mask + 1 : ((~mask) & 0xffffffffULL) + 1;
        flags = IORESOURCE_MEM | (lo & ~PCI_BASE_ADDRESS_MEM_MASK);
        if (lo & PCI_BASE_ADDRESS_MEM_PREFETCH)
            flags |= IORESOURCE_PREFETCH;
        if (save->wide)
            flags |= IORESOURCE_MEM_64;
    }
    if (!size || !is_power_of_2(size))
        return -EINVAL;
    save->old = *res;
    save->size = size;
    save->changed = true;
    res->start = 0;
    res->end = size - 1;
    res->flags = flags | IORESOURCE_SIZEALIGN | IORESOURCE_UNSET;
    pci_info(dev, "pf-bar-recover: discovered missing BAR%d size=%llu\n",
             bar, (unsigned long long)size);
    return 1;
}

/* Called only with the candidate's device lock held and no bound driver. */
static void undo_candidate(struct candidate *c)
{
    int i;
    u16 cmd;
    struct resource *r;
    pf_trace(c->dev, "stage=candidate-rollback begin\n");
    trace_resources(c->dev, "rollback-input");
    pci_read_config_word(c->dev, PCI_COMMAND, &cmd);
    pci_write_config_word(c->dev, PCI_COMMAND,
                          cmd & ~(PCI_COMMAND_MEMORY | PCI_COMMAND_IO));
    for (i = 0; i < PCI_STD_NUM_BARS; i++) {
        if (!c->bars[i].changed)
            continue;
        r = &c->dev->resource[i];
        if (r->parent)
            release_resource(r);
        *r = c->bars[i].old;
        if (c->bars[i].wide)
            pci_write_config_dword(c->dev, PCI_BASE_ADDRESS_0 + i * 4 + 4,
                                  c->bars[i].hi);
        pci_write_config_dword(c->dev, PCI_BASE_ADDRESS_0 + i * 4, c->bars[i].lo);
    }
    pci_write_config_word(c->dev, PCI_COMMAND, cmd);
    c->excluded = true;
    trace_resources(c->dev, "rollback-descriptors-restored");
}

static struct group *get_group(struct pci_dev *dev)
{
    struct group *g;
    struct pci_dev *root = top_bridge(dev);
    list_for_each_entry(g, &groups, link)
        if (g->root == root && (root || g->bus == dev->bus))
            return g;
    g = kzalloc(sizeof(*g), GFP_KERNEL);
    if (!g)
        return NULL;
    INIT_LIST_HEAD(&g->devices);
    INIT_LIST_HEAD(&g->windows);
    g->root = root;
    g->bus = root ? root->subordinate : dev->bus;
    list_add_tail(&g->link, &groups);
    return g;
}

static void collect_device(struct pci_dev *dev)
{
    struct candidate *c;
    struct group *g;
    u16 cmd = 0;
    int i, ret, count = 0, nr_bars, active_vfs, config_ret;
    bool present;
    if (!in_scope(dev) || dev->is_virtfn || (dev->hdr_type != PCI_HEADER_TYPE_NORMAL && dev->hdr_type != PCI_HEADER_TYPE_BRIDGE))
        return;
    if (!device_trylock(&dev->dev)) {
        skipped_count++;
        pci_info(dev, "pf-bar-recover: skip (device lock busy)\n");
        return;
    }
    pf_trace(dev, "stage=candidate id=%04x:%04x class=%06x driver=%s enabled=%u power=%d error_state=%d\n",
             dev->vendor, dev->device, dev->class,
             dev->dev.driver ? dev->dev.driver->name : "none",
             pci_is_enabled(dev), dev->current_state, dev->error_state);
    trace_resources(dev, "candidate-input");
    if (dev->dev.driver) {
        pf_result(dev, "skip", "driver", -EBUSY, 0, 0);
        skipped_count++;
        pci_info(dev, "pf-bar-recover: skip: bound driver=%s\n", dev->dev.driver->name);
        goto unlock;
    }
    active_vfs = pci_num_vf(dev);
    if (active_vfs) {
        pf_result(dev, "skip", "active-vfs", -EBUSY, 0, 0);
        skipped_count++;
        pci_info(dev, "pf-bar-recover: skip: active_vfs=%d\n", active_vfs);
        goto unlock;
    }
    present = pci_device_is_present(dev);
    if (!present) {
        pf_result(dev, "skip", "unavailable", -ENODEV, 0, 0);
        note_unavailable(dev);
        skipped_count++;
        pci_info(dev, "pf-bar-recover: skip: present=0 error_state=%d power_state=%d enabled=%u\n",
                 dev->error_state, dev->current_state, pci_is_enabled(dev));
        goto unlock;
    }
    config_ret = pci_read_config_word(dev, PCI_COMMAND, &cmd);
    if (config_ret) {
        pf_result(dev, "skip", "config-read", -EIO, 0, 0);
        skipped_count++;
        pci_info(dev, "pf-bar-recover: skip: PCI_COMMAND read failed config_ret=%d\n", config_ret);
        goto unlock;
    }
    pf_trace(dev, "stage=eligibility present=%u active_vfs=%d config_ret=%d command=%#06x\n", present, active_vfs, config_ret, cmd);
    if (cmd & PCI_COMMAND_MASTER) {
        pf_result(dev, "skip", "bus-master", -EBUSY, 0, 0);
        skipped_count++;
        pci_info(dev, "pf-bar-recover: skip: bus_master=1 command=%#06x enabled=%u\n",
                 cmd, pci_is_enabled(dev));
        goto unlock;
    }
    for (i = 0; i < PCI_STD_NUM_BARS; i++)
        if (dev->resource[i].child) {
            skipped_count++;
            pci_info(dev, "pf-bar-recover: skip (physical BAR has claimed subresources)\n");
            goto unlock;
        }
    c = kzalloc(sizeof(*c), GFP_KERNEL);
    if (!c) {
        skipped_count++;
        goto unlock;
    }
    c->dev = dev;
    if (pci_write_config_word(dev, PCI_COMMAND, cmd & ~(PCI_COMMAND_MEMORY | PCI_COMMAND_IO))) {
        kfree(c);
        failed_count++;
        pci_warn(dev, "pf-bar-recover: skip (cannot disable BAR decoding)\n");
        goto unlock;
    }
    nr_bars = dev->hdr_type == PCI_HEADER_TYPE_BRIDGE ? 2 : PCI_STD_NUM_BARS;
    for (i = 0; i < nr_bars; i++) {
        ret = probe_bar(dev, i, &c->bars[i]);
        if (ret < 0)
            break;
        count += ret;
        if (c->bars[i].wide)
            i++;
    }
    pci_write_config_word(dev, PCI_COMMAND, cmd);
    pf_trace(dev, "stage=probe-summary missing=%d rc=%d command_restored=%#06x\n", count, ret, cmd);
    if (ret < 0 || !count) {
        if (ret < 0) {
            undo_candidate(c);
            failed_count++;
            pci_warn(dev, "pf-bar-recover: skip (BAR probing failed: %d)\n", ret);
        }
        kfree(c);
        goto unlock;
    }
    g = get_group(dev);
    if (!g) {
        undo_candidate(c);
        kfree(c);
        failed_count++;
        goto unlock;
    }
    /* Only a recovering, unused PF loses its optional VF reservations.
     * A healthy PF or active VF owner is never modified, even with ports="".
     */
#ifdef CONFIG_PCI_IOV
    for (i = PCI_IOV_RESOURCES; i <= PCI_IOV_RESOURCE_END; i++) {
        struct resource *r = &dev->resource[i];
        if (r->child)
            continue;
        pf_trace(dev, "stage=release-vf-reservation index=%d value=%pR assigned=%u\n", i, r, !!r->parent);
        if (r->parent)
            release_resource(r);
        r->start = r->end = r->flags = 0;
    }
    if (dev->is_physfn)
        pci_sriov_set_totalvfs(dev, 0);
#endif
    pci_dev_get(dev);
    list_add_tail(&c->link, &g->devices);
    return; /* Keep device lock until allocation and final validation. */
unlock:
    device_unlock(&dev->dev);
}

static int save_window(struct pci_dev *dev, void *data)
{
    struct group *g = data;
    struct window_save *w;
    int i;
    if (!pci_is_bridge(dev) || dev->hdr_type != PCI_HEADER_TYPE_BRIDGE)
        return 0;
    w = kzalloc(sizeof(*w), GFP_KERNEL);
    if (!w) {
        g->window_error = -ENOMEM;
        return -ENOMEM;
    }
    w->dev = dev;
    trace_resources(dev, "window-snapshot");
    for (i = 0; i < 3; i++)
        w->old[i] = dev->resource[PCI_BRIDGE_RESOURCES + i];
    list_add_tail(&w->link, &g->windows);
    return 0;
}

static void reset_unassigned_windows(struct group *g)
{
    struct window_save *w;
    int i;
    list_for_each_entry(w, &g->windows, link)
        for (i = 0; i < 3; i++) {
            struct resource *r = &w->dev->resource[PCI_BRIDGE_RESOURCES + i];
            if (!r->parent)
                *r = w->old[i];
        }
}

/* Program ONLY windows that were previously unassigned. Existing assigned
 * bridge windows and all resources belonging to active endpoints stay fixed.
 */
/* Linux 7.x keeps typed descriptors for unused bridge windows and marks
 * them DISABLED. Their 0..0 range is not a one-byte allocation request.
 * Older kernels may clear flags instead; accept both representations.
 */
static bool window_assignable(const struct resource *r)
{
    return (r->flags & (IORESOURCE_MEM | IORESOURCE_IO)) &&
           !(r->flags & IORESOURCE_DISABLED);
}

static int program_windows(struct group *g)
{
    struct window_save *w;
    struct pci_bus_region b;
    struct resource *r;
    u32 lower;
    u16 cmd, old_cmd;
    int i, ret = 0;
    list_for_each_entry(w, &g->windows, link) {
        pci_read_config_word(w->dev, PCI_COMMAND, &cmd);
        old_cmd = cmd;
        for (i = 0; i < 3; i++) {
            r = &w->dev->resource[PCI_BRIDGE_RESOURCES + i];
            if (w->old[i].parent || !r->parent || !window_assignable(r))
                continue;
            pcibios_resource_to_bus(w->dev->bus, &b, r);
            if (i == 0) {
                unsigned int mask = w->dev->io_window_1k ? PCI_IO_1K_RANGE_MASK : PCI_IO_RANGE_MASK;
                lower = ((b.start >> 8) & mask) | (((b.end >> 8) & mask) << 8);
                ret |= pci_write_config_dword(w->dev, PCI_IO_BASE_UPPER16, 0xffff);
                ret |= pci_write_config_word(w->dev, PCI_IO_BASE, lower);
                ret |= pci_write_config_dword(w->dev, PCI_IO_BASE_UPPER16,
                                              (b.end & 0xffff0000) | (b.start >> 16));
                cmd |= PCI_COMMAND_IO;
            } else {
                lower = ((b.start >> 16) & 0xfff0) | (b.end & 0xfff00000);
                if (i == 2) {
                    ret |= pci_write_config_dword(w->dev, PCI_PREF_LIMIT_UPPER32, 0);
                    ret |= pci_write_config_dword(w->dev, PCI_PREF_MEMORY_BASE, lower);
                    ret |= pci_write_config_dword(w->dev, PCI_PREF_BASE_UPPER32,
                                                  (r->flags & IORESOURCE_MEM_64) ? upper_32_bits(b.start) : 0);
                    ret |= pci_write_config_dword(w->dev, PCI_PREF_LIMIT_UPPER32,
                                                  (r->flags & IORESOURCE_MEM_64) ? upper_32_bits(b.end) : 0);
                } else {
                    ret |= pci_write_config_dword(w->dev, PCI_MEMORY_BASE, lower);
                }
                cmd |= PCI_COMMAND_MEMORY;
            }
            pci_info(w->dev, "pf-bar-recover: new window %pR\n", r);
        }
        if (cmd != old_cmd)
            ret |= pci_write_config_word(w->dev, PCI_COMMAND, cmd);
    }
    return ret ? -EIO : 0;
}

static unsigned int bridge_depth(struct pci_dev *dev)
{
    unsigned int depth = 0;
    while ((dev = pci_upstream_bridge(dev)))
        depth++;
    return depth;
}

static int window_order(void *unused, const struct list_head *a,
                        const struct list_head *b)
{
    const struct window_save *wa = list_entry(a, struct window_save, link);
    const struct window_save *wb = list_entry(b, struct window_save, link);
    unsigned int da = bridge_depth(wa->dev), db = bridge_depth(wb->dev);
    resource_size_t sa = 0, sb = 0;
    int i;
    if (da != db)
        return da > db ? 1 : -1;
    for (i = 0; i < 3; i++) {
        const struct resource *ra = &wa->dev->resource[PCI_BRIDGE_RESOURCES + i];
        const struct resource *rb = &wb->dev->resource[PCI_BRIDGE_RESOURCES + i];
        if (!ra->parent && ra->flags)
            sa = max(sa, resource_size(ra));
        if (!rb->parent && rb->flags)
            sb = max(sb, resource_size(rb));
    }
    return sa < sb ? 1 : sa > sb ? -1 : 0;
}

static void recover_group(struct group *g)
{
    struct candidate *c, *largest;
    struct window_save *w;
    resource_size_t biggest;
    int i, ret, alloc_ret;
    unsigned int pass = 0;
    unsigned long failed_type = 0;
    bool complete;
    if (g->root) {
        pf_trace(g->root, "stage=group-begin\n");
        trace_apertures(g->root);
        ret = save_window(g->root, g);
        if (!ret)
            pci_walk_bus(g->bus, save_window, g);
        if (ret || g->window_error)
            goto fail_group;
        /* Size all missing BARs together. If the root aperture cannot fit
         * this batch, drop its largest request and retry smaller devices.
         * No assigned window is released or resized in these retries.
         */
        for (;;) {
            reset_unassigned_windows(g);
            pci_bus_size_bridges(g->bus);
            pf_trace(g->root, "stage=window-size pass=%u\n", ++pass);
            trace_resources(g->root, "window-sized");
            ret = 0;
            failed_type = 0;
            for (i = PCI_BRIDGE_RESOURCES; i <= PCI_BRIDGE_RESOURCE_END; i++) {
                struct resource *r = &g->root->resource[i];
                if (!r->parent && window_assignable(r)) {
                    alloc_ret = pci_assign_resource(g->root, i);
                    pf_trace(g->root, "stage=root-window-assign index=%d rc=%d value=%pR assigned=%u\n", i, alloc_ret, r, !!r->parent);
                    if (alloc_ret) {
                        if (!failed_type)
                            failed_type = r->flags & (IORESOURCE_IO | IORESOURCE_MEM | IORESOURCE_PREFETCH);
                        ret = -ENOSPC;
                    }
                }
            }
            if (!ret)
                break;
            largest = NULL;
            biggest = 0;
            list_for_each_entry(c, &g->devices, link) {
                if (c->excluded)
                    continue;
                for (i = 0; i < PCI_STD_NUM_BARS; i++)
                    if (c->bars[i].changed && c->bars[i].size > biggest &&
                        (c->dev->resource[i].flags &
                         (IORESOURCE_IO | IORESOURCE_MEM | IORESOURCE_PREFETCH)) == failed_type) {
                        largest = c;
                        biggest = c->bars[i].size;
                    }
            }
            if (!largest)
                break;
            pci_warn(largest->dev, "pf-bar-recover: skip (root aperture cannot fit batch; largest request removed)\n");
            undo_candidate(largest);
            failed_count++;
        }
    }
    /* Do not use a whole-bus endpoint allocator: it could also assign
     * unclaimed resources on a driver-bound non-candidate. Allocate only
     * new bridge windows and the specific BARs we probed and locked.
     */
    list_sort(NULL, &g->windows, window_order);
    list_for_each_entry(w, &g->windows, link)
        for (i = 0; i < 3; i++) {
            struct resource *r = &w->dev->resource[PCI_BRIDGE_RESOURCES + i];
            if (!r->parent && window_assignable(r)) {
                alloc_ret = pci_assign_resource(w->dev, PCI_BRIDGE_RESOURCES + i);
                pf_trace(w->dev, "stage=child-window-assign index=%d rc=%d value=%pR\n", PCI_BRIDGE_RESOURCES + i, alloc_ret, r);
                if (alloc_ret)
                    pci_warn(w->dev, "pf-bar-recover: window allocation failed; continuing\n");
            }
        }
    for (;;) {
        struct candidate *chosen = NULL;
        int bar = -1;
        biggest = 0;
        list_for_each_entry(c, &g->devices, link) {
            if (c->excluded)
                continue;
            for (i = 0; i < PCI_STD_NUM_BARS; i++)
                if (c->bars[i].changed && !c->bars[i].attempted &&
                    c->bars[i].size > biggest) {
                    chosen = c;
                    bar = i;
                    biggest = c->bars[i].size;
                }
        }
        if (!chosen)
            break;
        chosen->bars[bar].attempted = true;
        alloc_ret = pci_assign_resource(chosen->dev, bar);
        pf_trace(chosen->dev, "stage=pf-assign bar=%d requested=%llu rc=%d result=%pR assigned=%u\n", bar, (unsigned long long)biggest, alloc_ret, &chosen->dev->resource[bar], !!chosen->dev->resource[bar].parent);
        if (alloc_ret)
            pci_warn(chosen->dev, "pf-bar-recover: BAR%d allocation failed; continuing\n", bar);
    }
    if (program_windows(g))
        goto fail_group;
    list_for_each_entry(c, &g->devices, link) {
        if (c->excluded)
            continue;
        complete = true;
        for (i = 0; i < PCI_STD_NUM_BARS; i++) {
            struct resource *r = &c->dev->resource[i];
            if (c->bars[i].changed && (!r->parent || (r->flags & IORESOURCE_UNSET)))
                complete = false;
        }
        trace_resources(c->dev, "candidate-final");
        pf_trace(c->dev, "stage=candidate-result complete=%u\n", complete);
        if (complete) {
            restored_count++;
            pf_result(c->dev, "device", "commit", 0, 0, 0);
            pci_info(c->dev, "pf-bar-recover: restored missing physical BARs\n");
        } else {
            undo_candidate(c);
            failed_count++;
            pf_result(c->dev, "device", "allocate", -ENOSPC, 0, 0);
            pci_warn(c->dev, "pf-bar-recover: skip (allocation failed; remaining devices continue)\n");
        }
    }
    /* Newly sized but unassigned windows have no live resource to expose. */
    list_for_each_entry(w, &g->windows, link)
        for (i = 0; i < 3; i++)
            if (!w->dev->resource[PCI_BRIDGE_RESOURCES + i].parent)
                w->dev->resource[PCI_BRIDGE_RESOURCES + i] = w->old[i];
    return;
fail_group:
    list_for_each_entry(c, &g->devices, link)
        if (!c->excluded) {
            undo_candidate(c);
            failed_count++;
            pci_warn(c->dev, "pf-bar-recover: skip (bridge setup failed; other groups continue)\n");
        }
}

/* Drop every candidate lock before the whole-tree idle checks reacquire it.
 * Rescan/remove remains locked, so group roots stay registered throughout.
 */
static void release_candidates(void)
{
    struct group *g;
    struct candidate *c, *cn;
    list_for_each_entry(g, &groups, link)
        list_for_each_entry_safe(c, cn, &g->devices, link) {
            if (c->excluded)
                g->failed_candidates++;
            device_unlock(&c->dev->dev);
            pci_dev_put(c->dev);
            list_del(&c->link);
            kfree(c);
        }
}

static void free_groups(void)
{
    struct group *g, *gn;
    struct candidate *c, *cn;
    struct window_save *w, *wn;
    list_for_each_entry_safe(g, gn, &groups, link) {
        list_for_each_entry_safe(c, cn, &g->devices, link) {
            device_unlock(&c->dev->dev);
            pci_dev_put(c->dev);
            list_del(&c->link);
            kfree(c);
        }
        list_for_each_entry_safe(w, wn, &g->windows, link) {
            list_del(&w->link);
            kfree(w);
        }
        list_del(&g->link);
        kfree(g);
    }
}

#include "rebalance.inc"

static int __init recover_init(void)
{
    struct pci_dev *dev = NULL;
    struct group *g;
    int i, ret;
    if (!execute) {
        pr_info("pci_pf_bar_recover: idle; explicit execute=1 required\n");
        return 0;
    }
    if (!debug_limit || debug_limit > 16384)
        return -EINVAL;
    debug_run = ktime_get_boottime_ns();
    if (debug)
        pr_info("pci_pf_bar_recover: debug run=%llu begin mode=%s ports=%s limit=%u\n", (unsigned long long)debug_run, rebalance ? "rebalance" : conservative_only ? "conservative" : "automatic", *ports ? ports : "all", debug_limit);
    pci_lock_rescan_remove();
    ret = parse_ports();
    if (!ret)
        ret = parse_ensure_id();
    if (!ret && rebalance) {
        if (!nr_ports) {
            pr_err("pci_pf_bar_recover: rebalance requires an explicit root port list\n");
            ret = -EINVAL;
        } else {
            for (i = 0; i < nr_ports; i++)
                rebalance_root(selected_ports[i]);
        }
    } else if (!ret) {
        for_each_pci_dev(dev)
            collect_device(dev);
        list_for_each_entry(g, &groups, link)
            recover_group(g);
        release_candidates();
        if (!conservative_only && !*ensure_id)
            list_for_each_entry(g, &groups, link) {
                if (!g->root || !g->failed_candidates)
                    continue;
                pci_info(g->root, "pf-bar-recover: automatic idle-subtree fallback for %u failed candidates\n",
                         g->failed_candidates);
                if (rebalance_root(g->root)) {
                    restored_count += g->failed_candidates;
                    failed_count -= g->failed_candidates;
                }
            }
        free_groups();
        if (*ensure_id && ensure_matched) {
            if (!failed_count && !skipped_count) {
                ensure_complete = 1;
                pr_info("pci_pf_bar_recover: ensure-id all %u matched PFs complete; joint rebalance unnecessary\n", ensure_matched);
            } else {
                pr_info("pci_pf_bar_recover: ensure-id jointly rebalancing %d roots; matched PFs may shrink assigned ReBARs\n", nr_ports);
                if (rebalance_roots(selected_ports, nr_ports, true)) {
                    restored_count += failed_count;
                    failed_count = 0;
                    ensure_complete = 1;
                }
            }
            pr_info("pci_pf_bar_recover: ensure-id=%s complete=%u matched=%u\n", ensure_id, ensure_complete, ensure_matched);
        }
    }
    if (!ret && !conservative_only && !rebalance_broken)
        reservation_recover_roots();
    pr_info("pci_pf_bar_recover: restored=%u failed=%u skipped=%u cmp_reserved=%u; per-device failures do not abort scan\n",
            restored_count, failed_count, skipped_count, cmp_reserved_count);
    if (!ret && rebalance_broken)
        ret = -EUCLEAN;
    for (i = 0; i < nr_ports; i++)
        pci_dev_put(selected_ports[i]);
    pci_unlock_rescan_remove();
    if (debug)
        pr_info("pci_pf_bar_recover: debug run=%llu end rc=%d records=%u dropped=%u restored=%u failed=%u skipped=%u\n", (unsigned long long)debug_run, ret, debug_records, debug_dropped, restored_count, failed_count, skipped_count);
    return ret;
}
static void __exit recover_exit(void) { }
module_init(recover_init);
module_exit(recover_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Explicit one-shot recovery of missing physical PCI BARs");
