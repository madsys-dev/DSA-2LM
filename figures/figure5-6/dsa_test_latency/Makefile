obj-m += dsa_module.o
dsa_module-y := dsa_test.o init.o irq.o device.o sysfs.o submit.o dma.o cdev.o debugfs.o perfmon.o bus.o
# ccflags += -g -O0

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

run:
	insmod dsa_module.ko
	rmmod dsa_module.ko
	dmesg