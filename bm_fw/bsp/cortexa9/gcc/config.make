# configuration for xilinx BSP makefile
# created by reverse engineering and shotgun debugging...
# 2015 Lukas Schrittwieser
#
# NOTE: you have to edit xparameters.h according to your system (eg CPU clock freq etc)
# NOTE2: Define USE_AMP=1 to compile the lib for asymmetric multi processing (code runs on cpu1 and assumes some
#        system configs have been made by cpu0 during startup, see xapp 1078)

COMPILER_FLAGS= -O2 -c -fPIC
EXTRA_COMPILER_FLAGS= -g -Wall -DUSE_AMP=1

# configure which libs to build
LIBS = standalone_libs

# configure BSP sources: all *.c, *.s and *.S in current director, in .. and in common
LIBSOURCES = ./*.c ./*.s ./*.S ../*.c ../../common/*.c

INCLUDEFILES=../*.h ./*.h ../../common/*.h
