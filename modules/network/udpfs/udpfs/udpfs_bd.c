/*
 * UDPFS BD - Block Device over UDPFS/UDPRDMA
 *
 * Provides block device access over network using UDPRDMA for reliable transport.
 * Uses UDPFS block I/O subset (BREAD/BWRITE) with handle 0.
 * Block device capacity is queried via GETSTAT; sectors are fixed at 512 bytes.
 *
 * OPL: based on Neutrino's udpfs_bd.c, which connects once and serves the
 * server's -b image. This version
 *  - owns the connection in a watcher thread and reconnects (udpfs_bd_start);
 *  - in the game, can serve files instead of the image (folder mode, see
 *    udpfs_files.h): the block device is a row of files, read by BREAD on
 *    each file's handle;
 *  - in the menu, shares the connection with the "udpfs:" file device
 *    (udpfs_fs.c) through udpfs_lock() and udpfs_generation().
 */

#include <errno.h>
#include <bdm.h>
#include <thbase.h>
#include <thsemap.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "udpfs_core.h"
#include "udpfs_files.h"
#include "udpfs_lock.h"


/* Block device handle - pre-opened by server */
#define BLOCK_DEVICE_HANDLE 0

/* Directory bit in a GETSTAT reply's mode (UDPFS.md: FIO_S_IFDIR) */
#define FIO_S_IFDIR_UDPFS 0x1000

/* open() flags as UDPFS carries them (UDPFS.md, OPEN_REQ) */
#define UDPFS_O_RDONLY 0x0001
#define UDPFS_O_RDWR   0x0003

/* State */
static struct block_device g_udpbd;

/* The connection, see udpfs_lock.h. g_io serialises the protocol: one
 * request on the wire at a time. g_gen counts connections, so handles
 * from an earlier one are recognised as dead. */
static int g_io = -1;
static unsigned int g_gen = 0;
static int g_up = 0;         /* connected to a server */
static int g_registered = 0; /* block device registered with BDM */
static uint32_t g_sectors = 0;

#define RETRY_WAIT_US     (50 * 1000)
#define WATCH_PERIOD_US   (500 * 1000)
#define DISCOVER_PAUSE_US (2 * 1000 * 1000)
#define PING_PERIOD_US    (4 * 1000 * 1000)

void udpfs_lock(void)
{
    WaitSema(g_io);
}

void udpfs_unlock(void)
{
    SignalSema(g_io);
}

unsigned int udpfs_generation(void)
{
    return g_gen;
}

#ifndef UDPFS_MENU
/* Folder mode in the game: the files, written into this module by OPL
 * before the launch (bdmsupport.c, found by the magic). count 0 means the
 * server's whole-disk image, as upstream. */
struct udpfs_files udpfs_game_files = {UDPFS_FILES_MAGIC, 0, 0, {{0}}};

static int32_t g_file_handle[UDPFS_FILES_MAX];
static unsigned int g_file_gen[UDPFS_FILES_MAX];

/* Server handle for file k on this connection, opened on first use.
 * Called with g_io held. */
static int32_t _file_handle(unsigned int k)
{
    int32_t h;

    if (g_file_gen[k] == g_gen && g_file_handle[k] >= 0)
        return g_file_handle[k];

    /* Read-write for a VMC; an ISO on a read-only share still opens. */
    if (udpfs_core_open(udpfs_game_files.path[k], UDPFS_O_RDWR, 0, 0, &h) < 0 &&
        udpfs_core_open(udpfs_game_files.path[k], UDPFS_O_RDONLY, 0, 0, &h) < 0) {
        M_PRINTF("cannot open %s\n", udpfs_game_files.path[k]);
        return -1;
    }

    g_file_handle[k] = h;
    g_file_gen[k] = g_gen;
    return h;
}

