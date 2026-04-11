/*
 * ce_proto.c — PID=CE native FlexNet protocol
 *
 * Record types (confirmed from xnet ARM binary + PC/FlexNet32 DLL analysis):
 *   '0' — initial handshake (upper SSID announcement)
 *   '1' — link time in milliseconds
 *   '2' — keepalive null frame (241 bytes)
 *   '3' — status frames ('3+\r', '3-\r') and compact routing records
 *   '4' — token/sequence exchange (with flag char)
 *   '6' — destination broadcast record (RTT + callsign + via)
 *
 * Sources:
 *   flxnod32.dll  — format strings: '0%c  %c\r', '1%d\r', '4%d%c\r', '6 %5u%s %s'
 *   FLXDECOD.DLL  — decoder: "Initial Handshake", "RTT-Ping/Pong", "Router Data"
 *   xnet_arm7     — fdest.c: '2%240s', '1%ld', '4%u', '6 %c%4d%a %a'
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "flexnetd.h"

/* ── CE link setup: SSID range announcement (5 bytes) ───────────────── */
/*
 * Frame format (from captures, pid=CE):
 *   byte 0: 0x30 + min_ssid
 *   byte 1: 0x30 + max_ssid
 *   byte 2: 0x25  (flags/capability)
 *   byte 3: 0x21  (flags/capability)
 *   byte 4: 0x0D  (CR terminator)
 *
 * Example: MinSSID=0 MaxSSID=14 -> 30 3E 25 21 0D
 * Xnet decodes: "Link setup - max SSID 14"
 */
#define CE_LINK_SETUP_LEN  5

int ce_build_link_setup(uint8_t *buf, int buflen, int min_ssid, int max_ssid)
{
    (void)min_ssid;  /* link setup only carries the upper SSID */

    if (buflen < CE_LINK_SETUP_LEN) {
        LOG_ERR("ce_build_link_setup: buffer too small (%d < %d)",
                buflen, CE_LINK_SETUP_LEN);
        return -1;
    }
    /*
     * byte 0: always 0x30 — init handshake marker ('0' prefix).
     *         MUST stay 0x30 or xnet misclassifies the frame.
     * byte 1: 0x30 + max_ssid — the upper SSID bound.
     *         FLXDECOD.DLL: "Initial Handshake. Upper SSID: %d"
     * byte 2-3: 0x25 0x21 — capability flags.
     * byte 4: 0x0D — CR terminator.
     */
    buf[0] = 0x30;
    buf[1] = (uint8_t)(0x30 + max_ssid);
    buf[2] = 0x25;
    buf[3] = 0x21;
    buf[4] = 0x0D;
    LOG_INF("ce_build_link_setup: max_ssid=%d -> %02X %02X %02X %02X %02X",
            max_ssid, buf[0], buf[1], buf[2], buf[3], buf[4]);
    return CE_LINK_SETUP_LEN;
}

/* ── CE keepalive: '2' + 240 spaces = 241 bytes ─────────────────────── */
/*
 * Wire format from xnet ARM binary (fdest.c): sprintf(buf, "2%240s", "")
 * Confirmed by live capture: xnet sends '2' followed by 240 space chars.
 * No trailing '10\r' — that is a separate CE status frame.
 */
int ce_build_keepalive(uint8_t *buf, int buflen)
{
    if (buflen < CE_KEEPALIVE_LEN) {
        LOG_ERR("ce_build_keepalive: buffer too small (%d < %d)",
                buflen, CE_KEEPALIVE_LEN);
        return -1;
    }
    buf[0] = '2';
    memset(buf + 1, ' ', 240);
    LOG_DBG("ce_build_keepalive: built %d bytes", CE_KEEPALIVE_LEN);
    return CE_KEEPALIVE_LEN;
}

/*
 * ce_build_record — build one compact routing record (legacy '3' prefix).
 * Format: '3' CALLSIGN SSID [?]RTT '\r'
 * Returns bytes written, -1 on error.
 */
