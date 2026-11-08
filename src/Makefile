obj-m += pci_pf_bar_recover.o
KERNEL_RELEASE ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KERNEL_RELEASE)/build
.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) KCFLAGS="$(KCFLAGS) -ffile-prefix-map=$(CURDIR)=/usr/src/pci-pf-bar-recover" modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
