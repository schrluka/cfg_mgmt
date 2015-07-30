# original Makefile from Xilinx SDK (/opt/Xilinx/SDK/2014.4/data/embeddedsw/XilinxProcessorIPLib/drivers
# modified by Lukas Schrittwieser to make it build (some black magic xilinx build tools are usually required)

COMPILER=arm-xilinx-eabi-gcc
ARCHIVER=arm-xilinx-eabi-ar
CP=cp
COMPILER_FLAGS=-c -O2 -Wall
EXTRA_COMPILER_FLAGS=
LIB=libuartps.a

CC_FLAGS = $(COMPILER_FLAGS)
ECC_FLAGS = $(EXTRA_COMPILER_FLAGS)

RELEASEDIR=../../lib
INCLUDEDIR=../../include
INCLUDES=-I./. -I${INCLUDEDIR} -I../../../bsp/include

OUTS = *.o

LIBSOURCES:=*.c
INCLUDEFILES:=*.h

OBJECTS =	$(addsuffix .o, $(basename $(wildcard *.c)))

libs: banner xuartps_libs clean

%.o: %.c
	${COMPILER} $(CC_FLAGS) $(ECC_FLAGS) $(INCLUDES) -o $@ $<

banner:
	echo "Compiling uartps"

xuartps_libs: ${OBJECTS}
	$(ARCHIVER) -r ${RELEASEDIR}/${LIB} ${OBJECTS}

.PHONY: include
include: xuartps_includes

xuartps_includes:
	${CP} ${INCLUDEFILES} ${INCLUDEDIR}

clean:
	rm -rf ${OBJECTS}

