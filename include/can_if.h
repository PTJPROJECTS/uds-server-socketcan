#ifndef CAN_IF_H
#define CAN_IF_H

#include <stdint.h>

/*
 * Small wrapper so the rest of the code doesn't care if we are on a real
 * SocketCAN interface (vcan0, can0) or the UDP loopback "bus" I use when
 * the kernel has no CAN support (docker, WSL, CI runners...).
 */

#define CAN_IF_MAX_DLEN 8

typedef struct {
    uint32_t id;    /* 11-bit std id, no EFF for now */
    uint8_t  dlc;
    uint8_t  data[CAN_IF_MAX_DLEN];
} can_msg_t;

typedef enum {
    CAN_IF_SOCKETCAN,
    CAN_IF_UDP,
} can_if_type_t;

typedef struct {
    can_if_type_t type;
    int fd;
    /* udp only */
    uint16_t udp_local_port;
    uint16_t udp_remote_port;
    /* print every frame in candump style */
    int trace;
    char name[32];
} can_if_t;

int  can_if_open_socketcan(can_if_t *cif, const char *ifname);
int  can_if_open_udp(can_if_t *cif, uint16_t local_port, uint16_t remote_port);
void can_if_close(can_if_t *cif);

int can_if_send(can_if_t *cif, const can_msg_t *msg);

/* returns 1 if a frame was read, 0 on timeout, -1 on error */
int can_if_recv(can_if_t *cif, can_msg_t *msg, int timeout_ms);

#endif
