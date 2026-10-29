/*
  udpfs_fs: the "udpfs:" file device in the OPL menu, for UDPFS folder
  mode (udpfs_server.py -d).

  Based on Neutrino's udpfs_ioman.c, with three differences:

  - The connection belongs to the watcher in udpfs_bd.c. Every call takes
    its lock (udpfs_lock.h), and a file opened on an earlier connection
    answers -EIO instead of using a handle the new server never gave out.

  - OPL's BDM code asks a FAT driver for a file's fragments and LBA, to
    hand the game the sectors to read. Here every file the menu asks about
    gets a slot in a table (udpfs_files.h) and is described as one
    fragment at slot << UDPFS_FILE_SHIFT. Before a launch OPL reads the
    table with UDPFS_DEVCTL_FILES and gives it to the in-game module,
    which opens the same paths and serves those sectors from them.

  - The driver name OPL asks for is "udpfs", which puts the page on OPL's
    UDPFS folder slot (bdmsupport.c).

  Licenced under Academic Free License version 3.0.
*/

#include <errno.h>
#include <iomanX.h>
#include <io_common.h>
#include <stdio.h>
#include <string.h>
#include <usbhdfsd-common.h>

#include "main.h"
#include "udpfs_core.h"
#include "udpfs_files.h"
#include "udpfs_lock.h"


#define UDPFS_MAX_HANDLES 8

typedef struct
{
    int32_t server_handle; /* -1 = free */
    int is_dir;
    unsigned int gen; /* connection the handle belongs to */
    char path[UDPFS_FILE_PATH];
} udpfs_fd_t;

static udpfs_fd_t g_fds[UDPFS_MAX_HANDLES];

/* Files described to OPL as fragments, by slot. Filled in order; when
   full, the oldest slot is reused, which only matters for a launch that
   needs more than eight files at once (no OPL launch does). */
static struct udpfs_files g_table = {UDPFS_FILES_MAGIC, 0, 0, {{0}}};
static unsigned int g_table_next = 0;


static const char *_skip_slash(const char *p)
{
    while (*p == '/' || *p == '\\')
        p++;
    return p;
}

static int _alloc_fd(void)
{
    int i;
    for (i = 0; i < UDPFS_MAX_HANDLES; i++) {
        if (g_fds[i].server_handle < 0)
            return i;
    }
    return -1;
}

static void _free_fd(int idx)
{
    if (idx >= 0 && idx < UDPFS_MAX_HANDLES)
        g_fds[idx].server_handle = -1;
}

/* Called with the lock held. */
static int _validate_fd(iomanX_iop_file_t *f, int *fd_idx_out, int32_t *handle_out)
{
    int fd_idx = (int)(uintptr_t)f->privdata;

    if (fd_idx < 0 || fd_idx >= UDPFS_MAX_HANDLES)
        return -EBADF;
    if (g_fds[fd_idx].server_handle < 0)
        return -EBADF;
    if (!udpfs_core_is_connected() || g_fds[fd_idx].gen != udpfs_generation())
        return -EIO;
    *fd_idx_out = fd_idx;
    *handle_out = g_fds[fd_idx].server_handle;
    return 0;
}

/* Slot for a path, adding it when new. */
static unsigned int _table_slot(const char *path)
{
    unsigned int i;

    path = _skip_slash(path);
    for (i = 0; i < g_table.count; i++) {
        if (strncmp(g_table.path[i], path, UDPFS_FILE_PATH) == 0)
            return i;
    }

    if (g_table.count < UDPFS_FILES_MAX) {
        i = g_table.count++;
    } else {
        i = g_table_next;
        g_table_next = (g_table_next + 1) % UDPFS_FILES_MAX;
    }
    strncpy(g_table.path[i], path, UDPFS_FILE_PATH - 1);
    g_table.path[i][UDPFS_FILE_PATH - 1] = '\0';
    return i;
}


