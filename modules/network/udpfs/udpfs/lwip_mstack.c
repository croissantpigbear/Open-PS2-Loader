/*
  lwip_mstack: Neutrino's ministack and SMAP receive interface, emulated
  on top of the OPL menu's lwIP stack.

  In a running game the UDPFS client talks to Neutrino's own SMAP driver
  through ministack, which is lean and keeps the network to itself. In
  the OPL menu the network already belongs to ps2sdk's SMAP driver and
  lwIP (SMB, the RetroAchievements menu items, the HTTP client), and two
  drivers cannot share one controller. So the menu build of the client
  keeps udprdma.c, udpfs_core.c and udpfs_bd.c exactly as Neutrino has
  them and swaps what is underneath:

    udp_bind()            a lwIP socket plus a receive thread
    udp_packet_init()     records destination address and port
    udp_packet_send_ll()  one lwip_sendto() of header tail + data
    smap_fifo_read()      a memcpy from the datagram just received

  udprdma's receive callback expects an Ethernet frame: it reads the
  sender's IP and UDP port from the headers and the payload at fixed
  frame offsets (0x2C, 0x30). The receive thread therefore lays each
  datagram out behind a synthetic Ethernet/IPv4/UDP header, so those
  offsets point at the same bytes they would on the wire.

  Speed is not the point here: the menu reads directory entries, game
  configs, art and the few sectors the RetroAchievements hash needs.
  The game itself runs on the in-game build.

  Licenced under Academic Free License version 3.0.
*/

#include <thbase.h>
#include <thsemap.h>
#include <sysclib.h>
#include <stdio.h>

#include "ministack_udp.h"
#include "smap.h"
#include "lwip_glue.h"

/* Ethernet 14 + IPv4 20 + UDP 8: where the UDP payload sits in a frame. */
#define FRAME_HDR 42
/* What ministack pre-reads for its callbacks: the headers plus 2 bytes. */
#define PREREAD   44
/* Largest datagram the server sends: 1408 data + 32 app header + 6 RDMA. */
#define RX_MAX    1536

#define MAX_SOCKETS 2

struct shim_socket
{
    udp_socket_t s; /* first: ministack hands this pointer around */
    int fd;
    int tid;
};

static struct shim_socket g_sock[MAX_SOCKETS];

/* The frame being handed to a callback. Only the receive thread writes
   it, and smap_fifo_read() is only legal inside that callback, so one
   buffer serves every socket. */
static uint8_t g_rx[FRAME_HDR + RX_MAX] __attribute__((aligned(4)));
static int g_rx_len = 0;

/* Sends come from the thread that called udprdma_send()/recv(); the
   receive callback only sets event flags. The semaphore keeps the shared
   transmit buffer whole should that ever change. */
static uint8_t g_tx[RX_MAX] __attribute__((aligned(4)));
static int g_tx_sema = -1;

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void rx_thread(void *arg)
{
    struct shim_socket *ss = (struct shim_socket *)arg;

    for (;;) {
        unsigned int ip = 0;
        unsigned short port = 0;
        int got, hdr_len;

        got = glue_udp_recv(ss->fd - 1, &g_rx[FRAME_HDR], RX_MAX, &ip, &port);
        if (got <= 0) {
            DelayThread(10 * 1000);
            continue;
        }

        /* Only the fields the callbacks read: EtherType, protocol,
           addresses and ports, UDP length. MACs stay zero. */
        memset(g_rx, 0, FRAME_HDR);
        put16(&g_rx[12], ETH_TYPE_IPV4);
        g_rx[14] = 0x45;
        put16(&g_rx[16], (uint16_t)(20 + 8 + got));
        g_rx[22] = 64;
        g_rx[23] = IP_PROTOCOL_UDP;
        put32(&g_rx[26], ip);
        put32(&g_rx[30], 0);
        put16(&g_rx[34], port);
        put16(&g_rx[36], ss->s.port_src);
        put16(&g_rx[38], (uint16_t)(8 + got));

        g_rx_len = FRAME_HDR + got;
        hdr_len = g_rx_len < PREREAD ? g_rx_len : PREREAD;

        if (ss->s.handler != NULL)
            ss->s.handler(&ss->s, ss->s.handler_arg, g_rx, (uint16_t)hdr_len);
    }
}

