#ifndef ISOTP_H
#define ISOTP_H

#include <stddef.h>
#include <stdint.h>
#include "can_if.h"

/*
 * Minimal ISO 15765-2 (ISO-TP) over classic CAN, normal addressing.
 * 12 bit length field only, so max payload is 4095 bytes.
 */

#define ISOTP_MAX_PAYLOAD 4095

/* PCI types, upper nibble of byte 0 */
#define ISOTP_PCI_SF 0x0
#define ISOTP_PCI_FF 0x1
#define ISOTP_PCI_CF 0x2
#define ISOTP_PCI_FC 0x3

#define ISOTP_FC_CTS   0
#define ISOTP_FC_WAIT  1
#define ISOTP_FC_OVFLW 2

/* timeouts in ms (spec defaults) */
#define ISOTP_N_BS_MS  1000
#define ISOTP_N_CR_MS  1000
#define ISOTP_WFT_MAX  10

typedef enum {
    ISOTP_RX_IDLE,
    ISOTP_RX_WAIT_CF,
} isotp_rx_state_t;

typedef struct {
    can_if_t *cif;
    uint32_t tx_id;     /* physical response id, e.g. 0x7E8 */
    uint32_t rx_id;     /* physical request id, e.g. 0x7E0 */
    uint32_t func_id;   /* functional request id, 0x7DF. 0 = disabled */

    uint8_t bs;         /* block size we put in our FC */
    uint8_t stmin;      /* stmin we put in our FC */
    uint8_t pad;        /* padding byte, frames are always DLC 8 */

    isotp_rx_state_t rx_state;
    uint8_t  rx_buf[ISOTP_MAX_PAYLOAD];
    uint16_t rx_len;
    uint16_t rx_pos;
    uint8_t  rx_sn;
    uint8_t  rx_bs_cnt;
    uint64_t rx_deadline;

    /*
     * Called while isotp_send() is blocked waiting for a FC, so the app
     * can keep doing periodic stuff (status frame). Optional.
     */
    void (*on_idle)(void *arg);
    void *idle_arg;
} isotp_link_t;

void isotp_init(isotp_link_t *l, can_if_t *cif, uint32_t rx_id, uint32_t tx_id,
                uint32_t func_id);

/*
 * Feed one received CAN frame into the rx state machine.
 * Returns payload length when a full message is ready (copied into out),
 * 0 if nothing complete yet, -1 on a protocol error (message dropped).
 * *functional is set to 1 when the request came in on func_id.
 */
int isotp_on_frame(isotp_link_t *l, const can_msg_t *m, uint8_t *out,
                   size_t out_size, int *functional);

/* Check N_Cr timeout. Call this from the main loop. */
void isotp_poll(isotp_link_t *l, uint64_t now);

/* Blocking send. Waits for flow control on multi-frame. 0 ok, -1 error. */
int isotp_send(isotp_link_t *l, const uint8_t *data, size_t len);

/* exposed for the tests / debugging */
uint32_t isotp_stmin_to_us(uint8_t stmin);

#endif