static int udpfs_init_dev(iomanX_iop_device_t *d)
{
    int i;

    (void)d;
    for (i = 0; i < UDPFS_MAX_HANDLES; i++)
        g_fds[i].server_handle = -1;
    return 0;
}

static int udpfs_deinit_dev(iomanX_iop_device_t *d)
{
    (void)d;
    return 0;
}

static int udpfs_format(iomanX_iop_file_t *f, const char *unk1, const char *unk2, void *unk3, int unk4)
{
    return -EIO;
}

static int _open(iomanX_iop_file_t *f, const char *name, int flags, int mode, int is_dir)
{
    int fd_idx, ret = -EIO;
    int32_t server_handle;

    udpfs_lock();
    fd_idx = _alloc_fd();
    if (fd_idx < 0) {
        udpfs_unlock();
        return -EMFILE;
    }

    if (udpfs_core_is_connected())
        ret = udpfs_core_open(name, flags, mode, is_dir, &server_handle);
    if (ret >= 0) {
        g_fds[fd_idx].server_handle = server_handle;
        g_fds[fd_idx].is_dir = is_dir;
        g_fds[fd_idx].gen = udpfs_generation();
        strncpy(g_fds[fd_idx].path, _skip_slash(name), UDPFS_FILE_PATH - 1);
        g_fds[fd_idx].path[UDPFS_FILE_PATH - 1] = '\0';
        f->privdata = (void *)(uintptr_t)fd_idx;
        ret = 0;
    }
    udpfs_unlock();

    return ret;
}

static int udpfs_open(iomanX_iop_file_t *f, const char *name, int flags, int mode)
{
    return _open(f, name, flags, mode, 0);
}

static int udpfs_close(iomanX_iop_file_t *f)
{
    int fd_idx = (int)(uintptr_t)f->privdata;

    if (fd_idx < 0 || fd_idx >= UDPFS_MAX_HANDLES || g_fds[fd_idx].server_handle < 0)
        return -EBADF;

    udpfs_lock();
    /* A handle of a dead connection is not the new server's to close. */
    if (udpfs_core_is_connected() && g_fds[fd_idx].gen == udpfs_generation())
        udpfs_core_close(g_fds[fd_idx].server_handle);
    _free_fd(fd_idx);
    udpfs_unlock();
    return 0;
}

static int udpfs_read(iomanX_iop_file_t *f, void *buffer, int size)
{
    int fd_idx, ret;
    int32_t h;

    udpfs_lock();
    if ((ret = _validate_fd(f, &fd_idx, &h)) == 0)
        ret = udpfs_core_read(h, buffer, size);
    udpfs_unlock();
    return ret;
}

static int udpfs_write(iomanX_iop_file_t *f, void *buffer, int size)
{
    int fd_idx, ret;
    int32_t h;

    udpfs_lock();
    if ((ret = _validate_fd(f, &fd_idx, &h)) == 0)
        ret = udpfs_core_write(h, buffer, size);
    udpfs_unlock();
    return ret;
}

static s64 udpfs_lseek64(iomanX_iop_file_t *f, s64 offset, int whence)
{
    int fd_idx, ret;
    int32_t h;
    s64 pos;

    udpfs_lock();
    if ((ret = _validate_fd(f, &fd_idx, &h)) == 0)
        pos = udpfs_core_lseek(h, offset, whence);
    else
        pos = ret;
    udpfs_unlock();
    return pos;
}

static int udpfs_lseek(iomanX_iop_file_t *f, int offset, int whence)
{
    return (int)udpfs_lseek64(f, (s64)offset, whence);
}

/* The file's size in 512-byte sectors, rounded up. Lock held. */
static int _file_sectors(const char *path, u32 *sectors)
{
    iox_stat_t st;
    u64 size;
    int ret;

    if ((ret = udpfs_core_getstat(path, &st)) < 0)
        return ret;
    size = ((u64)st.hisize << 32) | st.size;
    *sectors = (u32)((size + 511) / 512);
    return 0;
}

/* OPL's VMC check: the file's first cluster (any non-zero value) and
   whether its chain is contiguous (always, here). */
