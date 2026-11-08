import re,subprocess,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def extract(text,name):
 start=re.search(r'static (?:int|resource_size_t) '+name+r'\(',text).start();pos=text.index('{',start);depth=1;end=pos+1
 while depth:
  depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]+'\n'
class Layout(unittest.TestCase):
 def test_alignment_order_avoids_a_gap_without_reducing_demand(self):
  text=(ROOT/'src/rebalance.inc').read_text()
  functions=''.join(extract(text,n)for n in ('idle_extent_alignment','idle_window_alignment','idle_window_order','idle_pack_advance'))
  code= r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stddef.h>
typedef uint64_t resource_size_t;
#define PCI_BRIDGE_RESOURCES 0
#define IORESOURCE_SIZEALIGN 1
#define IORESOURCE_STARTALIGN 2
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define is_power_of_2(x) ((x)&&!((x)&((x)-1)))
#define max(a,b) ((a)>(b)?(a):(b))
struct resource { uint64_t start,end;unsigned long flags;bool parent; };
struct pci_dev { struct resource resource[3];unsigned depth; };
struct list_head {struct pci_dev *dev;};
struct window_save {struct pci_dev *dev;};
struct idle_transaction {bool align_first;};
#define list_entry(p,t,m) ((const struct window_save *)(p))
static unsigned bridge_depth(struct pci_dev *d) {return d->depth;}
static uint64_t resource_size(struct resource *r) {return r->end-r->start+1;}
static int window_order(void *unused,const struct list_head *a,const struct list_head *b) {(void)unused;(void)a;(void)b;return 1;}
"""+functions+r"""
int main(void) {
 struct pci_dev a={.resource={{.start=4,.end=7,.flags=IORESOURCE_STARTALIGN}}};
 struct pci_dev b={.resource={{.start=8,.end=10,.flags=IORESOURCE_STARTALIGN}}};
 struct list_head wa={&a},wb={&b};struct idle_transaction t={.align_first=true};
 assert(idle_window_order(&t,&wa,&wb)>0);
 uint64_t old=0,alt=0;
 assert(!idle_pack_advance(&old,4,4));assert(!idle_pack_advance(&old,8,3));
 assert(!idle_pack_advance(&alt,8,3));assert(!idle_pack_advance(&alt,4,4));
 assert(old==11&&alt==8);
 t.align_first=false;assert(idle_window_order(&t,&wa,&wb)==1);
 return 0;
}
"""
  with tempfile.TemporaryDirectory()as tmp:
   c=Path(tmp)/'test.c';exe=Path(tmp)/'test';c.write_text(code)
   subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True)
