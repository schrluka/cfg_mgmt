#
# Original makefile from Xilinx SDK (/opt/Xilinx/SDK/2014.4/data/embeddedsw/XilinxProcessorIPLib/drivers)
# Modified by Lukas Schrittwieser, 2015

COMPILER=arm-xilinx-eabi-gcc
ARCHIVER=arm-xilinx-eabi-ar
CP=cp
COMPILER_FLAGS=-c -O2 -Wall
EXTRA_COMPILER_FLAGS=
LIB=libscugic.a

CC_FLAGS = $(COMPILER_FLAGS)
ECC_FLAGS = $(EXTRA_COMPILER_FLAGS)


#modified release and include dir path,
RELEASEDIR=../../lib
INCLUDEDIR=../../include
# added include path for board support package
INCLUDES=-I./. -I${INCLUDEDIR} -I../../../bsp/include

OUTS = *.o

LIBSOURCES:=*.c
INCLUDEFILES:=*.h

OBJECTS =	$(addsuffix .o, $(basename $(wildcard *.c)))

libs: banner scugic_libs clean

%.o: %.c
	${COMPILER} $(CC_FLAGS) $(ECC_FLAGS) $(INCLUDES) -o $@ $<

banner:
	echo "Compiling scugic"

scugic_libs: ${OBJECTS}
	$(ARCHIVER) -r ${RELEASEDIR}/${LIB} ${OBJECTS}

.PHONY: include
include: scugic_includes

scugic_includes:
	${CP} ${INCLUDEFILES} ${INCLUDEDIR}

clean:
	rm -rf ${OBJECTS}

