# AetherFS Kernel Module Makefile

obj-m := aetherfs.o
aetherfs-objs := aetherfs_mod.o btree.o balloc.o cow.o data_modes.o metadata_cow.o folio.o extent.o security.o journal.o inode.o super.o dir_idx.o inline.o dense.o reflink.o recovery.o percpu.o invariants.o

CC := gcc
CFLAGS := -Wall -Wextra -O2 -fPIC -D__KERNEL__ \
	-I/usr/src/linux-headers-6.17.0-20-generic/include \
	-Wno-error

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

help:
	$(MAKE) -C $(KDIR) M=$(PWD) help

.PHONY: all clean install help