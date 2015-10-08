# Makefile for standalone applications on ZYNQ7000
#

# select non-linux compiler
CROSS = arm-xilinx-eabi-
#CROSS = arm-xilinx-linux-gnueabihf-
CC = $(CROSS)gcc

# stdio messages are stored in a memory region called trace buffer which can be read via remoteproc (/debug/remoteproc/...)
# specify the buffer's size, it is passed to the C code and the linker
TRACE_BUFFER_SIZE=0x8000

# include path for libgcc headers (through symlic on system to compiler install path)
# and inc path for board support package (bsp)
# furthermore, include xilinx IPLIB
#INC = -Ibsp/include -Iiplib/include
#INC = -Ibsp_xsdk/include
INC = -Ibsp_xsdk/2014.4/include

# compiler config
CFLAGS = -Wall -g -std=c99 -DTRACE_BUFFER_SIZE=$(TRACE_BUFFER_SIZE) $(INC)

# linker config, add search path for libs
#LDFLAGS = -Wl,-Map=$(BIN).map -Wl,-Liplib/lib -Wl,-Lbsp/lib
#LDFLAGS = -Wl,-Map=$(BIN).map -Wl,-Lbsp_xsdk/lib
LDFLAGS = -Xlinker --defsym=TRACE_BUFFER_SIZE=$(TRACE_BUFFER_SIZE) -Wl,-Map=$(BIN).map -Wl,-Lbsp_xsdk/2014.4/lib

# NOTE: the libc has to be linked because libxil provides necessary
#       functions for bare metal (eg startup code), which can not be resolved by the linker
#		this works only by grouping all libs, see link command below (--start-group)
LIBS =  -lxil
LIBS += -lc
LIBS += -lgcc
#LIBS += -lscugic	# use these with old bsp / iplib
#LIBS += -luartps
LIBS += -lm
# Add more xilinx drivers or other libs here


# specify linker script
LSCRIPT = lscript.ld


# list all objects to be compiled and linked
OBJ = main.o remoteproc.o virtio_ring.o config.o config_vars.o

# file name for binary output
BIN = bm_cfg_mgmt

# where to put the object files
OBJPATH = obj

# where to find the source files
VPATH = src src/grbl

C_OBJS = $(addprefix $(OBJPATH)/, $(OBJ))

all: $(C_OBJS) config_vars.h
	$(CC) -T"$(LSCRIPT)" $(LDFLAGS) -o $(BIN) $(C_OBJS) -Wl,--start-group $(LIBS) -Wl,--end-group
	$(CROSS)objdump -D $(BIN) > $(BIN).lss
	$(CROSS)size $(BIN)
#	copy file to destination where the kernel can load it
	cp $(BIN) /srv/rootfs/lib/firmware/firmware
#	scp -i ../../PwmRect/IAF/SoC/ssh-keys/id_TE0720 $(BIN) root@129.132.124.67:/lib/firmware/firmware


$(OBJPATH)/%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c $<

clean:
	rm -f $(OBJPATH)/*.o

