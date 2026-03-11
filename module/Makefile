obj-m += klocklab.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

ccflags-y += -I$(PWD)/../common

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean