/* SPDX-License-Identifier: GPL-2.0 */
/* Experimental whole-idle-subtree transaction. Included after normal helpers. */
static bool rebalance_broken;
static struct pci_dev *reservation_attempted_roots[64];
static int nr_reservation_attempted;
#include "bar_policy.inc"

struct idle_snapshot {
    struct list_head link;
    struct pci_dev *dev;
    struct resource resources[PCI_NUM_RESOURCES];
    struct bar_save probes[PCI_STD_NUM_BARS];
    bool attempted[PCI_NUM_RESOURCES];
    bool packed[PCI_NUM_RESOURCES];
    u32 bars[PCI_STD_NUM_BARS], rom, mem, pref, pref_hi, pref_limit_hi, io_hi;
    u16 command, io;
    resource_size_t reserve_size[PCI_STD_NUM_BARS];
    struct {
        u16 pos;
        u32 original, supported;
        bool changed;
    } rebar[PCI_STD_NUM_BARS];
};
struct idle_transaction {
    struct list_head devices;
    struct pci_dev *root;
    struct pci_dev **roots;
    int nr_roots;
    bool cohort;
    bool align_first;
    int error;
    unsigned long failed_flags;
    struct pci_bus *failed_host;
};

static bool idle_is_root(struct idle_transaction *t, struct pci_dev *dev)
{
    int i;
    for (i = 0; i < t->nr_roots; i++)
        if (dev == t->roots[i])
            return true;
    return false;
}

static bool idle_resource_index(struct idle_transaction *t,
                                struct idle_snapshot *s, int i)
{
    return !idle_is_root(t, s->dev) || i >= PCI_BRIDGE_RESOURCES;
}

/* Physical ReBAR only. Use standard config fields rather than private PCI
 * helpers or the version-dependent possible-sizes API. Never touch VF ReBAR.
 */
static int idle_snapshot_rebars(struct idle_snapshot *s)
{
    int pos, count, i, bar;
    u32 ctrl, cap;
    if (pci_is_bridge(s->dev))
        return 0;
    pos = pci_find_ext_capability(s->dev, PCI_EXT_CAP_ID_REBAR);
    if (!pos)
        return 0;
    if (pci_read_config_dword(s->dev, pos + PCI_REBAR_CTRL, &ctrl))
        return -EIO;
    count = (ctrl & PCI_REBAR_CTRL_NBAR_MASK) >> PCI_REBAR_CTRL_NBAR_SHIFT;
    if (!count || count > PCI_STD_NUM_BARS || pos + count * 8 + 4 > PCI_CFG_SPACE_EXP_SIZE)
        return -EINVAL;
    for (i = 0; i < count; i++, pos += 8) {
        if (pci_read_config_dword(s->dev, pos + PCI_REBAR_CTRL, &ctrl) ||
            pci_read_config_dword(s->dev, pos + PCI_REBAR_CAP, &cap))
            return -EIO;
        bar = ctrl & PCI_REBAR_CTRL_BAR_IDX;
        if (bar >= PCI_STD_NUM_BARS || s->rebar[bar].pos)
            return -EINVAL;
        s->rebar[bar].pos = pos + PCI_REBAR_CTRL;
        s->rebar[bar].original = ctrl;
        s->rebar[bar].supported = cap >> 4;
    }
    return 0;
}

