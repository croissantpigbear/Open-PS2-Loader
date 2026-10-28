# raudp.irx     OPL's in-game SMAP driver and SMSTCPIP (USB, HDD, MMCE, disc, SMB)
# raudp_ms.irx  Neutrino's SMAP driver and ministack (games from a UDPFS server)
MS ?= 0

IOP_INCS += -I../common -I../../iopcore/common

ifeq ($(MS),1)
IOP_BIN      = raudp_ms.irx
IOP_OBJS_DIR = obj.ms/
IOP_INCS    += -I../udpfs/include
IOP_CFLAGS  += -DRAUDP_MINISTACK
else
IOP_BIN      = raudp.irx
IOP_OBJS_DIR = obj/
endif
IOP_OBJS = raudp.o imports.o exports.o

include $(PS2SDK)/Defs.make
include ../../Rules.bin.make
include $(PS2SDK)/samples/Makefile.iopglobal
