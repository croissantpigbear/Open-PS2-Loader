# OPL-RA with UDPFS

This branch (`udpfs`) adds UDPFS to [hacan359's OPL-RA](https://github.com/hacan359/Open-PS2-Loader):
games served from a PC or Mac over the network, with RetroAchievements.

- **Folder mode**: point the server at your OPL folder (DVD, CD, CFG, ART,
  VMC...) and the games show up in OPL's list; ISO, ZSO and CSO.
- **VHD mode**: a whole disk image served as a block device.
- Reconnects when the server restarts, in the menu and in a game.
- Themes: `#Format` (ZSO/CSO badges), `#LongName`, and a `CoverRow` element
  with `horizontal_list=1` for a row of covers.

The server and the apps (UDPFS Server, OPL Library) are in
[croissantpigbear/udpfs-server](https://github.com/croissantpigbear/udpfs-server),
with ready builds of this ELF and two themes on its Releases page.

UDPFS comes from [Neutrino](https://github.com/rickgaiser/neutrino) by Rick
Gaiser. Build as usual for OPL (`make` in the ps2homebrew Docker image).
