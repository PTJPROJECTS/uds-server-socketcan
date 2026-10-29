#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "can_if.h"
#include "isotp.h"
#include "timeutil.h"
#include "uds.h"

#define REQ_ID     0x7E0
#define RSP_ID     0x7E8
#define FUNC_ID    0x7DF
#define STATUS_ID  0x3A0   /* see dbc/ecu_status.dbc */

static volatile sig_atomic_t running = 1;

static void on_signal(int sig)
{
    (void)sig;
    running = 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-i ifname | -u local_port:remote_port] [options]\n"
            "  -i IF     SocketCAN interface (vcan0, can0, ...)\n"
            "  -u L:R    UDP loopback bus, listen on L, send to R\n"
            "  -s MS     S3 session timeout in ms (default %d)\n"
            "  -p MS     status frame period in ms, 0 = off (default 100)\n"
            "  -b BS     block size we send in our flow control (default 0)\n"
            "  -m STMIN  STmin we send in our flow control, raw byte (default 0)\n"
            "  -v        print every frame (candump style)\n",
            prog, UDS_S3_MS_DEFAULT);
}

struct status_ctx {
    can_if_t *cif;
    const uds_server_t *uds;
    uint32_t period_ms;
    uint64_t next;
    uint8_t alive;
};

static void status_tick(void *arg)
{
    struct status_ctx *st = arg;
    uint64_t now = now_ms();
    uint16_t cv;
    can_msg_t m;

    if (!st->period_ms || now < st->next)
        return;

    cv = uds_battery_cv(now);
    memset(&m, 0, sizeof(m));
    m.id = STATUS_ID;
    m.dlc = 8;
    m.data[0] = (uint8_t)(cv & 0xFF);   /* little endian, matches the DBC */
    m.data[1] = (uint8_t)(cv >> 8);
    m.data[2] = st->uds->session;
    m.data[3] = st->alive & 0x0F;
    m.data[4] = st->uds->reset_count;
    can_if_send(st->cif, &m);
    st->alive = (uint8_t)((st->alive + 1) & 0x0F);

    st->next += st->period_ms;
    if (st->next < now)     /* we fell behind, don't burst */
        st->next = now + st->period_ms;
}

int main(int argc, char **argv)
{
    can_if_t cif;
    isotp_link_t link;
    uds_server_t uds;
    const char *ifname = NULL;
    const char *udp = NULL;
    uint32_t s3_ms = UDS_S3_MS_DEFAULT;
    uint32_t period_ms = 100;
    int trace = 0;
    unsigned fc_bs = 0, fc_stmin = 0;
    int i, rc;
    struct status_ctx st;
    static uint8_t req[ISOTP_MAX_PAYLOAD];
    static uint8_t rsp[ISOTP_MAX_PAYLOAD];

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) {
            ifname = argv[++i];
        } else if (!strcmp(argv[i], "-u") && i + 1 < argc) {
            udp = argv[++i];
        } else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
            s3_ms = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            period_ms = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "-b") && i + 1 < argc) {
            fc_bs = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            fc_stmin = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "-v")) {
            trace = 1;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (ifname) {
        rc = can_if_open_socketcan(&cif, ifname);
    } else if (udp) {
        unsigned lp, rp;

        if (sscanf(udp, "%u:%u", &lp, &rp) != 2 || lp > 65535 || rp > 65535) {
            usage(argv[0]);
            return 1;
        }
        rc = can_if_open_udp(&cif, (uint16_t)lp, (uint16_t)rp);
    } else {
        usage(argv[0]);
        return 1;
    }
    if (rc < 0)
        return 1;
    cif.trace = trace;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    isotp_init(&link, &cif, REQ_ID, RSP_ID, FUNC_ID);
    link.bs = (uint8_t)fc_bs;
    link.stmin = (uint8_t)fc_stmin;
    link.on_idle = status_tick;
    link.idle_arg = &st;
    uds_init(&uds, s3_ms);

    printf("uds_server up on %s (req 0x%03X rsp 0x%03X func 0x%03X), S3=%u ms\n",
           cif.name, REQ_ID, RSP_ID, FUNC_ID, (unsigned)s3_ms);
    fflush(stdout);

    st.cif = &cif;
    st.uds = &uds;
    st.period_ms = period_ms;
    st.next = now_ms() + period_ms;
    st.alive = 0;

    while (running) {
        can_msg_t m;
        int functional = 0;
        uint64_t now = now_ms();
        int wait = 10;

        if (period_ms && st.next > now && st.next - now < (uint64_t)wait)
            wait = (int)(st.next - now);

        rc = can_if_recv(&cif, &m, wait);
        if (rc < 0)
            break;

        if (rc == 1) {
            int n = isotp_on_frame(&link, &m, req, sizeof(req), &functional);

            if (n > 0) {
                size_t rlen = uds_handle(&uds, req, (size_t)n, functional,
                                         rsp, sizeof(rsp), now_ms());
                if (rlen > 0 && isotp_send(&link, rsp, rlen) < 0)
                    fprintf(stderr, "isotp send failed\n");
                uds_do_reset(&uds);
            }
        }

        now = now_ms();
        isotp_poll(&link, now);
        uds_tick(&uds, now);

        status_tick(&st);
        fflush(stdout);
    }

    printf("bye\n");
    can_if_close(&cif);
    return 0;
}
