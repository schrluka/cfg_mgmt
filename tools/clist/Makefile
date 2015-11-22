CROSS=arm-xilinx-linux-gnueabi-

BIN=clist

all:
	$(CROSS)gcc -Wall -o $(BIN) -std=gnu99 src/clist.c
	sudo cp $(BIN) /srv/rootfs/root/remoteproc
