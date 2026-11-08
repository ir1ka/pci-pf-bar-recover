/* SPDX-License-Identifier: GPL-2.0 */
/* Actual production window-policy helpers, no hardware access. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef uint64_t resource_size_t;
#define PCI_STD_NUM_BARS 6
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define is_power_of_2(x) ((x)&&!((x)&((x)-1)))
#define IORESOURCE_MEM 0x200
#define IORESOURCE_PREFETCH 0x2000
#define max_t(t,a,b) ((t)(a)>(t)(b)?(t)(a):(t)(b))
struct resource {uint64_t start,end;unsigned long flags;struct resource *parent;};
struct pci_bus {int unused;};
struct pci_dev {unsigned vendor,device;bool is_virtfn,bridge,scope;struct resource resource[6];};
struct bar_save {uint64_t size;bool changed,wide;};
struct idle_snapshot {struct idle_snapshot *next;struct pci_dev *dev;struct bar_save probes[6];bool attempted[6];uint64_t reserve_size[6];};
struct idle_transaction {struct {struct idle_snapshot *first;}devices;unsigned long failed_flags;struct pci_bus *failed_host;};
static bool cmp_reserve=true;
static struct pci_bus host;
static struct pci_dev devs[2];static struct idle_snapshot snaps[2];static struct idle_transaction t;
static struct resource parent;
#define list_for_each_entry(s,h,l) for((s)=(h)->first;(s);(s)=(s)->next)
#define pci_info(d,...) ((void)(d))
static bool pci_is_bridge(struct pci_dev *d) {return d->bridge;}
static bool in_scope(struct pci_dev *d) {return d->scope;}
static uint64_t resource_size(struct resource *r) {return r->end-r->start+1;}
static struct pci_bus *idle_host_bus(struct pci_dev *d) {(void)d;return &host;}
static int idle_disable(struct idle_transaction *p) {(void)p;return 0;}
static int idle_release_resources(struct idle_transaction *p) {struct idle_snapshot *s;list_for_each_entry(s,&p->devices,link)for(int i=0;i<6;i++)s->dev->resource[i].parent=NULL;return 0;}
static void idle_clear_descriptors(struct idle_transaction *p) {struct idle_snapshot *s;list_for_each_entry(s,&p->devices,link)memset(s->dev->resource,0,sizeof(s->dev->resource));}
static int idle_prepare_bars(struct idle_transaction *p) {
 struct idle_snapshot *s;list_for_each_entry(s,&p->devices,link) {
  s->probes[1]=(struct bar_save){.size=1ULL<<26,.changed=true,.wide=true};
  s->probes[3]=(struct bar_save){.size=1ULL<<25,.changed=true,.wide=true};
  s->dev->resource[1]=(struct resource){.end=(1ULL<<26)-1,.flags=IORESOURCE_MEM|IORESOURCE_PREFETCH};
  s->dev->resource[3]=(struct resource){.end=(1ULL<<25)-1,.flags=IORESOURCE_MEM|IORESOURCE_PREFETCH};
 }return 0;
}
static int adjust_resource(struct resource *r,uint64_t start,uint64_t size) {assert(r->parent&&size<=resource_size(r));r->start=start;r->end=start+size-1;return 0;}
#include "bar_policy.inc"
/* PRODUCTION_FUNCTIONS */
static void reset(void) {
 memset(devs,0,sizeof(devs));memset(snaps,0,sizeof(snaps));memset(&t,0,sizeof(t));cmp_reserve=true;
 t.devices.first=&snaps[0];t.failed_flags=IORESOURCE_MEM|IORESOURCE_PREFETCH;t.failed_host=&host;
 for(int i=0;i<2;i++) {devs[i].vendor=0x10de;devs[i].device=i?0x2082:0x20c2;devs[i].scope=true;snaps[i].dev=&devs[i];snaps[i].next=i?NULL:&snaps[1];snaps[i].reserve_size[1]=1ULL<<36;}
 assert(!idle_prepare_bars(&t));
}
int main(void) {
 reset();assert(bar_reservation_goal(&devs[0],1,1ULL<<26)==1ULL<<36);assert(bar_reservation_goal(&devs[1],1,1ULL<<26)==1ULL<<36);
 assert(!bar_reservation_goal(&devs[0],3,1ULL<<25));
 devs[0].vendor=0x1234;assert(!bar_reservation_goal(&devs[0],1,1ULL<<26));devs[0].vendor=0x10de;
 devs[0].device=0x20f1;assert(!bar_reservation_goal(&devs[0],1,1ULL<<26));devs[0].device=0x20c2;
 devs[0].is_virtfn=true;assert(!bar_reservation_goal(&devs[0],1,1ULL<<26));devs[0].is_virtfn=false;
 devs[0].scope=false;assert(!bar_reservation_goal(&devs[0],1,1ULL<<26));devs[0].scope=true;
 cmp_reserve=false;assert(!bar_reservation_goal(&devs[0],1,1ULL<<26));cmp_reserve=true;
 /* No BAR0 is required; both devices enter the same planning phase. */
 assert(!devs[0].resource[0].parent&&!devs[1].resource[0].parent);
 assert(!idle_plan_extents(&t));
 for(int i=0;i<2;i++) {
  struct resource *r=&devs[i].resource[1];assert(resource_size(r)==1ULL<<36);
  uint64_t start=(1ULL<<36)+(uint64_t)i*(1ULL<<37);
  r->start=start;r->end=start+(1ULL<<36)-1;r->parent=&parent;
  struct resource *b3=&devs[i].resource[3];b3->start=r->end+1;b3->end=b3->start+(1ULL<<25)-1;b3->parent=&parent;
 }
 assert(!idle_finish_extents(&t));
 for(int i=0;i<2;i++) {assert(resource_size(&devs[i].resource[1])==1ULL<<26);assert(!(devs[i].resource[1].start&((1ULL<<36)-1)));assert(devs[i].resource[3].start>=devs[i].resource[1].start+(1ULL<<36));}
 reset();assert(idle_shrink_reservation(&t)==1);assert(snaps[0].reserve_size[1]==1ULL<<35&&snaps[1].reserve_size[1]==1ULL<<36);
 assert(idle_shrink_reservation(&t)==1);assert(snaps[1].reserve_size[1]==1ULL<<35);
 unsigned steps=2;while(idle_shrink_reservation(&t)==1)steps++;
 assert(steps==20);for(int i=0;i<2;i++)assert(snaps[i].reserve_size[1]==1ULL<<26&&resource_size(&devs[i].resource[1])==1ULL<<26);
 assert(!bar_reservation_next(1ULL<<26,1ULL<<26));
 reset();t.failed_flags=0;assert(!idle_shrink_reservation(&t));
 uint64_t cursor=0;
 assert(!idle_pack_advance(&cursor,1ULL<<36,(1ULL<<36)+(1ULL<<25)));
 assert(!idle_pack_advance(&cursor,1ULL<<36,(1ULL<<36)+(1ULL<<25)));
 assert(cursor==(3ULL<<36)+(1ULL<<25));
 cursor=UINT64_MAX-15;assert(idle_pack_advance(&cursor,64,0)==-EOVERFLOW);
 cursor=0;assert(idle_pack_advance(&cursor,3,1)==-EINVAL);
 cursor=1;assert(idle_pack_advance(&cursor,1,UINT64_MAX)==-EOVERFLOW);
 puts("PASS: exact profiles/scope, BAR0-independent planning, 64GiB-aligned slots, BAR3 exclusion, honest physical extents, monotonic largest-first capacity fallback; no MMIO or ReBAR expansion");
 return 0;
}