static int idle_snapshot_device(struct pci_dev *dev, void *data)
{
    struct idle_transaction *t = data;
    struct idle_snapshot *s;
    bool bridge = pci_is_bridge(dev);
    int i, pos, count;
    u16 ctrl;
    if (t->error)
        return t->error;
    if (pci_find_host_bridge(dev->bus)->preserve_config) {
        pci_warn(dev, "rebalance: firmware preserve_config prohibits moving windows; skipped without changes\n");
        t->error = -EPERM;
        return t->error;
    }
    if (!device_trylock(&dev->dev)) {
        t->error = -EBUSY;
        return t->error;
    }
    pf_trace(dev, "stage=idle-check bridge=%u driver=%s vf=%u active_vfs=%d enabled=%u power=%d error_state=%d default_vga=%u\n", bridge, dev->dev.driver ? dev->dev.driver->name : "none", dev->is_virtfn, pci_num_vf(dev), pci_is_enabled(dev), dev->current_state, dev->error_state, vga_default_device() == dev);
    trace_resources(dev, "idle-input");
    if ((dev->hdr_type != PCI_HEADER_TYPE_NORMAL &&
         dev->hdr_type != PCI_HEADER_TYPE_BRIDGE) || dev->is_virtfn ||
        pci_num_vf(dev) || !pci_device_is_present(dev) ||
        vga_default_device() == dev ||
        (dev->resource[PCI_ROM_RESOURCE].flags & IORESOURCE_ROM_SHADOW) ||
        (dev->dev.driver && (!bridge || strcmp(dev->dev.driver->name, "pcieport"))) ||
        (!bridge && (pci_is_enabled(dev) || dev->mmio_always_on)) ||
        (dev->current_state != PCI_D0 && dev->current_state != PCI_UNKNOWN)) {
        pci_info(dev, "rebalance: reject subtree (driver, enabled function/VF or unsupported state)\n");
        t->error = -EBUSY;
        goto unlock;
    }
    pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_SRIOV);
    if (pos && (pci_read_config_word(dev, pos + PCI_SRIOV_CTRL, &ctrl) ||
        (ctrl & (PCI_SRIOV_CTRL_VFE | PCI_SRIOV_CTRL_MSE)))) {
        t->error = -EBUSY;
        goto unlock;
    }
    s = kzalloc(sizeof(*s), GFP_KERNEL);
    if (!s) {
        t->error = -ENOMEM;
        goto unlock;
    }
    s->dev = dev;
    if (pci_read_config_word(dev, PCI_COMMAND, &s->command) ||
        (!bridge && (s->command & PCI_COMMAND_MASTER))) {
        t->error = -EBUSY;
        goto free;
    }
    for (i = 0; i < PCI_NUM_RESOURCES; i++) {
        s->resources[i] = dev->resource[i];
#ifdef CONFIG_PCI_IOV
        if (t->cohort && !ensure_matches(dev) && i >= PCI_IOV_RESOURCES &&
            i <= PCI_IOV_RESOURCE_END && s->resources[i].flags) {
            pci_warn(dev, "ensure-id: cannot preserve non-target VF reservations; joint transaction skipped\n");
            t->error = -EOPNOTSUPP;
            goto free;
        }
#endif
        if (idle_is_root(t, dev) && i < PCI_STD_NUM_BARS && s->resources[i].child) {
            t->error = -EBUSY;
            goto free;
        }
        if (idle_resource_index(t, s, i) &&
            (s->resources[i].flags & IORESOURCE_PCI_FIXED)) {
            t->error = -EOPNOTSUPP;
            goto free;
        }
    }
    count = bridge ? 2 : PCI_STD_NUM_BARS;
    for (i = 0; i < count; i++)
        if (pci_read_config_dword(dev, PCI_BASE_ADDRESS_0 + i * 4, &s->bars[i])) {
            t->error = -EIO;
            goto free;
        }
    if (pci_read_config_dword(dev, bridge ? PCI_ROM_ADDRESS1 : PCI_ROM_ADDRESS, &s->rom)) {
        t->error = -EIO;
        goto free;
    }
    if (bridge && (pci_read_config_word(dev, PCI_IO_BASE, &s->io) ||
        pci_read_config_dword(dev, PCI_IO_BASE_UPPER16, &s->io_hi) ||
        pci_read_config_dword(dev, PCI_MEMORY_BASE, &s->mem) ||
        pci_read_config_dword(dev, PCI_PREF_MEMORY_BASE, &s->pref) ||
        pci_read_config_dword(dev, PCI_PREF_BASE_UPPER32, &s->pref_hi) ||
        pci_read_config_dword(dev, PCI_PREF_LIMIT_UPPER32, &s->pref_limit_hi))) {
        t->error = -EIO;
        goto free;
    }
    t->error = idle_snapshot_rebars(s);
    if (t->error)
        goto free;
    pci_dev_get(dev);
    list_add_tail(&s->link, &t->devices);
    return 0; /* Retain device lock through commit/rollback. */
free:
    kfree(s);
unlock:
    device_unlock(&dev->dev);
    return t->error;
}

static bool idle_owns_resource(struct idle_transaction *t, struct resource *r)
{
    struct idle_snapshot *s;
    int i;
    list_for_each_entry(s, &t->devices, link)
        for (i = 0; i < PCI_NUM_RESOURCES; i++)
            if (idle_resource_index(t, s, i) && r == &s->dev->resource[i])
                return true;
    return false;
}

static int idle_check_claims(struct idle_transaction *t)
{
    struct idle_snapshot *s;
    struct resource *r;
    int i;
    list_for_each_entry(s, &t->devices, link)
        for (i = 0; i < PCI_NUM_RESOURCES; i++) {
            if (!idle_resource_index(t, s, i))
                continue;
            for (r = s->dev->resource[i].child; r; r = r->sibling)
                if (!idle_owns_resource(t, r)) {
                    pci_info(s->dev, "rebalance: reject subtree (external resource claim %s)\n",
                             r->name ? r->name : "unknown");
                    return -EBUSY;
                }
        }
    return 0;
}

static int idle_disable(struct idle_transaction *t)
{
    struct idle_snapshot *s;
    int ret = 0;
    u16 command;
    list_for_each_entry_reverse(s, &t->devices, link) {
        ret |= pci_write_config_word(s->dev, PCI_COMMAND,
                                    s->command & ~(PCI_COMMAND_MEMORY | PCI_COMMAND_IO));
        if (pci_is_bridge(s->dev)) {
            ret |= pci_write_config_dword(s->dev, PCI_IO_BASE_UPPER16, 0xffff);
            ret |= pci_write_config_word(s->dev, PCI_IO_BASE, 0x00f0);
            ret |= pci_write_config_dword(s->dev, PCI_IO_BASE_UPPER16, 0);
            ret |= pci_write_config_dword(s->dev, PCI_MEMORY_BASE, 0x0000fff0);
            ret |= pci_write_config_dword(s->dev, PCI_PREF_LIMIT_UPPER32, 0);
            ret |= pci_write_config_dword(s->dev, PCI_PREF_MEMORY_BASE, 0x0000fff0);
            ret |= pci_write_config_dword(s->dev, PCI_PREF_BASE_UPPER32, 0);
        }
    }
    list_for_each_entry(s, &t->devices, link) {
        if (pci_read_config_word(s->dev, PCI_COMMAND, &command) ||
            (command & (PCI_COMMAND_MEMORY | PCI_COMMAND_IO)) ||
            (!pci_is_bridge(s->dev) && (command & PCI_COMMAND_MASTER)))
            ret = -EIO;
    }
    return ret ? -EIO : 0;
}

static int idle_release_resources(struct idle_transaction *t)
{
    struct idle_snapshot *s;
    struct resource *r;
    int i;
    list_for_each_entry_reverse(s, &t->devices, link)
        for (i = PCI_NUM_RESOURCES - 1; i >= 0; i--) {
            if (!idle_resource_index(t, s, i))
                continue;
            r = &s->dev->resource[i];
            if (r->child)
                return -EBUSY;
            if (r->parent && release_resource(r))
                return -EBUSY;
        }
    return 0;
}

