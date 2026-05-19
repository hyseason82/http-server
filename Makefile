obj-m += mydev.o mydev_platform.o mydev_irq_tasklet.o mydev_i2c.o

KDIR := /lib/modules/$(shell uname -r)/build

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean
