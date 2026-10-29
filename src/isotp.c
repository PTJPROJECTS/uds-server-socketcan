#include "isotp.h"
#include "timeutil.h"

#include <stdio.h>
#include <string.h>

void isotp_init(isotp_link_t *l, can_if_t *cif, uint32_t rx_id, uint32_t tx_id,
                uint32_t func_id)
{
    memset(l, 0, sizeof(*l));
    l->cif = cif;
    l->rx_id = rx_id;
    l->tx_id = tx_id;
    l->func_id = func_id;
    l->bs = 0;          /* 0 = send everything, no more FC */
    l->stmin = 0;
    l->pad = 0xCC;
    l->rx_state = ISOTP_RX_IDLE;
}

uint32_t isotp_stmin_to_us(uint8_t stmin)
{
    if (stmin <= 0x7F)
        return (uint32_t)stmin * 1000u;
    if (stmin >= 0xF1 && stmin <= 0xF9)
        return (uint32_t)(stmin - 0xF0) * 100u;
    /* reserved values -> use the max (127 ms), per the spec */
    return 127000u;
}

static void fill_frame(isotp_link_t *l, can_msg_t *m)
{
    m->id = l->tx_id;
    m->dlc = 8;
    memset(m->data, l->pad, sizeof(m->data));
}

static int send_fc(isotp_link_t *l, uint8_t fs)
{
    can_msg_t m;

    fill_frame(l, &m);
    m.data[0] = (uint8_t)((ISOTP_PCI_FC << 4) | (fs & 0x0F));
    m.data[1] = l->bs;
    m.data[2] = l->stmin;
    return can_if_send(l->cif, &m);
}

int isotp_on_frame(isotp_link_t *l, const can_msg_t *m, uint8_t *out,
                   size_t out_size, int *functional)
{
    uint8_t pci;
    size_t n;

    *functional = 0;
    if (m->dlc < 1)
        return 0;

    pci = m->data[0] >> 4;

    /* functional addressing: single frames only */
    if (l->func_id && m->id == l->func_id) {
        if (pci != ISOTP_PCI_SF)
            return 0;
        n = m->data[0] & 0x0F;
        if (n == 0 || n > 7 || n + 1 > m->dlc || n > out_size)
            return 0;
        memcpy(out, &m->data[1], n);
        *functional = 1;
        return (int)n;
    }

    if (m->id != l->rx_id)
        return 0;

    switch (pci) {
    case ISOTP_PCI_SF:
        n = m->data[0] & 0x0F;
        if (n == 0 || n > 7 || n + 1 > m->dlc)
            return 0;
        if (l->rx_state != ISOTP_RX_IDLE)
            fprintf(stderr, "isotp: SF while receiving, dropping old msg\n");
        l->rx_state = ISOTP_RX_IDLE;
        if (n > out_size)
            return -1;
        memcpy(out, &m->data[1], n);
        return (int)n;

    case ISOTP_PCI_FF:
        if (m->dlc < 8)
            return 0;
        n = ((size_t)(m->data[0] & 0x0F) << 8) | m->data[1];
        if (n < 8)          /* would fit in a SF, invalid FF */
            return 0;
        if (n > sizeof(l->rx_buf) || n > out_size) {
            send_fc(l, ISOTP_FC_OVFLW);
            l->rx_state = ISOTP_RX_IDLE;
            return -1;
        }
        l->rx_len = (uint16_t)n;
        memcpy(l->rx_buf, &m->data[2], 6);
        l->rx_pos = 6;
        l->rx_sn = 1;
        l->rx_bs_cnt = 0;
        l->rx_state = ISOTP_RX_WAIT_CF;
        l->rx_deadline = now_ms() + ISOTP_N_CR_MS;
        send_fc(l, ISOTP_FC_CTS);
        return 0;

    case ISOTP_PCI_CF:
        if (l->rx_state != ISOTP_RX_WAIT_CF)
            return 0;       /* not expecting one, ignore */
        if ((m->data[0] & 0x0F) != l->rx_sn) {
            fprintf(stderr, "isotp: wrong SN, got %u want %u\n",
                    m->data[0] & 0x0F, l->rx_sn);
            l->rx_state = ISOTP_RX_IDLE;
            return -1;
        }
        l->rx_sn = (l->rx_sn + 1) & 0x0F;

        n = (size_t)(l->rx_len - l->rx_pos);
        if (n > 7)
            n = 7;
        if (n + 1 > m->dlc) {
            l->rx_state = ISOTP_RX_IDLE;
            return -1;
        }
        memcpy(&l->rx_buf[l->rx_pos], &m->data[1], n);
        l->rx_pos = (uint16_t)(l->rx_pos + n);
        l->rx_deadline = now_ms() + ISOTP_N_CR_MS;

        if (l->rx_pos >= l->rx_len) {
            l->rx_state = ISOTP_RX_IDLE;
            memcpy(out, l->rx_buf, l->rx_len);
            return l->rx_len;
        }

        if (l->bs) {
            l->rx_bs_cnt++;
            if (l->rx_bs_cnt >= l->bs) {
                l->rx_bs_cnt = 0;
                send_fc(l, ISOTP_FC_CTS);
            }
        }
        return 0;

    case ISOTP_PCI_FC:
        /* FC outside of a tx is just noise for us */
        return 0;

    default:
        return 0;
    }
}

void isotp_poll(isotp_link_t *l, uint64_t now)
{
    if (l->rx_state == ISOTP_RX_WAIT_CF && now > l->rx_deadline) {
        fprintf(stderr, "isotp: N_Cr timeout, got %u/%u bytes\n",
                l->rx_pos, l->rx_len);
        l->rx_state = ISOTP_RX_IDLE;
    }
}

/* wait for a FC frame. returns FS, fills bs/stmin, -1 on timeout */
static int wait_fc(isotp_link_t *l, uint8_t *bs, uint8_t *stmin)
{
    uint64_t deadline = now_ms() + ISOTP_N_BS_MS;
    can_msg_t m;

    for (;;) {
        uint64_t t = now_ms();
        int rc;

        if (t >= deadline)
            return -1;
        rc = can_if_recv(l->cif, &m, (int)(deadline - t));
        if (rc < 0)
            return -1;
        if (rc == 0)
            continue;
        if (m.id != l->rx_id || m.dlc < 3)
            continue;
        if ((m.data[0] >> 4) != ISOTP_PCI_FC)
            continue;

        *bs = m.data[1];
        *stmin = m.data[2];
        return m.data[0] & 0x0F;
    }
}

int isotp_send(isotp_link_t *l, const uint8_t *data, size_t len)
{
    can_msg_t m;
    size_t pos;
    uint8_t sn = 1;
    uint8_t bs = 0, stmin = 0;
    uint8_t blk = 0;
    int wft = 0;

    if (len == 0 || len > ISOTP_MAX_PAYLOAD)
        return -1;

    if (len <= 7) {
        fill_frame(l, &m);
        m.data[0] = (uint8_t)len;
        memcpy(&m.data[1], data, len);
        return can_if_send(l->cif, &m);
    }

    fill_frame(l, &m);
    m.data[0] = (uint8_t)((ISOTP_PCI_FF << 4) | ((len >> 8) & 0x0F));
    m.data[1] = (uint8_t)(len & 0xFF);
    memcpy(&m.data[2], data, 6);
    if (can_if_send(l->cif, &m) < 0)
        return -1;
    pos = 6;

    for (;;) {
        int fs = wait_fc(l, &bs, &stmin);

        if (fs < 0) {
            fprintf(stderr, "isotp: N_Bs timeout waiting for FC\n");
            return -1;
        }
        if (fs == ISOTP_FC_WAIT) {
            if (++wft > ISOTP_WFT_MAX) {
                fprintf(stderr, "isotp: too many FC.WAIT\n");
                return -1;
            }
            continue;
        }
        if (fs == ISOTP_FC_OVFLW) {
            fprintf(stderr, "isotp: receiver overflow\n");
            return -1;
        }
        if (fs != ISOTP_FC_CTS)
            return -1;

        wft = 0;
        blk = 0;
        while (pos < len) {
            size_t n = len - pos;

            if (n > 7)
                n = 7;
            fill_frame(l, &m);
            m.data[0] = (uint8_t)((ISOTP_PCI_CF << 4) | sn);
            memcpy(&m.data[1], &data[pos], n);
            if (can_if_send(l->cif, &m) < 0)
                return -1;
            pos += n;
            sn = (sn + 1) & 0x0F;

            if (pos >= len)
                return 0;
            if (bs && ++blk >= bs)
                break;      /* wait for next FC */
            sleep_us(isotp_stmin_to_us(stmin));
        }
    }
}