static void idle_clear_descriptors(struct idle_transaction *t)
{
    struct idle_snapshot *s;
    int i;
    list_for_each_entry(s, &t->devices, link)
        for (i = 0; i < PCI_NUM_RESOURCES; i++)
            if (idle_resource_index(t, s, i)) {
                struct resource *r = &s->dev->resource[i];
                r->start = r->end = r->flags = 0;
                r->parent = r->child = r->sibling = NULL;
            }
}

static int idle_restore_config(struct idle_transaction *t)
{
    struct idle_snapshot *s;
    int i, count, ret = 0;
    list_for_each_entry(s, &t->devices, link) {
        bool bridge = pci_is_bridge(s->dev);
        if (!idle_is_root(t, s->dev)) {
            count = bridge ? 2 : PCI_STD_NUM_BARS;
            for (i = 0; i < count; i++)
                ret |= pci_write_config_dword(s->dev, PCI_BASE_ADDRESS_0 + i * 4, s->bars[i]);
            ret |= pci_write_config_dword(s->dev, bridge ? PCI_ROM_ADDRESS1 : PCI_ROM_ADDRESS, s->rom);
        }
        if (bridge) {
            ret |= pci_write_config_dword(s->dev, PCI_IO_BASE_UPPER16, 0xffff);
            ret |= pci_write_config_word(s->dev, PCI_IO_BASE, s->io);
            ret |= pci_write_config_dword(s->dev, PCI_IO_BASE_UPPER16, s->io_hi);
            ret |= pci_write_config_dword(s->dev, PCI_MEMORY_BASE, s->mem);
            ret |= pci_write_config_dword(s->dev, PCI_PREF_LIMIT_UPPER32, 0);
            ret |= pci_write_config_dword(s->dev, PCI_PREF_MEMORY_BASE, s->pref);
            ret |= pci_write_config_dword(s->dev, PCI_PREF_BASE_UPPER32, s->pref_hi);
            ret |= pci_write_config_dword(s->dev, PCI_PREF_LIMIT_UPPER32, s->pref_limit_hi);
        }
    }
    /* Only restore decoders if every address register write succeeded. */
    if (!ret)
        list_for_each_entry(s, &t->devices, link)
            ret |= pci_write_config_word(s->dev, PCI_COMMAND, s->command);
    return ret ? -EIO : 0;
}

static int idle_restore_rebars(struct idle_transaction *t)
{
    struct idle_snapshot *s;
    int i, ret = 0;
    u32 ctrl;
    list_for_each_entry(s, &t->devices, link)
        for (i = 0; i < PCI_STD_NUM_BARS; i++) {
            if (!s->rebar[i].changed)
                continue;
            if (pci_write_config_dword(s->dev, s->rebar[i].pos, s->rebar[i].original) ||
                pci_read_config_dword(s->dev, s->rebar[i].pos, &ctrl) ||
                ctrl != s->rebar[i].original) {
                ret = -EIO;
                pci_err(s->dev, "rebalance: ReBAR%d original control restore failed\n", i);
            } else {
                pci_info(s->dev, "rebalance: ReBAR%d original control restored\n", i);
            }
        }
    return ret;
}

static int idle_rollback(struct idle_transaction *t)
{
    struct idle_snapshot *s;
    int i, ret;
    ret = idle_disable(t);
    if (ret)
        return ret;
    ret = idle_release_resources(t);
    if (ret)
        return ret;
    ret = idle_restore_rebars(t);
    if (ret)
        return ret;
    idle_clear_descriptors(t);
    /* Preorder: restore parent windows before claiming child resources. */
    list_for_each_entry(s, &t->devices, link)
        for (i = 0; i < PCI_NUM_RESOURCES; i++) {
            struct resource *r = &s->dev->resource[i];
            if (!idle_resource_index(t, s, i))
                continue;
            *r = s->resources[i];
            r->parent = r->child = r->sibling = NULL;
            if (s->resources[i].parent && request_resource(s->resources[i].parent, r))
                return -EBUSY;
        }
    return idle_restore_config(t);
}

static void idle_quarantine(struct idle_transaction *t)
{
    struct idle_snapshot *s;
    int ret, i;
    idle_disable(t);
    list_for_each_entry(s, &t->devices, link) {
        if (!pci_is_bridge(s->dev)) {
            ret = device_set_driver_override(&s->dev->dev, "pci-pf-bar-quarantine");
            if (ret)
                pci_err(s->dev, "rebalance: cannot set quarantine override: %d\n", ret);
        }
        for (i = 0; i < PCI_STD_NUM_BARS; i++)
            if (s->dev->resource[i].flags)
                s->dev->resource[i].flags |= IORESOURCE_UNSET | IORESOURCE_DISABLED;
    }
    rebalance_broken = true;
    pci_err(t->root, "rebalance: ROLLBACK FAILED; subtree disabled/quarantined, manual intervention required\n");
}

static int idle_prepare_bars(struct idle_transaction *t)
{
    struct idle_snapshot *s;
    int i, count, ret;
    list_for_each_entry(s, &t->devices, link) {
        if (idle_is_root(t, s->dev))
            continue;
        count = pci_is_bridge(s->dev) ? 2 : PCI_STD_NUM_BARS;
        for (i = 0; i < count; i++) {
            ret = probe_bar(s->dev, i, &s->probes[i]);
            if (ret < 0)
                return ret;
            if (s->probes[i].wide)
                i++;
        }
        for (i = 0; i < count; i++) {
            struct resource *r = &s->dev->resource[i];
            resource_size_t goal;
            if (!s->probes[i].changed || !s->probes[i].wide ||
                !(r->flags & IORESOURCE_MEM))
                continue;
            goal = bar_reservation_goal(s->dev, i, s->probes[i].size);
            if (goal) {
                if (!s->reserve_size[i])
                    s->reserve_size[i] = goal;
                if (s->reserve_size[i] < s->probes[i].size)
                    s->reserve_size[i] = s->probes[i].size;
            }
        }
        /* Preserve existing ROM capacity, but never enable ROM decoding. */
        if (s->resources[PCI_ROM_RESOURCE].flags & IORESOURCE_MEM) {
            struct resource *r = &s->dev->resource[PCI_ROM_RESOURCE];
            r->start = 0;
            r->end = resource_size(&s->resources[PCI_ROM_RESOURCE]) - 1;
            r->flags = (s->resources[PCI_ROM_RESOURCE].flags & ~IORESOURCE_ROM_ENABLE) |
                       IORESOURCE_SIZEALIGN | IORESOURCE_UNSET;
        }
    }
    return 0;
}

