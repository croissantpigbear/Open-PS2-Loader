# UDPFS: games from a PC, over the network

OPL-RA can run games from a PC that serves a disk with Neutrino's UDPFS
server. To OPL that disk is one more BDM device, next to USB, iLink, MX4SIO
and the internal HDD. The games, art, configs and VMCs sit on it exactly as
they would on a USB stick. It shows up as **UDPFS Games**.

This is the network route that works with RetroAchievements. With SMB, a
game that has an achievement set stops loading after about a minute,
because the SMB stream and the telemetry share OPL's lwIP stack and its
receive ring. UDPFS games run on Neutrino's own SMAP driver and
"ministack": a small UDP stack without TCP, a tcpip thread or mailboxes.
The telemetry sender is built for that stack (`raudp_ms.irx`), so the disc
reads and the snapshots no longer fight over one stack.

## PC side

You need Python 3 and Neutrino's server, `pc/udpfs_server.py`, from
<https://github.com/rickgaiser/neutrino> (the whole `pc/` folder, the server
imports `compressed_iso/` from beside it). Serve a disk *image* or a real
drive formatted FAT32 or exFAT, laid out like an OPL USB stick (`DVD/`,
`CD/`, `ART/`, `CFG/`, `VMC/`, ...):

    python udpfs_server.py -b opl-disk.img

On Linux a whole drive works too, for example the USB stick you already use
with OPL: `sudo python udpfs_server.py -b /dev/sdX`. **Unmount it first.**
The PS2 writes to the disk (configs, VMCs, the RetroAchievements watch
lists), and a PC that has the same FAT mounted at the same time corrupts
it.

The server listens on UDP 62966 (0xF5F6) and answers the console's
broadcast, so there is no address to configure. Your firewall has to allow
inbound UDP to it. xeRAbora runs alongside as usual (UDP 18194).

## Console side

1. **Network settings**: the console needs an address, DHCP or static.
2. **Settings**: BDM start mode on (Auto or Manual), and in **BDM devices**
   switch **UDPFS** on. Save.
3. The menu brings the network up once, then looks for the server in the
   background and adds **UDPFS Games** when it answers. It keeps looking,
   so the server can start after the console. If the network itself fails
   (no cable, no DHCP), UDPFS stays off until OPL is restarted.
4. RetroAchievements works as it does from USB: **RA: check game support**
   once per game, then start it.

The server must keep running while you play. The game reads its disc from
it.

## Limits

- PS1 games (POPStarter) do not run from UDPFS. POPStarter reads the
  device with its own drivers, and none of them speak UDPFS.
- Games that use the network adapter themselves (online modes, LAN play)
  cannot have it. The SMAP driver belongs to the disc stream, and the
  game's own network modules get stand-ins, as on an SMB share.
- One console per server. The server keeps one connection's state.
- In a game, the SMAP driver always auto-negotiates the link. A forced
  Ethernet mode in Network settings applies to the menu only.
- If the server goes away during play, the game stops at its next disc
  read.

## Where the code comes from

Neutrino by Rick Gaiser, AFL-3.0 (`LICENSE`), at commit
`7be8de2798c99af433c91993e021746a54d6800d`. The protocol documents are
`UDPFS.md` and `UDPRDMA.md`.

| Here | Neutrino | In OPL |
|---|---|---|
| `smap/` | `iop/smap` | in-game SMAP driver, `udpfs_smap_irx` |
| `ministack/` | `iop/ministack` | in-game ARP/IPv4/UDP, `udpfs_mstack_irx` |
| `udpfs/` | `iop/udpfs` (BD variant) | in-game block device, `udpfs_bd_irx`; with `MENU=1` the menu's, `udpfs_bd_menu_irx` |
| `include/` | the headers the three share | |

Local changes, kept small so they can be compared with upstream:

- **Build.** OPL's `Rules.bin.make` replaces Neutrino's `Rules.make`. The
  `_retonly` stub moved below the export table in `smap/exports.tab` and
  `ministack/exports.tab`, which the current `iopfixup` requires.
- **No udptty in ministack.** It replaces the IOP's TTY device and
  broadcasts every `printf` to UDP 18194, which is xeRAbora's port.
- **udprdma.c, three fixes** (each marked "OPL change"; worth sending
  upstream):
  - The ACK check, `(int16_t)((acked - sent) & 0xFFF)`, can never be
    negative, so any ACK counted for the packet just sent. That included
    the server's routine second ACK of the previous packet. A lost request
    then went unnoticed.
  - A stale ACK now neither ends the wait nor triggers a retransmit.
    Either one broke multi-chunk block writes.
  - `udprdma_recv` waits in 150 ms slices and sends a NACK when a started
    transfer goes quiet. A lost FIN, or a lost window ACK (the server
    gives up after 400 ms), used to cost the full timeout and then the
    connection.
- **Menu build** (`MENU=1`): `menu.c` replaces `main.c` and keeps retrying
  discovery in the background. `lwip_mstack.c` and `lwip_glue.c` stand in
  for ministack and the SMAP receive path on top of the menu's lwIP,
  because the menu's network already belongs to ps2sdk's SMAP driver. The
  protocol code is the same files as in the game.

The menu build was tested on a PC against `udpfs_server.py`, with the IOP
kernel calls emulated by POSIX threads: 200 reads of 1 to 300 sectors and
30 writes of 1 to 64 sectors, each read back, all byte-exact. It passed on
a clean link, with every 3rd, 4th or 5th request dropped, and with every
2nd or 3rd ACK dropped. With upstream `udprdma.c` the write test stalled at
the first lost request, and a lost ACK cost a 30-second wait and then the
connection. The in-game modules have only been built, not run on a
console.
