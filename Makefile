obj-m += mydev.o mydev_platform.o mydev_irq_tasklet.o mydev_i2c.o vcam.o

KDIR := /lib/modules/$(shell uname -r)/build

all:
	make -C $(KDIR) M=$(PWD) modules
	gcc -O2 -Wall -o capture capture.c

clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -f capture
