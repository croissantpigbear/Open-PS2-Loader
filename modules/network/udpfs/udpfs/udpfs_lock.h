#ifndef UDPFS_LOCK_H
#define UDPFS_LOCK_H

/* One connection to the server, shared by the block device (udpfs_bd.c)
   and, in the menu, the "udpfs:" file device (udpfs_fs.c). UDPRDMA has
   one request on the wire at a time, so every protocol call is made with
   the lock held. The generation changes with every new connection:
   server handles from an earlier one are dead. */
void udpfs_lock(void);
void udpfs_unlock(void);
unsigned int udpfs_generation(void);

#endif
