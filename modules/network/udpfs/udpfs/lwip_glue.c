/*
  lwip_glue: the socket calls lwip_mstack.c needs, over the OPL menu's
  lwIP stack (ps2ip.irx).

  Kept in its own file because ps2ip.h drags in the whole lwIP header
  set, whose byte-order macros collide with the inline helpers in
  Neutrino's ministack headers. Nothing here knows about UDPRDMA.

  Addresses cross this interface in host byte order (a.b.c.d as
  a << 24 | ...), ports likewise, the way ministack passes them.

  Licenced under Academic Free License version 3.0.
*/

#include <ps2ip.h>
#include <sysclib.h>
#include "lwip_glue.h"

int glue_udp_open(unsigned short port)
{
    struct sockaddr_in me;
    int s, on = 1;

    s = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0)
        return -1;

    /* Discovery is a broadcast to 255.255.255.255. */
    lwip_setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET;
    me.sin_port = htons(port);
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    if (lwip_bind(s, (struct sockaddr *)&me, sizeof(me)) < 0) {
        lwip_close(s);
        return -1;
    }

    return s;
}

int glue_udp_recv(int s, void *buf, int len, unsigned int *ip, unsigned short *port)
{
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    int got;

    got = lwip_recvfrom(s, buf, len, 0, (struct sockaddr *)&from, &fromlen);
    if (got < 0)
        return got;

    *ip = ntohl(from.sin_addr.s_addr);
    *port = ntohs(from.sin_port);
    return got;
}

int glue_udp_send(int s, const void *buf, int len, unsigned int ip, unsigned short port)
{
    struct sockaddr_in to;

    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = htonl(ip);

    return lwip_sendto(s, (void *)buf, len, 0, (struct sockaddr *)&to, sizeof(to));
}
