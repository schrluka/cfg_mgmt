CROSS=arm-xilinx-linux-gnueabihf-

BIN=clist

all:
	$(CROSS)gcc -Wall -o $(BIN) -std=gnu99 src/clist.c
#	sudo cp $(BIN) /srv/rootfs/root/remoteproc


install_pes: all
	sudo cp $(BIN) /home/pes/initrd/export/root