static struct pci_bus *idle_host_bus(struct pci_dev *dev)
{
    struct pci_bus *bus = dev->bus;
    while (bus->parent)
        bus = bus->parent;
    return bus;
}

/* These transient extents only model future demand while every device is
 * locked and decoding is disabled. Keep them claimed through all allocations
 * so BAR3 and peers stay outside a future aligned BAR1 slot. */
static int idle_plan_extents(struct idle_transaction *t)
{
    struct idle_snapshot *s;
    int i;
    list_for_each_entry(s, &t->devices, link)
        for (i = 0; i < PCI_STD_NUM_BARS; i++)
            if (s->reserve_size[i] > s->probes[i].size) {
                struct resource *r = &s->dev->resource[i];
                if (r->parent || !s->probes[i].changed)
                    return -EINVAL;
                r->end = r->start + s->reserve_size[i] - 1;
            }
    return 0;
}

/* Restore honest physical resource lengths before commit or unlocking. Bridge
 * windows retain their allocated capacity; no synthetic endpoint claim remains. */
static int idle_finish_extents(struct idle_transaction *t)
{
    struct idle_snapshot *s;
    int i, ret;
    list_for_each_entry(s, &t->devices, link)
        for (i = 0; i < PCI_STD_NUM_BARS; i++) {
            struct resource *r = &s->dev->resource[i];
            if (s->reserve_size[i] > s->probes[i].size) {
                ret = adjust_resource(r, r->start, s->probes[i].size);
                if (ret)
                    return ret;
            }
        }
    return 0;
}

/* Mirror this module's windows-first, then largest-BAR placement order.
 * Native sizing sums child sizes but need not account for gaps between
 * already-sized child windows (e.g. two 64GiB+32MiB windows aligned to 64GiB).
 * Do a postorder packing pass, without padding every child up to 128GiB. */
/* resource_alignment() is not exported on every supported kernel. */
static resource_size_t idle_extent_alignment(struct resource *r)
{
    switch (r->flags & (IORESOURCE_SIZEALIGN | IORESOURCE_STARTALIGN)) {
    case IORESOURCE_SIZEALIGN: return resource_size(r);
    case IORESOURCE_STARTALIGN: return r->start;
    default: return 0;
    }
}

static int idle_pack_advance(resource_size_t *cursor, resource_size_t align,
                             resource_size_t size)
{
    resource_size_t limit = ~(resource_size_t)0, next;
    if (!is_power_of_2(align))
        return -EINVAL;
    if (*cursor > limit - (align - 1))
        return -EOVERFLOW;
    next = ALIGN(*cursor, align);
    if (size > limit - next)
        return -EOVERFLOW;
    *cursor = next + size;
    return 0;
}

static resource_size_t idle_window_weight(struct idle_snapshot *s)
{
    resource_size_t size = 0;
    int i;
    for (i = 0; i < 3; i++) {
        struct resource *r = &s->dev->resource[PCI_BRIDGE_RESOURCES + i];
        if (!r->parent && r->flags)
            size = max(size, resource_size(r));
    }
    return size;
}

static resource_size_t idle_window_alignment(struct pci_dev *dev)
{
    resource_size_t alignment = 0;
    int i;
    for (i = 0; i < 3; i++) {
        struct resource *r = &dev->resource[PCI_BRIDGE_RESOURCES + i];
        if (!r->parent && r->flags)
            alignment = max(alignment, idle_extent_alignment(r));
    }
    return alignment;
}

static int idle_window_order(void *data, const struct list_head *a,
                             const struct list_head *b)
{
    struct idle_transaction *t = data;
    const struct window_save *wa = list_entry(a, struct window_save, link);
    const struct window_save *wb = list_entry(b, struct window_save, link);
    resource_size_t aa, ab;
    if (t->align_first && bridge_depth(wa->dev) == bridge_depth(wb->dev)) {
        aa = idle_window_alignment(wa->dev);
        ab = idle_window_alignment(wb->dev);
        if (aa != ab)
            return aa < ab ? 1 : -1;
    }
    return window_order(NULL, a, b);
}

