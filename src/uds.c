#include "uds.h"

#include <stdio.h>
#include <string.h>

/* fake ECU identity */
static const char VIN[]       = "TESTVIN0000000042";   /* 17 chars */
static const char SW_VER[]    = "0.3.1";
static const char ECU_SERIAL[] = "TJP-ECU-000123";

#define DID_ACTIVE_SESSION 0xF186
#define DID_ECU_SERIAL     0xF18C
#define DID_VIN            0xF190
#define DID_SW_VERSION     0xF195
#define DID_BATTERY_VOLT   0x0101

void uds_init(uds_server_t *s, uint32_t s3_ms)
{
    memset(s, 0, sizeof(*s));
    s->session = UDS_SESSION_DEFAULT;
    s->s3_ms = s3_ms ? s3_ms : UDS_S3_MS_DEFAULT;
}

const char *uds_session_name(uint8_t session)
{
    switch (session) {
    case UDS_SESSION_DEFAULT:     return "default";
    case UDS_SESSION_PROGRAMMING: return "programming";
    case UDS_SESSION_EXTENDED:    return "extended";
    default:                      return "?";
    }
}

uint16_t uds_battery_cv(uint64_t now)
{
    /* 12.60 V with a slow +/-0.15 V triangle so it looks "live" */
    uint32_t t = (uint32_t)((now / 100) % 60);
    int32_t wobble = (t < 30) ? (int32_t)t - 15 : 45 - (int32_t)t;

    return (uint16_t)(1260 + wobble);
}

static size_t nrc(uint8_t *rsp, uint8_t sid, uint8_t code)
{
    rsp[0] = UDS_NEG_RSP;
    rsp[1] = sid;
    rsp[2] = code;
    return 3;
}

static size_t handle_dsc(uds_server_t *s, const uint8_t *req, size_t len,
                         uint8_t *rsp, uint64_t now)
{
    uint8_t sub;

    if (len != 2)
        return nrc(rsp, req[0], NRC_INCORRECT_LENGTH);

    sub = req[1] & 0x7F;
    if (sub != UDS_SESSION_DEFAULT && sub != UDS_SESSION_PROGRAMMING &&
        sub != UDS_SESSION_EXTENDED)
        return nrc(rsp, req[0], NRC_SUBFUNCTION_NOT_SUPPORTED);

    /* like most ECUs: programming only reachable from extended */
    if (sub == UDS_SESSION_PROGRAMMING && s->session == UDS_SESSION_DEFAULT)
        return nrc(rsp, req[0], NRC_CONDITIONS_NOT_CORRECT);

    if (s->session != sub)
        printf("uds: session %s -> %s\n", uds_session_name(s->session),
               uds_session_name(sub));
    s->session = sub;
    s->s3_deadline = now + s->s3_ms;

    rsp[0] = UDS_SID_DSC + UDS_POS_OFFSET;
    rsp[1] = sub;
    rsp[2] = (UDS_P2_MS >> 8) & 0xFF;
    rsp[3] = UDS_P2_MS & 0xFF;
    rsp[4] = ((UDS_P2_STAR_MS / 10) >> 8) & 0xFF;   /* P2* is in 10 ms units */
    rsp[5] = (UDS_P2_STAR_MS / 10) & 0xFF;
    return 6;
}

static size_t handle_ecu_reset(uds_server_t *s, const uint8_t *req, size_t len,
                               uint8_t *rsp)
{
    uint8_t sub;

    /* my choice: no resets from the default session */
    if (s->session == UDS_SESSION_DEFAULT)
        return nrc(rsp, req[0], NRC_SERVICE_NOT_SUPPORTED_IN_SESSION);

    if (len != 2)
        return nrc(rsp, req[0], NRC_INCORRECT_LENGTH);

    sub = req[1] & 0x7F;
    if (sub != UDS_RESET_HARD && sub != UDS_RESET_SOFT)
        return nrc(rsp, req[0], NRC_SUBFUNCTION_NOT_SUPPORTED);

    s->reset_pending = sub;
    rsp[0] = UDS_SID_ER + UDS_POS_OFFSET;
    rsp[1] = sub;
    return 2;
}

/* writes the DID record (without the DID itself), returns length or -1 */
static int read_did(const uds_server_t *s, uint16_t did, uint8_t *out,
                    size_t room, uint64_t now)
{
    size_t n;
    uint16_t v;

    switch (did) {
    case DID_VIN:
        n = strlen(VIN);
        if (n > room) return -2;
        memcpy(out, VIN, n);
        return (int)n;
    case DID_SW_VERSION:
        n = strlen(SW_VER);
        if (n > room) return -2;
        memcpy(out, SW_VER, n);
        return (int)n;
    case DID_ECU_SERIAL:
        n = strlen(ECU_SERIAL);
        if (n > room) return -2;
        memcpy(out, ECU_SERIAL, n);
        return (int)n;
    case DID_ACTIVE_SESSION:
        if (room < 1) return -2;
        out[0] = s->session;
        return 1;
    case DID_BATTERY_VOLT:
        if (room < 2) return -2;
        v = uds_battery_cv(now);
        out[0] = (uint8_t)(v >> 8);     /* big endian on the wire */
        out[1] = (uint8_t)(v & 0xFF);
        return 2;
    default:
        return -1;
    }
}

static size_t handle_rdbi(uds_server_t *s, const uint8_t *req, size_t len,
                          uint8_t *rsp, size_t rsp_size, uint64_t now)
{
    size_t i, pos = 1;
    int found = 0;

    if (len < 3 || ((len - 1) % 2) != 0)
        return nrc(rsp, req[0], NRC_INCORRECT_LENGTH);

    rsp[0] = UDS_SID_RDBI + UDS_POS_OFFSET;
    for (i = 1; i + 1 < len; i += 2) {
        uint16_t did = (uint16_t)((req[i] << 8) | req[i + 1]);
        int n;

        if (pos + 2 > rsp_size)
            return nrc(rsp, req[0], NRC_RESPONSE_TOO_LONG);
        n = read_did(s, did, &rsp[pos + 2], rsp_size - pos - 2, now);
        if (n == -2)
            return nrc(rsp, req[0], NRC_RESPONSE_TOO_LONG);
        if (n < 0)
            continue;   /* unknown DIDs are skipped, NRC only if none hit */
        rsp[pos] = req[i];
        rsp[pos + 1] = req[i + 1];
        pos += 2 + (size_t)n;
        found++;
    }

    if (!found)
        return nrc(rsp, req[0], NRC_REQUEST_OUT_OF_RANGE);
    return pos;
}

static size_t handle_tester_present(const uint8_t *req, size_t len, uint8_t *rsp)
{
    if (len != 2)
        return nrc(rsp, req[0], NRC_INCORRECT_LENGTH);
    if ((req[1] & 0x7F) != 0x00)
        return nrc(rsp, req[0], NRC_SUBFUNCTION_NOT_SUPPORTED);

    rsp[0] = UDS_SID_TP + UDS_POS_OFFSET;
    rsp[1] = 0x00;
    return 2;
}

static int has_subfunction(uint8_t sid)
{
    return sid == UDS_SID_DSC || sid == UDS_SID_ER || sid == UDS_SID_TP;
}

size_t uds_handle(uds_server_t *s, const uint8_t *req, size_t len, int functional,
                  uint8_t *rsp, size_t rsp_size, uint64_t now)
{
    size_t n;
    uint8_t sid;

    if (len == 0 || rsp_size < 8)
        return 0;

    sid = req[0];

    /* any request keeps a non-default session alive */
    if (s->session != UDS_SESSION_DEFAULT)
        s->s3_deadline = now + s->s3_ms;

    switch (sid) {
    case UDS_SID_DSC:
        n = handle_dsc(s, req, len, rsp, now);
        break;
    case UDS_SID_ER:
        n = handle_ecu_reset(s, req, len, rsp);
        break;
    case UDS_SID_RDBI:
        n = handle_rdbi(s, req, len, rsp, rsp_size, now);
        break;
    case UDS_SID_TP:
        n = handle_tester_present(req, len, rsp);
        break;
    default:
        n = nrc(rsp, sid, NRC_SERVICE_NOT_SUPPORTED);
        break;
    }

    if (rsp[0] == UDS_NEG_RSP) {
        /* ISO 14229-1: these NRCs are not sent for functional requests */
        if (functional && (rsp[2] == NRC_SERVICE_NOT_SUPPORTED ||
                           rsp[2] == NRC_SUBFUNCTION_NOT_SUPPORTED ||
                           rsp[2] == NRC_REQUEST_OUT_OF_RANGE ||
                           rsp[2] == 0x7E ||
                           rsp[2] == NRC_SERVICE_NOT_SUPPORTED_IN_SESSION))
            return 0;
        return n;
    }

    if (has_subfunction(sid) && len >= 2 && (req[1] & UDS_SUPPRESS_POS_RSP))
        return 0;

    return n;
}

void uds_tick(uds_server_t *s, uint64_t now)
{
    if (s->session != UDS_SESSION_DEFAULT && now >= s->s3_deadline) {
        printf("uds: S3 timeout, back to default session\n");
        s->session = UDS_SESSION_DEFAULT;
    }
}

void uds_do_reset(uds_server_t *s)
{
    if (!s->reset_pending)
        return;
    printf("uds: %s reset\n", s->reset_pending == UDS_RESET_HARD ? "hard" : "soft");
    s->reset_pending = 0;
    s->session = UDS_SESSION_DEFAULT;
    s->reset_count++;
}
