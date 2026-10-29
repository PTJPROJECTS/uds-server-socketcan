#ifndef UDS_H
#define UDS_H

#include <stddef.h>
#include <stdint.h>

/* services */
#define UDS_SID_DSC   0x10  /* DiagnosticSessionControl */
#define UDS_SID_ER    0x11  /* ECUReset */
#define UDS_SID_RDBI  0x22  /* ReadDataByIdentifier */
#define UDS_SID_TP    0x3E  /* TesterPresent */
#define UDS_NEG_RSP   0x7F
#define UDS_POS_OFFSET 0x40

#define UDS_SUPPRESS_POS_RSP 0x80

/* negative response codes */
#define NRC_SERVICE_NOT_SUPPORTED           0x11
#define NRC_SUBFUNCTION_NOT_SUPPORTED       0x12
#define NRC_INCORRECT_LENGTH                0x13
#define NRC_RESPONSE_TOO_LONG               0x14
#define NRC_CONDITIONS_NOT_CORRECT          0x22
#define NRC_REQUEST_OUT_OF_RANGE            0x31
#define NRC_SERVICE_NOT_SUPPORTED_IN_SESSION 0x7F

/* sessions */
#define UDS_SESSION_DEFAULT     0x01
#define UDS_SESSION_PROGRAMMING 0x02
#define UDS_SESSION_EXTENDED    0x03

/* timing we report in the 0x50 response */
#define UDS_P2_MS       50
#define UDS_P2_STAR_MS  5000
#define UDS_S3_MS_DEFAULT 5000

/* reset types */
#define UDS_RESET_HARD 0x01
#define UDS_RESET_SOFT 0x03

typedef struct {
    uint8_t  session;
    uint32_t s3_ms;
    uint64_t s3_deadline;

    uint8_t  reset_pending;   /* reset type to run after the response goes out */
    uint8_t  reset_count;
} uds_server_t;

void uds_init(uds_server_t *s, uint32_t s3_ms);

/*
 * Handle one request. Returns response length (0 = no response,
 * e.g. suppressPosRspMsgIndicationBit set or functional NRC suppressed).
 */
size_t uds_handle(uds_server_t *s, const uint8_t *req, size_t len, int functional,
                  uint8_t *rsp, size_t rsp_size, uint64_t now);

/* S3 timer check, drops back to default session on timeout */
void uds_tick(uds_server_t *s, uint64_t now);

/* Run a pending reset (call after the positive response is sent) */
void uds_do_reset(uds_server_t *s);

/* fake battery voltage in 0.01 V */
uint16_t uds_battery_cv(uint64_t now);

const char *uds_session_name(uint8_t session);

#endif