static int idle_pack_bus(struct idle_transaction *t, struct pci_bus *bus,
                          struct resource *window, unsigned long type)
{
    struct idle_snapshot *s, *chosen;
    resource_size_t cursor = 0, alignment = 1ULL << 20, weight, biggest, align, best_align;
    struct resource *r;
    int i, bar, ret;
    if (!window_assignable(window) || !(window->flags & IORESOURCE_MEM))
        return 0;
    list_for_each_entry(s, &t->devices, link) {
        if (s->dev->bus != bus)
            continue;
        memset(s->packed, 0, sizeof(s->packed));
        for (i = 0; i < PCI_NUM_RESOURCES; i++) {
            r = &s->dev->resource[i];
            if ((i > PCI_ROM_RESOURCE && (i < PCI_BRIDGE_RESOURCES ||
                 i >= PCI_BRIDGE_RESOURCES + 3)) || r->parent ||
                !window_assignable(r) ||
                (r->flags & (IORESOURCE_MEM | IORESOURCE_PREFETCH)) != type)
                continue;
            alignment = max(alignment, idle_extent_alignment(r));
        }
    }
    /* Actual bridge allocation sorts same-depth windows by maximum size. */
    for (;;) {
        chosen = NULL;biggest = 0;best_align = 0;bar = -1;
        list_for_each_entry(s, &t->devices, link) {
            if (s->dev->bus != bus || !pci_is_bridge(s->dev))
                continue;
            for (i = PCI_BRIDGE_RESOURCES; i < PCI_BRIDGE_RESOURCES + 3; i++) {
                r = &s->dev->resource[i];
                if (s->packed[i] || r->parent || !window_assignable(r) ||
                    (r->flags & (IORESOURCE_MEM | IORESOURCE_PREFETCH)) != type)
                    continue;
                weight = idle_window_weight(s);
                align = idle_window_alignment(s->dev);
                if (!chosen || (t->align_first && align > best_align) ||
                    ((!t->align_first || align == best_align) && weight > biggest)) {
                    chosen = s;biggest = weight;best_align = align;bar = i;
                }
            }
        }
        if (!chosen)
            break;
        r = &chosen->dev->resource[bar];chosen->packed[bar] = true;
        align = max_t(resource_size_t, idle_extent_alignment(r), 1ULL << 20);
        ret = idle_pack_advance(&cursor, align, resource_size(r));
        if (ret)
            return ret;
    }
    for (;;) {
        chosen = NULL;biggest = 0;bar = -1;
        list_for_each_entry(s, &t->devices, link) {
            if (s->dev->bus != bus)
                continue;
            for (i = 0; i <= PCI_ROM_RESOURCE; i++) {
                r = &s->dev->resource[i];
                if (s->packed[i] || r->parent || !window_assignable(r) ||
                    (r->flags & (IORESOURCE_MEM | IORESOURCE_PREFETCH)) != type)
                    continue;
                if (resource_size(r) > biggest) {
                    chosen = s;biggest = resource_size(r);bar = i;
                }
            }
        }
        if (!chosen)
            break;
        r = &chosen->dev->resource[bar];chosen->packed[bar] = true;
        align = max_t(resource_size_t, idle_extent_alignment(r), 1);
        ret = idle_pack_advance(&cursor, align, resource_size(r));
        if (ret)
            return ret;
    }
    if (cursor) {
        resource_size_t size;
        ret = idle_pack_advance(&cursor, 1ULL << 20, 0);
        if (ret)
            return ret;
        size = max(resource_size(window), cursor);
        if (window->flags & IORESOURCE_STARTALIGN)
            alignment = max(alignment, window->start);
        if (size - 1 > ~(resource_size_t)0 - alignment)
            return -EOVERFLOW;
        window->start = alignment;
        window->end = alignment + size - 1;
        window->flags = (window->flags & ~IORESOURCE_SIZEALIGN) | IORESOURCE_STARTALIGN;
    }
    return 0;
}

static int idle_pack_windows(struct idle_transaction *t)
{
    struct idle_snapshot *s;
    bool needed = false;
    int i, ret;
    list_for_each_entry(s, &t->devices, link)
        for (i = 0; i < PCI_STD_NUM_BARS; i++)
            needed |= s->reserve_size[i] != 0;
    if (!needed)
        return 0;
    list_for_each_entry_reverse(s, &t->devices, link) {
        if (!pci_is_bridge(s->dev) || !s->dev->subordinate)
            continue;
        ret = idle_pack_bus(t, s->dev->subordinate,
                            &s->dev->resource[PCI_BRIDGE_MEM_WINDOW], IORESOURCE_MEM);
        if (!ret)
            ret = idle_pack_bus(t, s->dev->subordinate,
                                &s->dev->resource[PCI_BRIDGE_PREF_MEM_WINDOW],
                                IORESOURCE_MEM | IORESOURCE_PREFETCH);
        if (ret)
            return ret;
    }
    return 0;
}

static int idle_allocate(struct idle_transaction *t, struct group *g)
{
    struct window_save *w;
    struct idle_snapshot *s, *chosen;
    resource_size_t biggest;
    int i, bar, ret;
    t->failed_flags = 0;
    t->failed_host = NULL;
    ret = idle_plan_extents(t);
    if (ret)
        return ret;
    for (i = 0; i < t->nr_roots; i++)
        pci_bus_size_bridges(t->roots[i]->subordinate);
    ret = idle_pack_windows(t);
    if (ret)
        return ret;
    list_sort(t, &g->windows, idle_window_order);
    list_for_each_entry(w, &g->windows, link)
        for (i = 0; i < 3; i++) {
            struct resource *r = &w->dev->resource[PCI_BRIDGE_RESOURCES + i];
            if (!window_assignable(r)) {
                pf_trace(w->dev, "stage=rebalance-window-skip index=%d value=%pR flags=%#lx\n",
                         PCI_BRIDGE_RESOURCES + i, r, r->flags);
                continue;
            }
            ret = pci_assign_resource(w->dev, PCI_BRIDGE_RESOURCES + i);
            pf_trace(w->dev, "stage=rebalance-window-assign index=%d rc=%d value=%pR\n",
                     PCI_BRIDGE_RESOURCES + i, ret, r);
            if (ret) {
                t->failed_flags = r->flags;
                t->failed_host = idle_host_bus(w->dev);
                pci_warn(w->dev, "rebalance: window assignment failed index=%d rc=%d value=%pR\n",
                         PCI_BRIDGE_RESOURCES + i, ret, r);
                return ret;
            }
        }
    for (;;) {
        chosen = NULL;
        bar = -1;
        biggest = 0;
        list_for_each_entry(s, &t->devices, link) {
            if (idle_is_root(t, s->dev))
                continue;
            for (i = 0; i <= PCI_ROM_RESOURCE; i++) {
                struct resource *r = &s->dev->resource[i];
                if (!s->attempted[i] && (r->flags & (IORESOURCE_MEM | IORESOURCE_IO)) &&
                    resource_size(r) > biggest) {
                    chosen = s;
                    bar = i;
                    biggest = resource_size(r);
                }
            }
        }
        if (!chosen)
            break;
        chosen->attempted[bar] = true;
        ret = pci_assign_resource(chosen->dev, bar);
        pf_trace(chosen->dev, "stage=rebalance-pf-assign bar=%d rc=%d value=%pR\n",
                 bar, ret, &chosen->dev->resource[bar]);
        if (ret) {
            t->failed_flags = chosen->dev->resource[bar].flags;
            t->failed_host = idle_host_bus(chosen->dev);
            pci_warn(chosen->dev, "rebalance: BAR%d assignment failed rc=%d value=%pR\n",
                     bar, ret, &chosen->dev->resource[bar]);
            return ret;
        }
    }
    ret = idle_finish_extents(t);
    return ret ? ret : program_windows(g);
}

