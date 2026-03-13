obj-m += rcxkm.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	insmod rcxkm.ko

uninstall:
	rmmod rcxkm

.PHONY: all clean install uninstall
