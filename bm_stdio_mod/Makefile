obj-m += bm_stdio.o

KDIR = ../../../linux-xlnx/

all:
	make ARCH=arm CROSS_COMPILE=arm-xilinx-linux-gnueabi- -C $(KDIR) M=$(PWD) modules
	arm-xilinx-linux-gnueabi-objdump -D bm_stdio.ko > bm_stdio.lss
	sudo cp bm_stdio.ko /srv/rootfs/root/remoteproc

#clean:
#	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
