/*
  udpfs_bd_menu: entry point of the UDPFS block device in the OPL menu.

  Neutrino's main.c connects once, at load, and gives up if no server
  answers. Here udpfs_bd_start (udpfs_bd.c) runs a watcher thread
  instead: it keeps looking for the server in the background, registers
  the "udp" block device with BDM when one answers, pings it every few
  seconds, and takes the device away and brings it back when the server
  is restarted or serves another image. OPL's device list follows, the
  way it follows a USB stick pulled and plugged in again.

  OPL loads this module only after the menu's network is up (lwIP has an
  address), because the watcher opens a socket at once.

  Licenced under Academic Free License version 3.0.
*/

#include <loadcore.h>
#include <irx.h>

#include "main.h"

int udpfs_bd_start(void);

IRX_ID("udpfs_bd_menu", 1, 1);

int _start(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    if (udpfs_bd_start() != 0)
        return MODULE_NO_RESIDENT_END;

    return MODULE_RESIDENT_END;
}
