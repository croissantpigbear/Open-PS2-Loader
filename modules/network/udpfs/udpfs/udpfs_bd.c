/*
 * UDPFS BD - Block Device over UDPFS/UDPRDMA
 *
 * Provides block device access over network using UDPRDMA for reliable transport.
 * Uses UDPFS block I/O subset (BREAD/BWRITE) with handle 0.
 * Block device capacity is queried via GETSTAT; sectors are fixed at 512 bytes.
 */

#include <errno.h>
#include <bdm.h>
#include <thbase.h>
#include <thsemap.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "udpfs_core.h"


/* Block device handle - pre-opened by server */
#define BLOCK_DEVICE_HANDLE 0

/* State */
static struct block_device g_udpbd;

/* OPL change: reconnecting. Upstream connects once; if the server goes
 * away the device is gone for good, and in OPL's in-game cdvdman the
 * bdm_disconnect_bd() inside a failed read even deadlocks (cdvdman holds
 * its I/O semaphore around the read, and disconnect waits on it).
 *
 * Here a watcher thread (udpfs_bd_start) owns the connection:
 *  - In the game, a read or write that fails waits for the watcher to
 *    find the server again and is then retried. The game pauses instead
 *    of dying; the block device stays registered throughout.
 *  - In the menu, the watcher also pings the server every few seconds,
 *    so a restarted server is noticed without a read. On loss it takes
 *    the device away and registers it again once the server is back,
 *    so OPL re-reads the disk (which may be another image by then).
 * g_io serialises the protocol: one request on the wire at a time. */
static int g_io = -1;
static int g_registered = 0;
static uint32_t g_sectors = 0;

#define RETRY_WAIT_US     (50 * 1000)
#define WATCH_PERIOD_US   (500 * 1000)
#define DISCOVER_PAUSE_US (2 * 1000 * 1000)
#define PING_PERIOD_US    (4 * 1000 * 1000)

static void _lock(void)
{
    WaitSema(g_io);
}

static void _unlock(void)
{
    SignalSema(g_io);
}


/*
 * Block device read (retry + chunking handled by udpfs_bread)
 */
static int udpfs_bd_read(struct block_device *bd, uint64_t sector, void *buffer, uint16_t count)
{
    int ret;

    if (sector >= bd->sectorCount)
        return -EINVAL;

    if ((sector + count) > bd->sectorCount)
        count = bd->sectorCount - sector;

    for (;;) {
        _lock();
        if (udpfs_core_is_connected())
            ret = udpfs_core_bread(BLOCK_DEVICE_HANDLE, sector, buffer, count, bd->sectorSize);
        else
            ret = -EIO;
        _unlock();

        if (ret >= 0)
            return count;

#ifdef UDPFS_MENU
        M_DEBUG("udpfs_bd: read failed\n");
        return ret;
#else
        /* In a game there is nobody to tell: wait for the server. */
        DelayThread(RETRY_WAIT_US);
#endif
    }
}


/*
 * Write sectors via shared block I/O helper (zero-copy)
 */
static int udpfs_bd_write(struct block_device *bd, uint64_t sector, const void *buffer, uint16_t count)
{
    int ret;

    M_DEBUG("udpfs_bd: write sector=%u, count=%u\n", (uint32_t)sector, count);

    if (sector >= bd->sectorCount)
        return -EINVAL;

    if ((sector + count) > bd->sectorCount)
        count = bd->sectorCount - sector;

    for (;;) {
        _lock();
        if (udpfs_core_is_connected())
            ret = udpfs_core_bwrite(BLOCK_DEVICE_HANDLE, sector, buffer, count, bd->sectorSize);
        else
            ret = -EIO;
        _unlock();

        if (ret >= 0)
            return count;

#ifdef UDPFS_MENU
        return -EIO;
#else
        DelayThread(RETRY_WAIT_US);
#endif
    }
}


static void udpfs_bd_flush(struct block_device *bd)
{
    M_DEBUG("udpfs_bd: flush\n");
}


static int udpfs_bd_stop(struct block_device *bd)
{
    M_DEBUG("udpfs_bd: stop\n");
    return 0;
}


/*
 * Initialize UDPFS block device driver
 */