/* Lower only advisory demand, releasing the entire partial plan first. */
static int idle_shrink_reservation(struct idle_transaction *t)
{
    struct idle_snapshot *s, *chosen = NULL;
    resource_size_t biggest = 0, next, selected_next = 0;
    int i, bar = -1, ret;
    if (!(t->failed_flags & IORESOURCE_MEM))
        return 0;
    list_for_each_entry(s, &t->devices, link) {
        if (idle_host_bus(s->dev) != t->failed_host)
            continue;
        for (i = 0; i < PCI_STD_NUM_BARS; i++) {
            if ((s->dev->resource[i].flags ^ t->failed_flags) & IORESOURCE_PREFETCH)
                continue;
            next = bar_reservation_next(s->reserve_size[i], s->probes[i].size);
            if (next && s->reserve_size[i] > biggest) {
                chosen = s;
                bar = i;
                biggest = s->reserve_size[i];
                selected_next = next;
            }
        }
    }
    if (!chosen)
        return 0;
    ret = idle_disable(t);
    if (!ret)
        ret = idle_release_resources(t);
    if (ret)
        return ret;
    idle_clear_descriptors(t);
    list_for_each_entry(s, &t->devices, link) {
        memset(s->attempted, 0, sizeof(s->attempted));
        memset(s->probes, 0, sizeof(s->probes));
    }
    chosen->reserve_size[bar] = selected_next;
    pci_info(chosen->dev, "pf-bar reserve: BAR%d goal reduced %llu -> %llu bytes; no device resize\n",
             bar, (unsigned long long)biggest, (unsigned long long)selected_next);
    ret = idle_prepare_bars(t);
    return ret ? ret : 1;
}

/* One monotonic step: shrink the largest eligible missing physical BAR to
 * its next advertised size. Finite (<=31 steps per BAR), no combinatorial or
 * grow-back search, and no hardware/address changes outside the locked forest.
 * Return 1 to retry, 0 when exhausted, negative for an unsafe/error condition.
 */
static int idle_shrink_rebar(struct idle_transaction *t)
{
    struct idle_snapshot *s, *chosen = NULL;
    resource_size_t biggest = 0;
    u32 ctrl, selected_ctrl = 0, readback;
    int i, next, bar = -1, selected_next = -1, current_size, ret;
    if (!(t->failed_flags & IORESOURCE_MEM))
        return 0;
    list_for_each_entry(s, &t->devices, link) {
        if (idle_is_root(t, s->dev) || pci_is_bridge(s->dev))
            continue;
        for (i = 0; i < PCI_STD_NUM_BARS; i++) {
            struct resource *r = &s->dev->resource[i];
            /* Assigned BAR shrinking is permitted only for the explicit cohort
             * or the strictly scoped reservation profile, never arbitrary peers. */
            if (!s->rebar[i].pos || (!t->cohort && s->resources[i].parent && !s->reserve_size[i]) ||
                (t->cohort && (!ensure_matches(s->dev) || idle_host_bus(s->dev) != t->failed_host)) ||
                !(r->flags & IORESOURCE_MEM) || !s->probes[i].changed ||
                ((r->flags ^ t->failed_flags) & IORESOURCE_PREFETCH))
                continue;
            if (pci_read_config_dword(s->dev, s->rebar[i].pos, &ctrl))
                return -EIO;
            current_size = (ctrl & PCI_REBAR_CTRL_BAR_SIZE) >> PCI_REBAR_CTRL_BAR_SHIFT;
            if (s->probes[i].size != (1ULL << (current_size + 20)))
                continue; /* Capability and hardware mask must agree. */
            for (next = current_size - 1; next >= 0; next--)
                if (s->rebar[i].supported & (1U << next))
                    break;
            if (next >= 0 && s->probes[i].size > biggest) {
                chosen = s;
                biggest = s->probes[i].size;
                bar = i;
                selected_next = next;
                selected_ctrl = ctrl;
            }
        }
    }
    if (!chosen)
        return 0;
    /* Release even partially allocated windows before changing the mask. */
    ret = idle_disable(t);
    if (!ret)
        ret = idle_release_resources(t);
    if (ret)
        return ret;
    idle_clear_descriptors(t);
    list_for_each_entry(s, &t->devices, link) {
        memset(s->attempted, 0, sizeof(s->attempted));
        memset(s->probes, 0, sizeof(s->probes));
    }
    ctrl = (selected_ctrl & ~PCI_REBAR_CTRL_BAR_SIZE) |
           (selected_next << PCI_REBAR_CTRL_BAR_SHIFT);
    chosen->rebar[bar].changed = true; /* Record before a possibly partial write. */
    if (pci_write_config_dword(chosen->dev, chosen->rebar[bar].pos, ctrl) ||
        pci_read_config_dword(chosen->dev, chosen->rebar[bar].pos, &readback) ||
        ctrl != readback)
        return -EIO;
    pci_info(chosen->dev, "rebalance: ReBAR%d reduced %llu -> %llu bytes; retry allocation\n",
             bar, (unsigned long long)biggest, 1ULL << (selected_next + 20));
    ret = idle_prepare_bars(t);
    if (ret)
        return ret;
    if (chosen->probes[bar].size != (1ULL << (selected_next + 20))) {
        pci_err(chosen->dev, "rebalance: ReBAR%d resized control/mask disagree; rollback\n", bar);
        return -EIO;
    }
    return 1;
}