static int udpfs_ioctl(iomanX_iop_file_t *f, int cmd, void *data)
{
    (void)f;
    (void)data;

    switch (cmd) {
        case USBMASS_IOCTL_GET_CLUSTER:
            return 1;
        case USBMASS_IOCTL_CHECK_CHAIN:
            return 1;
        default:
            return -EIO;
    }
}

static int udpfs_remove(iomanX_iop_file_t *f, const char *name)
{
    int ret = -EIO;

    udpfs_lock();
    if (udpfs_core_is_connected())
        ret = udpfs_core_remove(name);
    udpfs_unlock();
    return ret;
}

static int udpfs_mkdir(iomanX_iop_file_t *f, const char *path, int mode)
{
    int ret = -EIO;

    udpfs_lock();
    if (udpfs_core_is_connected())
        ret = udpfs_core_mkdir(path, mode);
    udpfs_unlock();
    return ret;
}

static int udpfs_rmdir(iomanX_iop_file_t *f, const char *path)
{
    int ret = -EIO;

    udpfs_lock();
    if (udpfs_core_is_connected())
        ret = udpfs_core_rmdir(path);
    udpfs_unlock();
    return ret;
}

static int udpfs_dopen(iomanX_iop_file_t *f, const char *path)
{
    return _open(f, path, 0, 0, 1);
}

static int udpfs_dclose(iomanX_iop_file_t *f)
{
    return udpfs_close(f);
}

static int udpfs_dread(iomanX_iop_file_t *f, iox_dirent_t *dirent)
{
    int fd_idx, ret;
    int32_t h;
    iox_stat_t stat;
    static char name[256];

    udpfs_lock();
    if ((ret = _validate_fd(f, &fd_idx, &h)) == 0)
        ret = udpfs_core_dread(h, &stat, name, sizeof(name));
    if (ret > 0) {
        memset(dirent, 0, sizeof(iox_dirent_t));
        memcpy(&dirent->stat, &stat, sizeof(iox_stat_t));
        strncpy(dirent->name, name, 255);
        dirent->name[255] = '\0';
        ret = 1;
    }
    udpfs_unlock();
    return ret;
}

static int udpfs_getstat(iomanX_iop_file_t *f, const char *name, iox_stat_t *stat)
{
    int ret = -EIO;

    udpfs_lock();
    if (udpfs_core_is_connected())
        ret = udpfs_core_getstat(name, stat);
    udpfs_unlock();
    return ret;
}

static int udpfs_chstat(iomanX_iop_file_t *f, const char *name, iox_stat_t *stat, unsigned int mask)
{
    return -EIO;
}

static int udpfs_rename(iomanX_iop_file_t *f, const char *old, const char *new_name)
{
    return -EIO;
}

static int udpfs_chdir(iomanX_iop_file_t *f, const char *name)
{
    return -EIO;
}

static int udpfs_sync(iomanX_iop_file_t *f, const char *dev, int flag)
{
    return 0;
}

static int udpfs_mount(iomanX_iop_file_t *f, const char *fsname, const char *devname, int flag, void *arg, int arglen)
{
    return -EIO;
}

static int udpfs_umount(iomanX_iop_file_t *f, const char *fsname)
{
    return -EIO;
}

static int udpfs_devctl(iomanX_iop_file_t *f, const char *name, int cmd, void *arg, unsigned int arglen, void *buf, unsigned int buflen)
{
    switch (cmd) {
        case UDPFS_DEVCTL_FILES:
            if (buf == NULL || buflen < sizeof(g_table))
                return -EINVAL;
            memcpy(buf, &g_table, sizeof(g_table));
            return 0;
        case UDPFS_DEVCTL_STATE: {
            int state;

            udpfs_lock();
            state = udpfs_core_is_connected() ? (int)(udpfs_generation() & 0x7FFFFFFF) : 0;
            udpfs_unlock();
            return state;
        }
        case USBMASS_DEVCTL_STOP_ALL:
        case USBMASS_DEVCTL_STOP_UNIT:
            return 0;
        default:
            return -EIO;
    }
}

