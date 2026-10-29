/*
  udpfs_bd: entry point of the UDPFS block device in a running game.

  Neutrino's main.c connects once at load. OPL starts the reconnecting
  watcher instead (udpfs_bd_start in udpfs_bd.c): it still connects
  before returning when the server answers, so the game's first disc read
  finds the disk, and if the server goes away later the game waits for it
  rather than dying.

  Licenced under Academic Free License version 3.0.
*/

#include <loadcore.h>
#include <irx.h>

#include "main.h"

int udpfs_bd_start(void);

IRX_ID(MODNAME, 0x2, 0x1A);

int _start(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    if (udpfs_bd_start() != 0)
        return MODULE_NO_RESIDENT_END;

    return MODULE_RESIDENT_END;
}
