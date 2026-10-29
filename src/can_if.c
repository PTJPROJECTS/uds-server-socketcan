#define _DEFAULT_SOURCE
#include "can_if.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/can.h>
#include <linux/can/raw.h>

static void trace_frame(const can_if_t *cif, const can_msg_t *m, char dir)
{
    int i;

    if (!cif->trace)
        return;
    /* same layout as candump so logs are easy to compare */
    printf("  %-6s %c %03X   [%u] ", cif->name, dir, (unsigned)m->id, m->dlc);
    for (i = 0; i < m->dlc; i++)
        printf(" %02X", m->data[i]);
    printf("\n");
    fflush(stdout);
}

int can_if_open_socketcan(can_if_t *cif, const char *ifname)
{
    struct sockaddr_can addr;
    struct ifreq ifr;

    memset(cif, 0, sizeof(*cif));
    cif->type = CAN_IF_SOCKETCAN;
    snprintf(cif->name, sizeof(cif->name), "%s", ifname);

    cif->fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (cif->fd < 0) {
        perror("socket(PF_CAN)");
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
    if (ioctl(cif->fd, SIOCGIFINDEX, &ifr) < 0) {
        perror("SIOCGIFINDEX");
        close(cif->fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(cif->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind(can)");
        close(cif->fd);
        return -1;
    }
    return 0;
}

/*
 * UDP "bus": each CAN frame goes out as one datagram holding a
 * struct can_frame (16 bytes), same as what you'd read from a raw
 * CAN socket. Point to point only, server <-> one tester.
 */
int can_if_open_udp(can_if_t *cif, uint16_t local_port, uint16_t remote_port)
{
    struct sockaddr_in addr;

    memset(cif, 0, sizeof(*cif));
    cif->type = CAN_IF_UDP;
    cif->udp_local_port = local_port;
    cif->udp_remote_port = remote_port;
    snprintf(cif->name, sizeof(cif->name), "udp%u", (unsigned)local_port);

    cif->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (cif->fd < 0) {
        perror("socket(udp)");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(local_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(cif->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind(udp)");
        close(cif->fd);
        return -1;
    }
    return 0;
}

void can_if_close(can_if_t *cif)
{
    if (cif->fd >= 0)
        close(cif->fd);
    cif->fd = -1;
}

int can_if_send(can_if_t *cif, const can_msg_t *msg)
{
    struct can_frame f;
    ssize_t n;

    memset(&f, 0, sizeof(f));
    f.can_id = msg->id & CAN_SFF_MASK;
    f.can_dlc = msg->dlc;
    memcpy(f.data, msg->data, msg->dlc);

    if (cif->type == CAN_IF_SOCKETCAN) {
        n = write(cif->fd, &f, sizeof(f));
    } else {
        struct sockaddr_in to;

        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        to.sin_port = htons(cif->udp_remote_port);
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        n = sendto(cif->fd, &f, sizeof(f), 0, (struct sockaddr *)&to, sizeof(to));
        /* nobody listening on the other side yet, that's fine */
        if (n < 0 && errno == ECONNREFUSED)
            n = sizeof(f);
    }

    if (n != (ssize_t)sizeof(f)) {
        perror("can_if_send");
        return -1;
    }
    trace_frame(cif, msg, 'T');
    return 0;
}

int can_if_recv(can_if_t *cif, can_msg_t *msg, int timeout_ms)
{
    struct pollfd pfd;
    struct can_frame f;
    ssize_t n;
    int rc;

    pfd.fd = cif->fd;
    pfd.events = POLLIN;

    rc = poll(&pfd, 1, timeout_ms);
    if (rc < 0) {
        if (errno == EINTR)
            return 0;
        perror("poll");
        return -1;
    }
    if (rc == 0)
        return 0;

    n = recv(cif->fd, &f, sizeof(f), 0);
    if (n < 0) {
        if (errno == ECONNREFUSED || errno == EINTR)
            return 0;
        perror("recv");
        return -1;
    }
    if (n != (ssize_t)sizeof(f))
        return 0;   /* junk, ignore */

    if (f.can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG))
        return 0;

    msg->id = f.can_id & CAN_SFF_MASK;
    msg->dlc = f.can_dlc > 8 ? 8 : f.can_dlc;
    memcpy(msg->data, f.data, msg->dlc);
    trace_frame(cif, msg, 'R');
    return 1;
}