/* Which handle and which sector in it serve a block device sector. */
static int _route(uint64_t sector, int32_t *handle, uint64_t *local)
{
    unsigned int k;

    if (udpfs_game_files.count == 0) {
        *handle = BLOCK_DEVICE_HANDLE;
        *local = sector;
        return 0;
    }

    k = (unsigned int)(sector >> UDPFS_FILE_SHIFT);
    if (k >= udpfs_game_files.count || k >= UDPFS_FILES_MAX)
        return -EINVAL;
    *handle = _file_handle(k);
    *local = sector & ((1ULL << UDPFS_FILE_SHIFT) - 1);
    return *handle < 0 ? -EIO : 0;
}
#else
static int _route(uint64_t sector, int32_t *handle, uint64_t *local)
{
    *handle = BLOCK_DEVICE_HANDLE;
    *local = sector;
    return 0;
}
#endif


/*
 * Block device read (retry + chunking handled by udpfs_bread)
 *
 * In the game a read that fails waits for the watcher to find the server
 * again and is then retried: the game pauses rather than dies, and the
 * device stays registered (OPL's cdvdman deadlocks on the bdm_disconnect_bd
 * upstream does from in here). In the menu the error goes back to BDM.
 */
static int udpfs_bd_read(struct block_device *bd, uint64_t sector, void *buffer, uint16_t count)
{
    int32_t handle;
    uint64_t local;
    int ret;

    if (sector >= bd->sectorCount)
        return -EINVAL;

    if ((sector + count) > bd->sectorCount)
        count = bd->sectorCount - sector;

    for (;;) {
        udpfs_lock();
        if (!udpfs_core_is_connected())
            ret = -EIO;
        else if ((ret = _route(sector, &handle, &local)) == 0)
            ret = udpfs_core_bread(handle, local, buffer, count, bd->sectorSize);
        udpfs_unlock();

        if (ret >= 0)
            return count;
        if (ret == -EINVAL)
            return ret;

#ifdef UDPFS_MENU
        M_DEBUG("udpfs_bd: read failed\n");
        return ret;
#else
        DelayThread(RETRY_WAIT_US);
#endif
    }
}


/*
 * Write sectors via shared block I/O helper (zero-copy)
 */
