obj-m += dsa_module.o
dsa_module-y := dsa_test.o
# ccflags += -std=gnu99 -g -O0
# ccflags-y += -std=gnu99 -g -O0
# ccflags += -g -O0

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

run:
	insmod dsa_module.ko
	rmmod dsa_module.ko
	dmesg