static int idle_commit(struct idle_transaction *t)
{
    struct idle_snapshot *s;
    struct pci_bus_region bus;
    int i, count;
    u32 lo, hi, mask;
    list_for_each_entry(s, &t->devices, link) {
        if (idle_is_root(t, s->dev))
            continue;
        count = pci_is_bridge(s->dev) ? 2 : PCI_STD_NUM_BARS;
        for (i = 0; i < count; i++) {
            struct resource *r = &s->dev->resource[i];
            if (!(r->flags & (IORESOURCE_MEM | IORESOURCE_IO)))
                continue;
            if (s->rebar[i].changed) {
                if (pci_read_config_dword(s->dev, s->rebar[i].pos, &lo) ||
                    resource_size(r) != (1ULL << (((lo & PCI_REBAR_CTRL_BAR_SIZE) >>
                                                 PCI_REBAR_CTRL_BAR_SHIFT) + 20)))
                    return -EIO;
            }
            if (!r->parent || (r->flags & IORESOURCE_UNSET))
                return -ENOSPC;
            pcibios_resource_to_bus(s->dev->bus, &bus, r);
            mask = r->flags & IORESOURCE_IO ? PCI_BASE_ADDRESS_IO_MASK : PCI_BASE_ADDRESS_MEM_MASK;
            if (pci_read_config_dword(s->dev, PCI_BASE_ADDRESS_0 + i * 4, &lo) ||
                (lo & mask) != (lower_32_bits(bus.start) & mask))
                return -EIO;
            if (r->flags & IORESOURCE_MEM_64) {
                if (pci_read_config_dword(s->dev, PCI_BASE_ADDRESS_0 + i * 4 + 4, &hi) ||
                    hi != upper_32_bits(bus.start))
                    return -EIO;
                i++;
            }
        }
    }
    /* program_windows already enables only the newly built bridge windows. */
    list_for_each_entry(s, &t->devices, link) {
        u16 desired = s->command, decode_cmd;
        if (pci_is_bridge(s->dev)) {
            if (pci_read_config_word(s->dev, PCI_COMMAND, &decode_cmd))
                return -EIO;
            desired |= decode_cmd & (PCI_COMMAND_MEMORY | PCI_COMMAND_IO);
            for (i = 0; i < 2; i++) {
                struct resource *r = &s->dev->resource[i];
                if (r->parent && (r->flags & IORESOURCE_MEM))
                    desired |= PCI_COMMAND_MEMORY;
                if (r->parent && (r->flags & IORESOURCE_IO))
                    desired |= PCI_COMMAND_IO;
            }
        }
        if (pci_write_config_word(s->dev, PCI_COMMAND, desired))
            return -EIO;
    }
#ifdef CONFIG_PCI_IOV
    list_for_each_entry(s, &t->devices, link)
        if (!idle_is_root(t, s->dev) && s->dev->is_physfn &&
            (!t->cohort || ensure_matches(s->dev)))
            pci_sriov_set_totalvfs(s->dev, 0);
#endif
    return 0;
}