int ce_build_record(uint8_t *buf, int buflen,
                    const char *callsign, int ssid, int rtt, int indirect)
{
    char tmp[64];
    int  len;

    if (indirect)
        len = snprintf(tmp, sizeof(tmp), "3%s%02d?%d\r", callsign, ssid, rtt);
    else
        len = snprintf(tmp, sizeof(tmp), "3%s%02d%d\r",  callsign, ssid, rtt);

    if (len < 0 || len >= (int)sizeof(tmp) || len >= buflen) {
        LOG_ERR("ce_build_record: overflow for %s-%d", callsign, ssid);
        return -1;
    }
    memcpy(buf, tmp, (size_t)len);
    LOG_DBG("ce_build_record: '%.*s'", len - 1, tmp);
    return len;
}

/* ── CE link time: '1' + decimal_ms + '\r' ─────────────────────────── */
int ce_build_link_time(uint8_t *buf, int buflen, long link_time_ms)
{
    char tmp[32];
    int len = snprintf(tmp, sizeof(tmp), "1%ld\r", link_time_ms);
    if (len < 0 || len >= (int)sizeof(tmp) || len >= buflen) {
        LOG_ERR("ce_build_link_time: overflow");
        return -1;
    }
    memcpy(buf, tmp, (size_t)len);
    LOG_DBG("ce_build_link_time: %ld ms", link_time_ms);
    return len;
}

/* ── CE token: '4' + decimal_value + flag_char + '\r' ──────────────── */
/*
 * Win32 PC/FlexNet format: '4%d%c\r'
 *   token_val = sequence number
 *   flag      = token handover flag character
 *               (space = normal, 'R' = request, etc.)
 */
int ce_build_token(uint8_t *buf, int buflen, int token_val, char flag)
{
    char tmp[32];
    int len = snprintf(tmp, sizeof(tmp), "4%d%c\r", token_val, flag);
    if (len < 0 || len >= (int)sizeof(tmp) || len >= buflen) {
        LOG_ERR("ce_build_token: overflow");
        return -1;
    }
    memcpy(buf, tmp, (size_t)len);
    LOG_DBG("ce_build_token: val=%d flag='%c'", token_val, flag);
    return len;
}

/* ── CE destination broadcast: '6 ' + 5-digit RTT + callsign_info ─── */
/*
 * Wire format from flxnod32.dll: '6 %5u%s %s'
 * Decoder display:  '%c%-6.6s%3d-%-2d%5d '
 *   flag_char + 6-char call + ssid_lo-ssid_hi + 5-digit RTT
 *
 * The '6 ' prefix starts the frame, then RTT (5 digits), then
 * destination callsign with SSID range, then via-callsign.
 */
int ce_build_dest_broadcast(uint8_t *buf, int buflen,
                            int rtt, const char *callsign,
                            const char *via_callsign)
{
    char tmp[64];
    int len;
    if (via_callsign && via_callsign[0])
        len = snprintf(tmp, sizeof(tmp), "6 %5u%s %s\r",
                       (unsigned)rtt, callsign, via_callsign);
    else
        len = snprintf(tmp, sizeof(tmp), "6 %5u%s\r",
                       (unsigned)rtt, callsign);

    if (len < 0 || len >= (int)sizeof(tmp) || len >= buflen) {
        LOG_ERR("ce_build_dest_broadcast: overflow for %s", callsign);
        return -1;
    }
    memcpy(buf, tmp, (size_t)len);
    LOG_DBG("ce_build_dest_broadcast: rtt=%d call=%s via=%s",
            rtt, callsign, via_callsign ? via_callsign : "(none)");
    return len;
}

/*
 * ce_parse_dest_broadcast — parse a CE type-6 destination broadcast record.
 *
 * Wire format (from flxnod32.dll + FLXDECOD.DLL):
 *   '6 ' + 5-digit RTT + callsign_with_ssid + ' ' + via_callsign + '\r'
 *
 * Decoder display format: '%c%-6.6s%3d-%-2d%5d '
 *   flag_char + 6-char call + ssid_lo - ssid_hi + 5-digit RTT
 *
 * Returns 0 on success, -1 on parse failure.
 */
