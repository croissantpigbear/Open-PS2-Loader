/*
  RA: layout of the game-memory snapshot that ee_core writes into IOP
  memory every frame and the raudp IOP module reads from there.

  Why DMA into a buffer instead of a SIF command: a SIF command number is
  an index into the handler table, and in-game that table belongs to the
  game (OPL's own side allocates exactly one slot, see SifSetCmdBuffer in src/system.c). An
  index past the end writes into someone else's memory, and
  sceSifAddCmdHandler returns void, so the error stays silent. The shared
  tables are left alone: ee_core allocates a buffer in the IOP heap with
  SifAllocIopHeap, passes the address to the module as a load argument,
  and writes to it directly from then on.

  Torn writes: DMA is not atomic and copies from the start of the buffer
  to the end, so seq is repeated in a trailer word placed AFTER the
  values (RA_SNAP_TRAILER_OFF). The reader takes seq from the header,
  copies the values, then compares the trailer and the header again; any
  difference means the snapshot was overwritten mid-copy and is skipped.
  The seq_end field inside the header cannot detect this: it is written
  before the values arrive.

  Slots: ee_core writes snapshot seq into slot (seq % slots) of a buffer
  of `slots` equal slots, `stride` bytes apart; both numbers ride in every
  header. With one buffer, a game that keeps the SIF busy (Rayman 2:
  Revolution streams all the time) had each snapshot land while raudp was
  still reading the previous one, or overwritten before it went out: a
  third of all frames never reached the PC. With four, raudp reads a slot
  the EE will not touch for three frames, and sends them in seq order. A
  writer that sets slots to 0 or 1 (rapull, under POPS) gets the single
  buffer behaviour.
*/

#ifndef __RA_SNAP_H__
#define __RA_SNAP_H__

#define RA_SNAP_MAGIC 0x52415331 /* "RAS1" */

#include "ra_watch.h"

/* Snapshot header, 64 bytes, a multiple of 16 as SIF DMA requires. The
   values follow, packed back to back in watch list order. Addresses are
   not sent: the PC client generated the watch list and knows the order.

   The tear check uses the trailer word after the values, not seq_end. */
struct ra_snap
{
    unsigned int magic;    /* RA_SNAP_MAGIC; tells a snapshot from garbage */
    unsigned int seq;      /* increments every frame; written first */
    unsigned int frames;   /* EE frame counter since launch */
    unsigned int dma_skip; /* frames skipped: previous DMA still in flight */
    unsigned int dma_fail; /* isceSifSetDma failures */
    unsigned int count;    /* entries in the watch list */
    unsigned int bytes;    /* bytes of values that follow */
    unsigned int seq_end;  /* copy of seq (header only; see the trailer) */
    /* Serial of the running game, e.g. "SLUS_210.65". The PC client
       uses it to pick the watch list and the image hash to report to
       RetroAchievements, so the user never names the game by hand. */
    char game_id[16];
    /* What the reads cost on the EE: COP0 Count ticks spent reading the
       values and resolving chains for this snapshot, and ticks between
       the last two VBLANKs, so the PC can turn the first into a share
       of the frame without knowing the clock. */
    unsigned int read_cycles;
    unsigned int frame_cycles;
    /* The slot layout, the same in every header: see the note at the
       top. 0 or 1 means a single buffer. */
    unsigned int slots;
    unsigned int stride;
    /* followed by bytes bytes of values, then the trailer word */
};

#define RA_SNAP_HDR ((int)sizeof(struct ra_snap))

/* Offset of the trailer word (a copy of seq) after the values, 4-aligned */
#define RA_SNAP_TRAILER_OFF(bytes) (RA_SNAP_HDR + (((bytes) + 3) & ~3))

/* Transfer size, trailer included, rounded up to 16: SIF DMA moves quadwords */
#define RA_SNAP_DMA_SIZE(bytes) ((RA_SNAP_TRAILER_OFF(bytes) + 4 + 15) & ~15)

/* Buffer size on both sides: the largest transfer, rounded up to a
   64-byte cache line so the EE buffer shares no line with other data */
#define RA_SNAP_TOTAL_FOR(bytes) ((RA_SNAP_DMA_SIZE(bytes) + 63) & ~63)
#define RA_SNAP_TOTAL            RA_SNAP_TOTAL_FOR(RA_SNAP_MAX_BYTES)

/* Slots ee_core asks for; fewer when the IOP heap cannot spare them */
#define RA_SNAP_SLOTS 4

/* PC -> game side: raudp DMAs this 16-byte record into an ee_core buffer
   whose address came as a load argument; the VBLANK handler treats a new
   seq as a new event. */
#define RA_EVENT_MAGIC 0x52414531 /* "RAE1" */

#define RA_EVENT_UNLOCK 1 /* an achievement unlocked; arg is its id */
#define RA_EVENT_RESET  2 /* leave the game for the loader, as the IGR combo does */

struct ra_event
{
    unsigned int magic; /* RA_EVENT_MAGIC */
    unsigned int seq;   /* increments per event; 0 means none yet */
    unsigned int kind;  /* RA_EVENT_* */
    unsigned int arg;
};

/* The load argument ee_core hands raudp as argv[1], comma separated:
     RA_ARG_SNAP   eight hex digits, the snapshot buffer in IOP RAM
     RA_ARG_EVENT  eight hex digits, the event buffer in EE RAM
     RA_ARG_RX     '1' or '0', whether raudp may read from the network
                   while the game runs
     RA_ARG_ID     the game's serial, up to RA_ARG_ID_MAX characters
   argv[2] is SMAP's ipconfig string. Both sides build and parse the
   argument by these offsets. */
#define RA_ARG_SNAP   0
#define RA_ARG_EVENT  9
#define RA_ARG_RX     18
#define RA_ARG_ID     20
#define RA_ARG_ID_MAX 15
#define RA_ARG_MAX    (RA_ARG_ID + RA_ARG_ID_MAX) /* characters before the terminator */

#endif /* __RA_SNAP_H__ */
