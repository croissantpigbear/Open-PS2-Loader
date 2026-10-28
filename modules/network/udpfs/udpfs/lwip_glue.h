#ifndef LWIP_GLUE_H
#define LWIP_GLUE_H

/* UDP over the menu's lwIP stack, addresses and ports in host order.
   See lwip_glue.c. */

/* Socket bound to `port` on every interface, broadcast allowed.
   Returns the socket or -1. */
int glue_udp_open(unsigned short port);

/* Blocks for one datagram. Returns its length, the sender in ip/port,
   or a negative value on error. */
int glue_udp_recv(int s, void *buf, int len, unsigned int *ip, unsigned short *port);

/* One datagram to ip:port. Returns bytes sent or a negative value. */
int glue_udp_send(int s, const void *buf, int len, unsigned int ip, unsigned short port);

#endif
