# Neutrino SMAP driver (Rick Gaiser, AFL-3.0), unchanged apart from the
# build: OPL's rules instead of Neutrino's. See ../README.md.
IOP_BIN  = smap.irx
IOP_OBJS = main.o smap.o xfer.o imports.o exports.o

IOP_INCS += -I. -I../include
IOP_CFLAGS += -mno-check-zero-division

include $(PS2SDK)/Defs.make
include ../../../Rules.bin.make
include $(PS2SDK)/samples/Makefile.iopglobal