int ce_parse_dest_broadcast(const uint8_t *data, int len,
                            int *rtt_out, char *callsign_out,
                            int *ssid_lo_out, int *ssid_hi_out,
                            char *via_callsign_out, char *flag_out)
{
    if (len < 4 || data[0] != '6') return -1;

    char payload[256];
    int plen = (len < 254) ? len : 254;
    memcpy(payload, data, (size_t)plen);
    payload[plen] = '\0';

    /* strip trailing \r \n */
    char *end = payload + strlen(payload) - 1;
    while (end > payload && (*end == '\r' || *end == '\n' || *end == ' '))
        *end-- = '\0';

    /* skip '6' and optional space */
    const char *p = payload + 1;
    while (*p == ' ') p++;

    /* optional flag character (non-digit before the RTT value) */
    char flag = ' ';
    if (*p && !isdigit((unsigned char)*p) && *p != '-') {
        flag = *p++;
    }
    if (flag_out) *flag_out = flag;

    /* RTT: up to 5 digits */
    char rtt_buf[8] = {0};
    int ri = 0;
    while (*p && isdigit((unsigned char)*p) && ri < 6)
        rtt_buf[ri++] = *p++;
    if (ri == 0) return -1;
    int rtt = atoi(rtt_buf);
    if (rtt_out) *rtt_out = rtt;

    /* callsign (may include -SSID) */
    while (*p == ' ') p++;
    char call[MAX_CALLSIGN_LEN] = {0};
    int ci = 0;
    while (*p && *p != ' ' && ci < MAX_CALLSIGN_LEN - 1)
        call[ci++] = *p++;
    call[ci] = '\0';
    if (ci == 0) return -1;

    /* parse SSID from callsign if present (e.g. "DB0AAT-5") */
    int ssid_lo = 0, ssid_hi = 0;
    char base[MAX_CALLSIGN_LEN] = {0};
    const char *dash = strrchr(call, '-');
    if (dash && isdigit((unsigned char)*(dash + 1))) {
        int blen = (int)(dash - call);
        if (blen > 0 && blen < MAX_CALLSIGN_LEN) {
            memcpy(base, call, (size_t)blen);
            base[blen] = '\0';
            ssid_lo = atoi(dash + 1);
            ssid_hi = ssid_lo;
        } else {
            snprintf(base, MAX_CALLSIGN_LEN, "%s", call);
        }
    } else {
        snprintf(base, MAX_CALLSIGN_LEN, "%s", call);
        ssid_hi = 15;   /* no SSID specified = full range assumed */
    }

    if (callsign_out) snprintf(callsign_out, MAX_CALLSIGN_LEN, "%s", base);
    if (ssid_lo_out)  *ssid_lo_out = ssid_lo;
    if (ssid_hi_out)  *ssid_hi_out = ssid_hi;

    /* optional via callsign */
    while (*p == ' ') p++;
    if (via_callsign_out) {
        if (*p)
            snprintf(via_callsign_out, MAX_CALLSIGN_LEN, "%s", p);
        else
            via_callsign_out[0] = '\0';
    }

    LOG_DBG("ce_parse_dest_broadcast: flag='%c' rtt=%d call=%s "
            "ssid=%d-%d via=%s",
            flag, rtt, base, ssid_lo, ssid_hi, (p && *p) ? p : "(none)");
    return 0;
}

/*
 * ce_parse_frame — classify and parse a CE frame.
 *
 * Returns (see CE_FRAME_* constants in flexnetd.h):
 *   CE_FRAME_KEEPALIVE(1)   keepalive
 *   CE_FRAME_STATUS_POS(2)  status '+' (positive)
 *   CE_FRAME_STATUS_NEG(3)  status '-' (negative / withdrawal)
 *   CE_FRAME_STATUS_10(4)   status '10\r'
 *   CE_FRAME_COMPACT(5)     compact record ('3' prefix)
 *   CE_FRAME_LINK_TIME(6)   link time ('1' prefix)
 *   CE_FRAME_TOKEN(7)       token/sequence ('4' prefix)
 *   CE_FRAME_DEST_BCAST(8)  destination broadcast ('6' prefix)
 *   CE_FRAME_INIT(9)        initial handshake ('0' prefix)
 *  -1  unrecognised
 */
