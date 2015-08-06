obj-m += cfg_mgmt.o

KDIR = ../../../linux-xlnx/

all:
	make ARCH=arm CROSS_COMPILE=arm-xilinx-linux-gnueabihf- -C $(KDIR) M=$(PWD) modules
	arm-xilinx-linux-gnueabihf-objdump -D cfg_mgmt.ko > cfg_mgmt.lss
	sudo cp cfg_mgmt.ko ~/initrd/export/root

#clean:
#	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
