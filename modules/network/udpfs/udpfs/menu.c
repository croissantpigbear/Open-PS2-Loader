/*
  udpfs_bd_menu: entry point of the UDPFS block device in the OPL menu.

  Neutrino's main.c connects once, at load, and gives up if no server
  answers. Here udpfs_bd_start (udpfs_bd.c) runs a watcher thread
  instead: it keeps looking for the server in the background, registers
  the "udp" block device with BDM when one answers, pings it every few
  seconds, and takes the device away and brings it back when the server
  is restarted or serves another image. OPL's device list follows, the
  way it follows a USB stick pulled and plugged in again.

  The same connection serves the "udpfs:" file device (udpfs_fs.c), for
  a server that shares a folder (-d) instead of, or as well as, an image.

  OPL loads this module only after the menu's network is up (lwIP has an
  address), because the watcher opens a socket at once.

  Licenced under Academic Free License version 3.0.
*/

#include <loadcore.h>
#include <irx.h>

#include "main.h"

int udpfs_bd_start(void);
int udpfs_fs_init(void);

IRX_ID("udpfs_bd_menu", 1, 1);

int _start(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* The file device first: it only answers -EIO until the watcher has
       a server. */
    if (udpfs_fs_init() != 0 || udpfs_bd_start() != 0)
        return MODULE_NO_RESIDENT_END;

    return MODULE_RESIDENT_END;
}
