# Neutrino ministack (Rick Gaiser, AFL-3.0): ARP, IPv4 and UDP on top of
# the Neutrino SMAP driver. Built without udptty, which would broadcast
# the IOP's printf output to UDP 18194, the port xeRAbora listens on.
IOP_BIN  = ministack.irx
IOP_OBJS = main.o ministack_eth.o ministack_arp.o ministack_ip.o ministack_udp.o exports.o imports.o

IOP_INCS += -I. -I../include
IOP_CFLAGS += -mno-check-zero-division

include $(PS2SDK)/Defs.make
include ../../../Rules.bin.make
include $(PS2SDK)/samples/Makefile.iopglobal