static bool rebalance_roots(struct pci_dev **roots, int nr_roots, bool cohort)
{
    struct idle_transaction t;
    struct idle_snapshot *s, *next;
    struct group g;
    struct window_save *w, *wn;
    struct pci_dev *root = roots[0];
    int ret, rollback, retry, ri;
    bool committed = false;
    const char *stage = "snapshot";
    pf_trace(root, "stage=rebalance-begin\n");
    trace_apertures(root);
    INIT_LIST_HEAD(&t.devices);
    t.root = root;
    t.roots = roots;
    t.nr_roots = nr_roots;
    t.cohort = cohort;
    t.align_first = false;
    t.error = 0;
    memset(&g, 0, sizeof(g));
    INIT_LIST_HEAD(&g.windows);
    g.root = root;
    g.bus = root->subordinate;
    for (ri = 0; ri < nr_roots && !t.error; ri++) {
        idle_snapshot_device(roots[ri], &t);
        if (!t.error)
            pci_walk_bus(roots[ri]->subordinate, idle_snapshot_device, &t);
    }
    ret = t.error ? t.error : idle_check_claims(&t);
    if (ret) {
        pci_warn(root, "rebalance: subtree not idle/supported (%d); skipped without changes\n", ret);
        goto cleanup;
    }
    /* Allocate all bookkeeping before making any PCI changes. */
    ret = 0;
    for (ri = 0; ri < nr_roots && !ret && !g.window_error; ri++) {
        ret = save_window(roots[ri], &g);
        if (!ret)
            pci_walk_bus(roots[ri]->subordinate, save_window, &g);
    }
    if (ret || g.window_error) {
        ret = ret ? ret : g.window_error;
        goto cleanup;
    }
    list_for_each_entry(w, &g.windows, link)
        memset(w->old, 0, sizeof(w->old));
    stage = "disable";
    ret = idle_disable(&t);
    pf_trace(root, "stage=rebalance-disable rc=%d\n", ret);
    if (!ret) {
        stage = "release";
        ret = idle_release_resources(&t);
        pf_trace(root, "stage=rebalance-release rc=%d\n", ret);
    }
    if (!ret) {
        stage = "probe";
        idle_clear_descriptors(&t);
        ret = idle_prepare_bars(&t);
        if (!ret)
            list_for_each_entry(s, &t.devices, link)
                if (s->reserve_size[1]) {
                    for (ri = 0; ri < nr_reservation_attempted; ri++)
                        if (reservation_attempted_roots[ri] == top_bridge(s->dev))
                            break;
                    if (ri == nr_reservation_attempted && nr_reservation_attempted < ARRAY_SIZE(reservation_attempted_roots))
                        reservation_attempted_roots[nr_reservation_attempted++] = top_bridge(s->dev);
                }
        pf_trace(root, "stage=rebalance-probe rc=%d\n", ret);
    }
    if (!ret) {
        stage = "allocate";
        ret = idle_allocate(&t, &g);
        while (ret == -ENOSPC || ret == -ENOMEM) {
            /* One alternative layout at unchanged demand before each retreat.
             * Release partial assignments and probe the same physical sizes. */
            if (!t.align_first) {
                retry = idle_disable(&t);
                if (!retry) retry = idle_release_resources(&t);
                if (!retry) {
                    idle_clear_descriptors(&t);
                    list_for_each_entry(s, &t.devices, link) {
                        memset(s->attempted, 0, sizeof(s->attempted));
                        memset(s->probes, 0, sizeof(s->probes));
                    }
                    retry = idle_prepare_bars(&t);
                }
                if (retry) { ret = retry; break; }
                t.align_first = true;
                pci_info(root, "rebalance: retry alignment-first layout without reducing BAR/window demand\n");
                ret = idle_allocate(&t, &g);
                continue;
            }
            retry = idle_shrink_reservation(&t);
            if (!retry)
                retry = idle_shrink_rebar(&t);
            if (retry <= 0) {
                if (retry < 0)
                    ret = retry;
                break;
            }
            t.align_first = false;
            ret = idle_allocate(&t, &g);
        }
        pf_trace(root, "stage=rebalance-allocate rc=%d\n", ret);
        trace_resources(root, "rebalance-allocated-root");
    }
    if (!ret) {
        stage = "commit";
        ret = idle_commit(&t);
        pf_trace(root, "stage=rebalance-commit rc=%d\n", ret);
    }
    if (ret) {
        pf_trace(root, "stage=rebalance-failed failed_stage=%s rc=%d\n", stage, ret);
        rollback = idle_rollback(&t);
        pf_trace(root, "stage=rebalance-rollback rc=%d\n", rollback);
        if (rollback)
            idle_quarantine(&t);
        else
            pci_warn(root, "rebalance: failed stage=%s (%d); original resources/config restored; other ports continue\n", stage, ret);
    } else {
        committed = true;
        list_for_each_entry(s, &t.devices, link)
            if (s->reserve_size[1]) {
                cmp_reserved_count++;
                pci_info(s->dev, "pf-bar reserve: BAR1 actual=%llu goal=%llu bytes committed; driver owns unlock/expansion\n",
                         (unsigned long long)s->probes[1].size,
                         (unsigned long long)s->reserve_size[1]);
            }
        pci_info(root, "rebalance: COMMITTED idle subtree, bridge windows may have moved/expanded\n");
    }
cleanup:
    for (ri = 0; ri < nr_roots; ri++)
        pf_result(roots[ri], "root", stage, ret, 0, 0);
    if (committed)
        list_for_each_entry(s, &t.devices, link)
            if (s->reserve_size[1])
                pf_result(s->dev, "reservation", "commit", 0,
                          s->probes[1].size, s->reserve_size[1]);
    list_for_each_entry_safe(w, wn, &g.windows, link) {
        list_del(&w->link);
        kfree(w);
    }
    list_for_each_entry_safe(s, next, &t.devices, link) {
        trace_resources(s->dev, "rebalance-final");
        device_unlock(&s->dev->dev);
        pci_dev_put(s->dev);
        list_del(&s->link);
        kfree(s);
    }
    return committed;
}

static bool rebalance_root(struct pci_dev *root)
{
    return rebalance_roots(&root, 1, false);
}

/* Healthy small BARs do not enter missing-BAR groups. Schedule their roots
 * independently, once each, after conservative bootstrap and fallback. */
static void reservation_recover_roots(void)
{
    struct pci_dev *dev = NULL, *root, *roots[64];
    int nr = 0, i;
    if (!cmp_reserve)
        return;
    for_each_pci_dev(dev) {
        if (bar_reservation_goal(dev, 1, resource_size(&dev->resource[1])) <=
            resource_size(&dev->resource[1]))
            continue;
        root = top_bridge(dev);
        if (!root || !root->subordinate)
            continue;
        for (i = 0; i < nr_reservation_attempted; i++)
            if (reservation_attempted_roots[i] == root)
                break;
        if (i < nr_reservation_attempted)
            continue;
        for (i = 0; i < nr; i++)
            if (roots[i] == root)
                break;
        if (i < nr)
            continue;
        if (nr == ARRAY_SIZE(roots)) {
            pci_warn(dev, "pf-bar reserve: root limit reached; remaining roots skipped\n");
            continue;
        }
        roots[nr++] = pci_dev_get(root);
    }
    for (i = 0; i < nr; i++) {
        pci_info(roots[i], "pf-bar reserve: default window planning for scoped CMP devices\n");
        if (!rebalance_broken && !rebalance_root(roots[i]))
            failed_count++;
        pci_dev_put(roots[i]);
    }
}