static int udpfs_symlink(iomanX_iop_file_t *f, const char *old, const char *new_name)
{
    return -EIO;
}

static int udpfs_readlink(iomanX_iop_file_t *f, const char *path, char *buf, unsigned int buflen)
{
    return -EIO;
}

/* The questions OPL's BDM code asks a FAT driver. */
static int udpfs_ioctl2(iomanX_iop_file_t *f, int cmd, void *data, unsigned int datalen, void *rdata, unsigned int rdatalen)
{
    int fd_idx = (int)(uintptr_t)f->privdata;

    switch (cmd) {
        case USBMASS_IOCTL_GET_DRIVERNAME:
            if (rdata == NULL || rdatalen < 6)
                return -EINVAL;
            strcpy(rdata, "udpfs");
            return 0;

        case USBMASS_IOCTL_GET_DEVICE_NUMBER:
            if (rdata == NULL || rdatalen < sizeof(int))
                return -EINVAL;
            *(int *)rdata = 0;
            return 0;

        case USBMASS_IOCTL_GET_LBA:
        case USBMASS_IOCTL_GET_FRAGLIST: {
            unsigned int slot;
            u32 sectors = 0;
            int ret;

            if (fd_idx < 0 || fd_idx >= UDPFS_MAX_HANDLES || g_fds[fd_idx].server_handle < 0 || g_fds[fd_idx].is_dir)
                return -EBADF;
            if (rdata == NULL)
                return -EINVAL;

            udpfs_lock();
            ret = udpfs_core_is_connected() ? _file_sectors(g_fds[fd_idx].path, &sectors) : -EIO;
            if (ret == 0)
                slot = _table_slot(g_fds[fd_idx].path);
            udpfs_unlock();
            if (ret < 0)
                return ret;

            if (cmd == USBMASS_IOCTL_GET_LBA) {
                if (rdatalen < sizeof(u64))
                    return -EINVAL;
                *(u64 *)rdata = (u64)slot << UDPFS_FILE_SHIFT;
                return 0;
            }

            if (rdatalen < sizeof(bd_fragment_t))
                return 0; /* no room: no fragments */
            {
                bd_fragment_t frag;

                frag.sector = (u64)slot << UDPFS_FILE_SHIFT;
                frag.count = sectors;
                memcpy(rdata, &frag, sizeof(frag)); /* packed, unaligned */
            }
            return 1;
        }

        case 0x80: /* Neutrino: the server handle behind the file */
            if (fd_idx >= 0 && fd_idx < UDPFS_MAX_HANDLES)
                return g_fds[fd_idx].server_handle;
            return -EBADF;

        default:
            return -EIO;
    }
}


static iomanX_iop_device_ops_t udpfs_device_ops = {
    udpfs_init_dev,
    udpfs_deinit_dev,
    udpfs_format,
    udpfs_open,
    udpfs_close,
    udpfs_read,
    udpfs_write,
    udpfs_lseek,
    udpfs_ioctl,
    udpfs_remove,
    udpfs_mkdir,
    udpfs_rmdir,
    udpfs_dopen,
    udpfs_dclose,
    udpfs_dread,
    udpfs_getstat,
    udpfs_chstat,
    /* Extended ops */
    udpfs_rename,
    udpfs_chdir,
    udpfs_sync,
    udpfs_mount,
    udpfs_umount,
    udpfs_lseek64,
    udpfs_devctl,
    udpfs_symlink,
    udpfs_readlink,
    udpfs_ioctl2};

static const char udpfs_name[] = "udpfs";
static iomanX_iop_device_t udpfs_device = {
    udpfs_name,
    IOP_DT_FSEXT | IOP_DT_FS,
    1,
    udpfs_name,
    &udpfs_device_ops};

int udpfs_fs_init(void)
{
    return AddDrv(&udpfs_device) < 0 ? -1 : 0;
}
