/*
  UDPFS folder mode: the files a game reads, and how they are addressed.

  A game from a folder served by udpfs_server.py (-d) still runs on OPL's
  BDM cdvdman: the in-game "udp" block device is not a disk but a row of
  files, file k occupying the sectors from k << UDPFS_FILE_SHIFT up. Its
  reads become UDPFS block reads (BREAD) on that file's server handle.

  The menu's "udpfs:" device answers OPL's fragment and LBA questions
  with those addresses and remembers the path behind each index. Before a
  launch OPL fetches that table (UDPFS_DEVCTL_FILES) and writes it into
  the in-game module's copy of this structure (found by its magic), so
  the game opens the same files by path.

  2^28 sectors are 128 GiB per file; 8 files keep every address below
  2^31, so the VMC emulator's 32-bit LBAs reach them too.

  Licenced under Academic Free License version 3.0.
*/

#ifndef UDPFS_FILES_H
#define UDPFS_FILES_H

#define UDPFS_FILES_MAGIC "UDPFSFT1"
#define UDPFS_FILES_MAX   8
#define UDPFS_FILE_PATH   160
#define UDPFS_FILE_SHIFT  28

/* devctl on "udpfs:": copies struct udpfs_files into the reply buffer */
#define UDPFS_DEVCTL_FILES 0x55460001
/* devctl on "udpfs:": 0 while there is no server, else a number that
   changes with every new connection. OPL's BDM page polls it the way it
   watches BDM's connect and disconnect events for a USB stick. */
#define UDPFS_DEVCTL_STATE 0x55460002

struct udpfs_files
{
    char magic[8];        /* UDPFS_FILES_MAGIC */
    unsigned int count;   /* 0: whole-disk mode, the server's -b image */
    unsigned int reserved;
    char path[UDPFS_FILES_MAX][UDPFS_FILE_PATH];
};

#endif