udp_socket_t *udp_bind(uint16_t port_src, udp_port_handler handler, void *handler_arg)
{
    iop_thread_t t;
    int i, fd;

    if (g_tx_sema < 0) {
        iop_sema_t sema;

        sema.attr = 0;
        sema.option = 0;
        sema.initial = 1;
        sema.max = 1;
        g_tx_sema = CreateSema(&sema);
    }

    /* udprdma_destroy() cannot unbind (ministack has no call for it), and
       the menu retries discovery until a server answers: a second bind of
       the same port takes over the existing socket. */
    for (i = 0; i < MAX_SOCKETS; i++) {
        if (g_sock[i].fd > 0 && g_sock[i].s.port_src == port_src) {
            g_sock[i].s.handler_arg = handler_arg;
            g_sock[i].s.handler = handler;
            return &g_sock[i].s;
        }
    }

    for (i = 0; i < MAX_SOCKETS; i++) {
        if (g_sock[i].fd <= 0)
            break;
    }
    if (i == MAX_SOCKETS)
        return NULL;

    fd = glue_udp_open(port_src);
    if (fd < 0) {
        printf("udpfs: cannot bind UDP %u\n", port_src);
        return NULL;
    }

    /* lwIP socket descriptors start at 0; fd holds descriptor + 1 so
       that 0 keeps meaning "free". */
    g_sock[i].fd = fd + 1;
    g_sock[i].s.port_src = port_src;
    g_sock[i].s.handler = handler;
    g_sock[i].s.handler_arg = handler_arg;

    t.attr = TH_C;
    t.option = 0;
    t.thread = rx_thread;
    t.stacksize = 0x800;
    /* Above the threads that wait on udprdma's event flags, so an ACK is
       seen as soon as lwIP has it. */
    t.priority = 0x30;
    g_sock[i].tid = CreateThread(&t);
    if (g_sock[i].tid < 0) {
        g_sock[i].fd = 0;
        return NULL;
    }

    StartThread(g_sock[i].tid, &g_sock[i]);

    return &g_sock[i].s;
}

void udp_packet_init(udp_packet_t *pkt, uint32_t ip_dst, uint16_t port_dst)
{
    memset(pkt, 0, sizeof(*pkt));
    pkt->ip.addr_dst.addr[0] = (uint8_t)(ip_dst >> 24);
    pkt->ip.addr_dst.addr[1] = (uint8_t)(ip_dst >> 16);
    pkt->ip.addr_dst.addr[2] = (uint8_t)(ip_dst >> 8);
    pkt->ip.addr_dst.addr[3] = (uint8_t)ip_dst;
    pkt->udp.port_dst = htons(port_dst);
}

int udp_packet_send_ll(udp_socket_t *socket, udp_packet_t *pkt, uint16_t pktdatasize, const void *data, uint16_t datasize)
{
    struct shim_socket *ss = (struct shim_socket *)socket;
    const uint8_t *a = pkt->ip.addr_dst.addr;
    uint32_t ip = ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) | ((uint32_t)a[2] << 8) | a[3];
    uint16_t port = ntohs(pkt->udp.port_dst);
    int total = pktdatasize + datasize, ret;

    if (ss == NULL || ss->fd <= 0 || total > (int)sizeof(g_tx))
        return -1;

    WaitSema(g_tx_sema);
    /* The payload starts right after the UDP header in the caller's
       packet, whatever follows it in the structure. */
    memcpy(g_tx, (const uint8_t *)pkt + FRAME_HDR, pktdatasize);
    if (datasize > 0)
        memcpy(&g_tx[pktdatasize], data, datasize);
    ret = glue_udp_send(ss->fd - 1, g_tx, total, ip, port);
    SignalSema(g_tx_sema);

    return ret < 0 ? -1 : 0;
}

void smap_fifo_read(uint16_t offset, void *dst, uint32_t bytes)
{
    uint8_t *d = (uint8_t *)dst;
    uint32_t have = 0;

    if (offset < g_rx_len)
        have = (uint32_t)(g_rx_len - offset);
    if (have > bytes)
        have = bytes;

    if (have > 0)
        memcpy(d, &g_rx[offset], have);
    /* Past the datagram the controller would hand back whatever sits in
       the FIFO; zeros are the honest equivalent. */
    if (have < bytes)
        memset(d + have, 0, bytes - have);
}
