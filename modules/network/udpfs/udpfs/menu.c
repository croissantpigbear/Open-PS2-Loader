/*
  udpfs_bd_menu: entry point of the UDPFS block device in the OPL menu.

  Neutrino's main.c connects once, at load, and gives up if no server
  answers. In the menu the PC side may well start after the console, so
  this build keeps trying in the background: every attempt is one
  UDPRDMA discovery (about five seconds of broadcasts), then a pause.
  Once a server answers, udpfs_bd_init() registers the "udp" block
  device with BDM, and OPL's device list picks it up the way it picks up
  a USB stick plugged in late.

  OPL loads this module only after the menu's network is up (lwIP has an
  address), because the first thing it does is open a socket.

  Licenced under Academic Free License version 3.0.
*/

#include <loadcore.h>
#include <thbase.h>
#include <irx.h>

#include "main.h"

int udpfs_bd_init(void);

IRX_ID("udpfs_bd_menu", 1, 0);

#define RETRY_PAUSE_US (3 * 1000 * 1000)

static void connect_thread(void *arg)
{
    (void)arg;

    while (udpfs_bd_init() != 0)
        DelayThread(RETRY_PAUSE_US);

    M_PRINTF("connected, block device registered\n");
}

int _start(int argc, char *argv[])
{
    iop_thread_t t;
    int tid;

    (void)argc;
    (void)argv;

    t.attr = TH_C;
    t.option = 0;
    t.thread = connect_thread;
    t.stacksize = 0x1000;
    t.priority = 0x50;

    tid = CreateThread(&t);
    if (tid < 0)
        return MODULE_NO_RESIDENT_END;
    StartThread(tid, NULL);

    return MODULE_RESIDENT_END;
}