int ce_parse_frame(const uint8_t *data, int len,
                   char *callsign_out, int *ssid_out, int *rtt_out)
{
    if (len <= 0) return -1;

    /* keepalive: 241 bytes starting with '2' */
    if (len == CE_KEEPALIVE_LEN && data[0] == '2') {
        LOG_DBG("ce_parse_frame: keepalive (%d bytes)", len);
        return CE_FRAME_KEEPALIVE;
    }

    /* tiny status frames (exactly 3 bytes) */
    if (len == 3) {
        if (data[0]=='3' && data[1]=='+' && data[2]=='\r') {
            LOG_DBG("ce_parse_frame: status '3+'");
            return CE_FRAME_STATUS_POS;
        }
        if (data[0]=='3' && data[1]=='-' && data[2]=='\r') {
            LOG_DBG("ce_parse_frame: status '3-'");
            return CE_FRAME_STATUS_NEG;
        }
        if (data[0]=='1' && data[1]=='0' && data[2]=='\r') {
            LOG_DBG("ce_parse_frame: status '10'");
            return CE_FRAME_STATUS_10;
        }
    }

    /* ── Init handshake: '0' prefix ────────────────────────────────── */
    /* Format from PC/FlexNet: '0%c  %c\r'
     * byte[1] = upper SSID encoded as 0x30+ssid
     * Confirmed by FLXDECOD.DLL: "Initial Handshake. Upper SSID: %d" */
    if (data[0] == '0' && len >= 2) {
        int upper_ssid = (int)(data[1]) - 0x30;
        if (upper_ssid < 0)  upper_ssid = 0;
        if (upper_ssid > 15) upper_ssid = 15;
        if (ssid_out) *ssid_out = upper_ssid;
        LOG_INF("ce_parse_frame: init handshake — upper SSID=%d", upper_ssid);
        return CE_FRAME_INIT;
    }

    /* ── Link time: '1' + decimal_ms + '\r' ────────────────────────── */
    /* Format: '1%d\r' — link time in milliseconds.
     * Must distinguish from '10\r' (already caught above as 3-byte). */
    if (data[0] == '1' && len > 3) {
        char tbuf[16] = {0};
        int ti = 0;
        for (int i = 1; i < len && i < 15; i++) {
            if (isdigit((unsigned char)data[i]))
                tbuf[ti++] = (char)data[i];
            else
                break;
        }
        if (ti > 0) {
            int link_time = atoi(tbuf);
            if (rtt_out) *rtt_out = link_time;
            LOG_INF("ce_parse_frame: link time = %d ms", link_time);
            return CE_FRAME_LINK_TIME;
        }
    }

    /* ── Token: '4' + decimal + flag_char + '\r' ───────────────────── */
    /* Format from PC/FlexNet: '4%d%c\r' */
    if (data[0] == '4' && len >= 3) {
        char vbuf[16] = {0};
        int vi = 0;
        int i = 1;
        while (i < len && isdigit((unsigned char)data[i]) && vi < 14)
            vbuf[vi++] = (char)data[i++];
        if (vi > 0) {
            int token_val = atoi(vbuf);
            char flag = (i < len && data[i] != '\r') ? (char)data[i] : ' ';
            if (rtt_out) *rtt_out = token_val;
            if (callsign_out) {
                callsign_out[0] = flag;
                callsign_out[1] = '\0';
            }
            LOG_INF("ce_parse_frame: token val=%d flag='%c'",
                    token_val, flag);
            return CE_FRAME_TOKEN;
        }
    }

    /* ── Destination broadcast: '6' prefix ─────────────────────────── */
    if (data[0] == '6') {
        LOG_DBG("ce_parse_frame: type-6 destination broadcast (len=%d)", len);
        return CE_FRAME_DEST_BCAST;
    }

    /* ── Compact record: '3' prefix ───────────────────────────────── */
    if (data[0] != '3') {
        LOG_DBG("ce_parse_frame: unknown (first=0x%02X)", data[0]);
        return -1;
    }

    /* For classification only — return COMPACT.
     * Use ce_parse_compact_records() for multi-entry parsing. */
    return CE_FRAME_COMPACT;
}

