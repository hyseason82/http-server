obj-m += mydev.o

KDIR := /home/hayes/wsl2-kernel

all:
	make -C $(KDIR) M=$(PWD) KCONFIG_CONFIG=$(KDIR)/Microsoft/config-wsl modules

clean:
	make -C $(KDIR) M=$(PWD) clean