static int udpfs_bd_write(struct block_device *bd, uint64_t sector, const void *buffer, uint16_t count)
{
    int32_t handle;
    uint64_t local;
    int ret;

    M_DEBUG("udpfs_bd: write sector=%u, count=%u\n", (uint32_t)sector, count);

    if (sector >= bd->sectorCount)
        return -EINVAL;

    if ((sector + count) > bd->sectorCount)
        count = bd->sectorCount - sector;

    for (;;) {
        udpfs_lock();
        if (!udpfs_core_is_connected())
            ret = -EIO;
        else if ((ret = _route(sector, &handle, &local)) == 0)
            ret = udpfs_core_bwrite(handle, local, buffer, count, bd->sectorSize);
        udpfs_unlock();

        if (ret >= 0)
            return count;
        if (ret == -EINVAL)
            return ret;

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
 * Register the block device with BDM: `sectors` sectors of 512 bytes.
 * size_tag is what the menu's ping compares against (the image size).
 */
static void _register(uint32_t size_tag, uint64_t sectors)
{
    g_udpbd.name = "udp";
    g_udpbd.path = "udpbd";
    g_udpbd.devNr = 0;
    g_udpbd.parNr = 0;
    g_udpbd.sectorOffset = 0;
    g_udpbd.sectorSize = 512;
    g_udpbd.sectorCount = sectors;
    g_udpbd.priv = NULL;
    g_udpbd.read = udpfs_bd_read;
    g_udpbd.write = udpfs_bd_write;
    g_udpbd.flush = udpfs_bd_flush;
    g_udpbd.stop = udpfs_bd_stop;

    g_sectors = size_tag;
    g_registered = 1;
    bdm_connect_bd(&g_udpbd);
}

/*
 * The server's disk image in sectors: > 0, 0 when it has none, < 0 when
 * the request failed. Called with g_io held.
 *
 * Upstream asks GETSTAT with an empty path and takes the size. A server
 * that shares only a folder (-d) answers that with its root directory,
 * so a directory means no image here.
 */
static int _disk_sectors(void)
{
    iox_stat_t st;
    uint64_t size;
    int ret;

    ret = udpfs_core_getstat("", &st);
    if (!udpfs_core_is_connected())
        return -1;
    if (ret < 0 || (st.mode & FIO_S_IFDIR_UDPFS) != 0)
        return 0;
    size = ((uint64_t)st.hisize << 32) | st.size;
    size /= 512;
    if (size == 0)
        return 0;
    return size > 0x7FFFFFFF ? 0x7FFFFFFF : (int)size;
}

/*
 * Connect: discovery, then the block device if there is one. Called with
 * g_io held. Returns 0 when connected.
 *
 * In the menu a server without a -b image (folder only) is still a
 * connection, for the "udpfs:" device; there is just no block device.
 */
static int _connect(void)
{
    int n;

    if (udpfs_core_init() != 0)
        return -1;
    g_gen++;
    g_up = 1;

    if (g_registered)
        return 0;

#ifndef UDPFS_MENU
    if (udpfs_game_files.count > 0) {
        _register(0, (uint64_t)udpfs_game_files.count << UDPFS_FILE_SHIFT);
        M_PRINTF("server found, %u files\n", udpfs_game_files.count);
        return 0;
    }
#endif

    n = _disk_sectors();
    if (n > 0) {
        _register((uint32_t)n, (uint64_t)(uint32_t)n);
        M_PRINTF("server found, disk of %u sectors\n", (uint32_t)n);
    } else {
        M_PRINTF("server found, no disk image (folder only)\n");
    }

#ifndef UDPFS_MENU
    /* A game needs its disk. */
    if (!g_registered) {
        udpfs_core_exit();
        g_up = 0;
        return -1;
    }
#endif
    return 0;
}


/*
 * The watcher thread: connects, keeps trying, and keeps the connection.
 */
static void _watch(void *arg)
{
    (void)arg;

    for (;;) {
        if (!g_up) {
            udpfs_lock();
            _connect();
            udpfs_unlock();
            if (!g_up)
                DelayThread(DISCOVER_PAUSE_US);
            continue;
        }

#ifdef UDPFS_MENU
        DelayThread(PING_PERIOD_US);

        /* Idle menu: ask for the disk size. A restarted server answers on
         * another port and with fresh sequence numbers, so this fails
         * against it; a server that now serves another image, or none, or
         * one where there was none, answers differently. */
        {
            int lost, n;

            udpfs_lock();
            n = udpfs_core_is_connected() ? _disk_sectors() : -1;
            if (!udpfs_core_is_connected())
                lost = 1;
            else if (g_registered)
                lost = n < 0 || (uint32_t)n != g_sectors;
            else
                lost = n > 0;
            if (lost) {
                udpfs_core_exit();
                g_up = 0;
            }
            udpfs_unlock();

            if (lost) {
                M_PRINTF("server lost or changed, looking again\n");
                if (g_registered) {
                    g_registered = 0;
                    bdm_disconnect_bd(&g_udpbd);
                }
            }
        }
#else
        DelayThread(WATCH_PERIOD_US);

        /* In a game the reads themselves notice a loss. Keep the block
         * device and only find the server again; the waiting read then
         * goes through, on handles opened anew. */
        udpfs_lock();
        if (!udpfs_core_is_connected()) {
            M_PRINTF("server lost, looking again\n");
            udpfs_core_exit();
            g_up = 0;
            while (_connect() != 0)
                DelayThread(RETRY_WAIT_US);
            M_PRINTF("server back\n");
        }
        udpfs_unlock();
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
    {
        int i;

        for (i = 0; i < UDPFS_FILES_MAX; i++)
            g_file_handle[i] = -1;
    }

    /* The game's first disc read follows this module's load. Connect
     * before returning when the server is there, as upstream does; the
     * watcher keeps trying otherwise, and cdvdman waits for the disk. */
    _connect();
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