/*
 * ce_parse_compact_records — parse all entries from a CE type-3 frame.
 *
 * Wire format (confirmed from live captures, April 2026):
 *   '3' + records... + ['-'] + '\r'
 *
 * Each record:
 *   CALL(6 chars, space-padded) + SSID_LO(1 char) + SSID_HI(1 char)
 *   + RTT(1-5 digits) + ' '
 *
 * SSID encoding: char = 0x30 + ssid (0-15)
 *   '0'-'9' = SSID 0-9, ':' = 10, ';' = 11, '<' = 12,
 *   '=' = 13, '>' = 14, '?' = 15
 *
 * Trailing '-' before '\r' marks withdrawal (RTT = infinity for all).
 *
 * Returns number of entries parsed into out[], -1 on error.
 */
int ce_parse_compact_records(const uint8_t *data, int len,
                             DestEntry *out, int max_entries)
{
    if (len < 4 || data[0] != '3') return -1;

    /* work on a mutable copy, skip leading '3' */
    char payload[2048];
    int plen = len - 1;
    if (plen >= (int)sizeof(payload)) plen = (int)sizeof(payload) - 1;
    memcpy(payload, data + 1, (size_t)plen);
    payload[plen] = '\0';

    /* check for withdrawal: trailing '-\r' */
    int withdrawal = 0;
    char *end = payload + strlen(payload) - 1;
    while (end >= payload && (*end == '\r' || *end == '\n'))
        *end-- = '\0';
    if (end >= payload && *end == '-') {
        withdrawal = 1;
        *end-- = '\0';
    }
    /* also strip trailing spaces */
    while (end >= payload && *end == ' ')
        *end-- = '\0';

    const char *p = payload;
    int count = 0;

    while (*p && count < max_entries) {

        /* skip any spaces between records */
        while (*p == ' ') p++;
        if (!*p || *p == '\r' || *p == '\n') break;

        /* CALLSIGN: exactly 6 chars (may include trailing spaces) */
        if ((int)(strlen(p)) < 8) break;  /* need at least 6+2 chars */

        char call[MAX_CALLSIGN_LEN];
        int ci = 0;
        for (int i = 0; i < 6; i++) {
            if (p[i] != ' ' && p[i] != '\0')
                call[ci++] = p[i];
        }
        call[ci] = '\0';
        p += 6;

        if (ci == 0) break;

        /* SSID_LO: 1 char (0x30 + ssid) */
        int ssid_lo = (int)(*p) - 0x30;
        if (ssid_lo < 0)  ssid_lo = 0;
        if (ssid_lo > 15) ssid_lo = 15;
        p++;

        /* SSID_HI: 1 char (0x30 + ssid) */
        int ssid_hi = (int)(*p) - 0x30;
        if (ssid_hi < 0)  ssid_hi = 0;
        if (ssid_hi > 15) ssid_hi = 15;
        p++;

        /* RTT: variable-length digits */
        char rtt_buf[8] = {0};
        int ri = 0;
        while (*p && isdigit((unsigned char)*p) && ri < 6)
            rtt_buf[ri++] = *p++;
        int rtt = ri ? atoi(rtt_buf) : 0;
        if (withdrawal) rtt = RTT_INFINITY;

        /* build entry */
        memset(&out[count], 0, sizeof(DestEntry));
        snprintf(out[count].callsign, MAX_CALLSIGN_LEN, "%s", call);
        out[count].ssid_lo     = ssid_lo;
        out[count].ssid_hi     = ssid_hi;
        out[count].rtt         = rtt;
        out[count].is_infinity = (rtt >= RTT_INFINITY);

        LOG_DBG("ce_parse_compact[%d]: %-9s %2d-%2d  %s",
                count, call, ssid_lo, ssid_hi, rtt_str(rtt));
        count++;

        /* skip trailing space separator */
        while (*p == ' ') p++;
    }

    LOG_INF("ce_parse_compact_records: %d entries (withdrawal=%d)",
            count, withdrawal);
    return count;
}
