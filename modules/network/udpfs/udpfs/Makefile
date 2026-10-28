# Neutrino UDPFS client, block-device variant (Rick Gaiser, AFL-3.0).
#
#   make            udpfs_bd.irx       in-game: Neutrino SMAP + ministack
#   make MENU=1     udpfs_bd_menu.irx  OPL menu: the same protocol code over
#                                      the menu's lwIP stack (lwip_mstack.c)
MENU ?= 0

IOP_INCS += -I. -I../include
IOP_CFLAGS += -mno-check-zero-division -DUDPRDMA_MAX_SOCKETS=1 -DFEATURE_UDPFS_BD

ifeq ($(MENU),1)
IOP_BIN      = udpfs_bd_menu.irx
IOP_OBJS_DIR = obj.menu/
IOP_OBJS     = menu.o udprdma.o udpfs_core.o udpfs_bd.o lwip_mstack.o lwip_glue.o imports.o exports.o
IOP_CFLAGS  += -DUDPFS_MENU
else
IOP_BIN      = udpfs_bd.irx
IOP_OBJS_DIR = obj.ingame/
IOP_OBJS     = main.o udprdma.o udpfs_core.o udpfs_bd.o imports.o exports.o
endif

include $(PS2SDK)/Defs.make
include ../../../Rules.bin.make
include $(PS2SDK)/samples/Makefile.iopglobal
