obj-m += cfg_mgmt.o

all:
	make ARCH=arm CROSS_COMPILE=arm-xilinx-linux-gnueabihf- -C ../../../linux-xlnx/  M=$(PWD) modules
#	cp cfg_mgmt.ko /srv/rootfs

#clean:
#	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