int udpfs_bd_init(void)
{
    int ret;

    M_DEBUG("UDPFS BD over UDPRDMA\n");

    /* Initialize core UDPFS */
    ret = udpfs_core_init();
    if (ret < 0) {
        return -1;
    }

    /* Get disk capacity via GETSTAT (returns sector count) */
    ret = udpfs_core_get_sector_count(BLOCK_DEVICE_HANDLE);
    if (ret < 0) {
        udpfs_core_exit();
        return -1;
    }

    /* Setup block device with fixed 512-byte sectors */
    g_udpbd.name = "udp";
    g_udpbd.path = "udpbd";
    g_udpbd.devNr = 0;
    g_udpbd.parNr = 0;
    g_udpbd.sectorOffset = 0;
    g_udpbd.sectorSize = 512;      /* Fixed sector size */
    g_udpbd.sectorCount = ret;     /* From GETSTAT / 512 */
    g_udpbd.priv = NULL;
    g_udpbd.read = udpfs_bd_read;
    g_udpbd.write = udpfs_bd_write;
    g_udpbd.flush = udpfs_bd_flush;
    g_udpbd.stop = udpfs_bd_stop;

    /* Connect to BDM */
    g_sectors = (uint32_t)ret;
    g_registered = 1;
    bdm_connect_bd(&g_udpbd);

    M_DEBUG("udpfs_bd: ready (sectorSize=%u, sectorCount=%u)\n",
            g_udpbd.sectorSize, g_udpbd.sectorCount);
    return 0;
}


/*
 * OPL addition: the watcher. Connects (or keeps trying), then keeps the
 * connection: see the note at g_io. Runs as its own thread for the life
 * of the module.
 */
#ifdef UDPFS_MENU
static void _drop(void)
{
    if (g_registered) {
        g_registered = 0;
        bdm_disconnect_bd(&g_udpbd);
    }
}
#endif

static void _watch(void *arg)
{
    (void)arg;

    for (;;) {
        if (!g_registered) {
            /* First connection, or (menu) after a loss: discovery, size,
             * registration with BDM. */
            _lock();
            if (udpfs_bd_init() == 0)
                M_PRINTF("server found, %u sectors\n", g_sectors);
            _unlock();
            if (!g_registered)
                DelayThread(DISCOVER_PAUSE_US);
            continue;
        }

#ifdef UDPFS_MENU
        DelayThread(PING_PERIOD_US);

        /* Idle menu: ask for the size. A restarted server answers on
         * another port and with fresh sequence numbers, so this fails
         * against it; a different image answers with another size. */
        {
            int lost, n;

            _lock();
            n = udpfs_core_is_connected() ? udpfs_core_get_sector_count(BLOCK_DEVICE_HANDLE) : -1;
            lost = n < 0 || (uint32_t)n != g_sectors;
            if (lost)
                udpfs_core_exit();
            _unlock();

            if (lost) {
                M_PRINTF("server lost, looking again\n");
                _drop();
            }
        }
#else
        DelayThread(WATCH_PERIOD_US);

        /* In a game the reads themselves notice a loss. Keep the block
         * device and only find the server again; the waiting read then
         * goes through. */
        _lock();
        if (!udpfs_core_is_connected()) {
            M_PRINTF("server lost, looking again\n");
            udpfs_core_exit();
            while (udpfs_core_init() != 0)
                DelayThread(RETRY_WAIT_US);
            M_PRINTF("server back\n");
        }
        _unlock();
#endif
    }
}

int udpfs_bd_start(void)
{
    iop_sema_t sema;
    iop_thread_t t;
    int tid;

    sema.attr = 0;
    sema.option = 0;
    sema.initial = 1;
    sema.max = 1;
    if ((g_io = CreateSema(&sema)) < 0)
        return -1;

#ifndef UDPFS_MENU
    /* The game's first disc read follows this module's load. Connect
     * before returning when the server is there, as upstream does; the
     * watcher keeps trying otherwise, and cdvdman waits for the disk. */
    if (udpfs_bd_init() == 0)
        M_PRINTF("server found, %u sectors\n", g_sectors);
#endif

    t.attr = TH_C;
    t.option = 0;
    t.thread = _watch;
    t.stacksize = 0x800;
    t.priority = 0x50;
    if ((tid = CreateThread(&t)) < 0)
        return -1;
    StartThread(tid, NULL);

    return 0;
}
