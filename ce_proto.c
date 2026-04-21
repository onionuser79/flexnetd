/*
 * ce_proto.c — PID=CE native FlexNet protocol
 *
 * Record types:
 *   '0' — initial handshake (upper SSID announcement)
 *   '1' — RTT-Pong / link time in milliseconds
 *   '2' — RTT-Ping / keepalive null frame (241 bytes)
 *   '3' — status frames ('3+\r', '3-\r') and compact routing records
 *   '4' — destination filter (Send only / Resend all with RTT threshold)
 *   '6' — Route/Traceroute REQUEST (path query)
 *   '7' — Route/Traceroute REPLY (accumulated hop list)
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
 * The peer decodes this as an initial handshake with upper SSID.
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
     *         MUST stay 0x30 or the peer misclassifies the frame.
     * byte 1: 0x30 + max_ssid — the upper SSID bound.
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

/* ── CE keepalive: '2' + 240 spaces = 241 bytes ──────────────────────── */
/*
 * Wire format (see PROTOCOL_SPEC.md §2.5):
 *   0x32 + 240 × 0x20  — pure '2' + spaces, NO trailer.
 *
 * (X)Net emits 241 bytes ('2' + 240 spaces); PCFlexnet emits 201 bytes
 * ('2' + 200 spaces).  Both variants are all-whitespace after the
 * leading '2'.
 *
 * HISTORY: Earlier versions of flexnetd (M6.8 through v0.7.1.2) emitted
 * '2' + 237 spaces + '10\r' based on an earlier spec revision that
 * misinterpreted a monitor capture — the '10\r' bytes are actually a
 * separate type-1 link-time frame sent right after the keepalive, not
 * part of it.  v0.7.2 aligns with the corrected spec.
 *
 * Keepalive period is 180 s per the spec; see DEFAULT_KEEPALIVE_S.
 */
int ce_build_keepalive(uint8_t *buf, int buflen)
{
    if (buflen < CE_KEEPALIVE_LEN) {
        LOG_ERR("ce_build_keepalive: buffer too small (%d < %d)",
                buflen, CE_KEEPALIVE_LEN);
        return -1;
    }
    buf[0] = '2';                           /* keepalive marker */
    memset(buf + 1, ' ', CE_KEEPALIVE_LEN - 1);  /* 240 spaces */
    LOG_DBG("ce_build_keepalive: built %d bytes ('2' + 240 spaces)",
            CE_KEEPALIVE_LEN);
    return CE_KEEPALIVE_LEN;
}

/*
 * ce_build_record — build one compact routing record.
 *
 * Wire format:
 *   '3' + CALLSIGN(6 chars, space-padded) + SSID_LO(1 char) + SSID_HI(1 char)
 *       + ['?'] + RTT(decimal digits) + ' ' + '\r'
 *
 * SSID encoding: char = 0x30 + ssid_value (0-15)
 *   '0'-'9' = SSID 0-9, ':' = 10, ';' = 11, '<' = 12,
 *   '=' = 13, '>' = 14, '?' = 15
 *
 * Returns bytes written, -1 on error.
 */
int ce_build_record(uint8_t *buf, int buflen,
                    const char *callsign, int ssid_lo, int ssid_hi,
                    int rtt, int indirect)
{
    /* clamp SSID values to valid range */
    if (ssid_lo < 0)  ssid_lo = 0;
    if (ssid_lo > 15) ssid_lo = 15;
    if (ssid_hi < 0)  ssid_hi = 0;
    if (ssid_hi > 15) ssid_hi = 15;

    char tmp[64];
    int  len;

    if (indirect)
        len = snprintf(tmp, sizeof(tmp), "3%-6.6s%c%c?%d \r",
                       callsign,
                       (char)(0x30 + ssid_lo),
                       (char)(0x30 + ssid_hi),
                       rtt);
    else
        len = snprintf(tmp, sizeof(tmp), "3%-6.6s%c%c%d \r",
                       callsign,
                       (char)(0x30 + ssid_lo),
                       (char)(0x30 + ssid_hi),
                       rtt);

    if (len < 0 || len >= (int)sizeof(tmp) || len >= buflen) {
        LOG_ERR("ce_build_record: overflow for %s %d-%d",
                callsign, ssid_lo, ssid_hi);
        return -1;
    }
    memcpy(buf, tmp, (size_t)len);
    LOG_DBG("ce_build_record: '%.*s' (ssid %d-%d, encoded 0x%02X 0x%02X)",
            len - 1, tmp, ssid_lo, ssid_hi,
            0x30 + ssid_lo, 0x30 + ssid_hi);
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

/* ── CE type-4 — routing-table sequence gossip ────────────────────── */
/*
 * Wire format (see PROTOCOL_SPEC.md §2.7):
 *   sprintf(buf, "4%u\r", seq);
 *
 * Purpose: cheap "routing table has changed" notification.  A node
 * maintains a 16-bit local sequence number that increments whenever
 * the routing table mutates.  On every per-port tick it compares the
 * local seq against the last value advertised to that peer and, if
 * different, emits a type-4 frame.  The peer just records the received
 * seq (no reply).  When a peer sees a seq higher than what it last
 * requested routes for, it knows to send '3+' to pull.
 *
 * flexnetd v0.7.2: TX fires from poll_cycle when our reachable
 * destination count changes between iterations; RX stores the peer
 * seq (for future use driving proactive '3+').
 *
 * The 'flag' argument of the legacy signature is retained for
 * source-compat but IGNORED on the wire (format has no flag).
 */
int ce_build_token(uint8_t *buf, int buflen, int token_val, char flag)
{
    (void)flag;   /* xnet format has no flag byte */
    char tmp[32];
    int len = snprintf(tmp, sizeof(tmp), "4%u\r", (unsigned)token_val);
    if (len < 0 || len >= (int)sizeof(tmp) || len >= buflen) {
        LOG_ERR("ce_build_token: overflow");
        return -1;
    }
    memcpy(buf, tmp, (size_t)len);
    LOG_DBG("ce_build_token: seq=%u", (unsigned)token_val);
    return len;
}

/* ── CE type-6 legacy builder (DEPRECATED) ─────────────────────────── */
/*
 * NOTE: The correct semantics of type-6 is "Route/Traceroute REQUEST"
 *       (first byte of payload = hop count + 0x20, followed by 5-digit
 *       QSO number, originator callsign, space, target callsign).
 *       This legacy builder treats the 5-digit field as RTT and the
 *       callsigns as dest+via.  It is retained only for historical
 *       reasons; M5.3 introduces the proper request/reply builders.
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
 * ce_parse_dest_broadcast — legacy parser (DEPRECATED, see M5.3).
 *
 * Historically interpreted as a destination broadcast with RTT + dest + via.
 * The actual type-6 semantics is a Route/Traceroute REQUEST; this parser is
 * kept for backward compatibility with fields that happen to line up.
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
 *   CE_FRAME_KEEPALIVE(1)    keepalive ('2' + N spaces)
 *   CE_FRAME_STATUS_POS(2)   status '3+\r' (request routes)
 *   CE_FRAME_STATUS_NEG(3)   status '3-\r' (end of batch)
 *   CE_FRAME_STATUS_10(4)    OBSOLETE — '10\r' now classified as link-time
 *   CE_FRAME_COMPACT(5)      compact record ('3' prefix with records)
 *   CE_FRAME_LINK_TIME(6)    link time ('1' prefix, any decimal value)
 *   CE_FRAME_TOKEN(7)        routing-seq gossip ('4' prefix) — xnet type-4
 *   CE_FRAME_DEST_BCAST(8)   legacy '6' dest broadcast (no longer emitted)
 *   CE_FRAME_INIT(9)         initial handshake ('0' prefix)
 *   CE_FRAME_PATH_REQUEST    type-6 Route/Traceroute request
 *   CE_FRAME_PATH_REPLY      type-7 accumulated hop list
 *  -1  unrecognised
 */
int ce_parse_frame(const uint8_t *data, int len,
                   char *callsign_out, int *ssid_out, int *rtt_out)
{
    if (len <= 0) return -1;

    /* keepalive: starts with '2', all remaining bytes are spaces.
     * xnet uses 241 bytes ('2' + 240 spaces); pcf uses 201 bytes
     * ('2' + 200 spaces).  Accept any length ≥ 2 whose body after the
     * '2' is pure whitespace — that cleanly handles both peers and
     * also guards against partial / fragmented deliveries. */
    if (data[0] == '2' && len >= 2) {
        int all_spaces = 1;
        for (int i = 1; i < len; i++) {
            if (data[i] != ' ') { all_spaces = 0; break; }
        }
        if (all_spaces) {
            LOG_DBG("ce_parse_frame: keepalive (%d bytes, all spaces)", len);
            return CE_FRAME_KEEPALIVE;
        }
    }

    /* tiny status frames (exactly 3 bytes) — routing tokens only.
     * Historical '10\r' classification removed in v0.7.2: per the
     * protocol spec, '10\r'/'11\r'/'12\r' are ordinary type-1
     * link-time frames with value 0/1/2 respectively, not a distinct
     * frame kind.  They fall through to the CE_FRAME_LINK_TIME path
     * below. */
    if (len == 3) {
        if (data[0]=='3' && data[1]=='+' && data[2]=='\r') {
            LOG_DBG("ce_parse_frame: status '3+'");
            return CE_FRAME_STATUS_POS;
        }
        if (data[0]=='3' && data[1]=='-' && data[2]=='\r') {
            LOG_DBG("ce_parse_frame: status '3-'");
            return CE_FRAME_STATUS_NEG;
        }
    }

    /* ── Init handshake: '0' prefix ────────────────────────────────── */
    /* Format: '0%c  %c\r'
     * byte[1] = upper SSID encoded as 0x30+ssid */
    if (data[0] == '0' && len >= 2) {
        int upper_ssid = (int)(data[1]) - 0x30;
        if (upper_ssid < 0)  upper_ssid = 0;
        if (upper_ssid > 15) upper_ssid = 15;
        if (ssid_out) *ssid_out = upper_ssid;
        LOG_INF("ce_parse_frame: init handshake — upper SSID=%d", upper_ssid);
        return CE_FRAME_INIT;
    }

    /* ── Link time: '1' + decimal + '\r' ──────────────────────────── */
    /* Format: '1%ld\r' (see PROTOCOL_SPEC.md §2.4).  Wire value is in
     * SECONDS (the sender's internal 10-ms tick count divided by 100).
     * Short frames like "10\r", "11\r", "12\r" and long ones like
     * "1600\r" are all the same kind — just different decimal values. */
    if (data[0] == '1' && len >= 3) {
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
            LOG_INF("ce_parse_frame: link time = %d s (wire)", link_time);
            return CE_FRAME_LINK_TIME;
        }
    }

    /* ── Type-4 routing-seq gossip: '4' + decimal + '\r' ──────────── */
    /* Per spec §2.7: RX just parses the decimal value and stores it
     * as the peer's current sequence — no flag byte, no reply. */
    if (data[0] == '4' && len >= 3) {
        char vbuf[16] = {0};
        int vi = 0;
        int i = 1;
        while (i < len && isdigit((unsigned char)data[i]) && vi < 14)
            vbuf[vi++] = (char)data[i++];
        if (vi > 0) {
            int token_val = atoi(vbuf);
            if (rtt_out) *rtt_out = token_val;
            if (callsign_out) callsign_out[0] = '\0';
            LOG_INF("ce_parse_frame: type-4 routing-seq=%u",
                    (unsigned)token_val);
            return CE_FRAME_TOKEN;
        }
    }

    /* ── Type-6 path REQUEST ───────────────────────────────────────── */
    if (data[0] == '6') {
        LOG_DBG("ce_parse_frame: type-6 path request (len=%d)", len);
        return CE_FRAME_PATH_REQUEST;
    }

    /* ── Type-7 path REPLY ─────────────────────────────────────────── */
    if (data[0] == '7') {
        LOG_DBG("ce_parse_frame: type-7 path reply (len=%d)", len);
        return CE_FRAME_PATH_REPLY;
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
 * port_idx: index into g_cfg.ports[] for the CE session that received
 * this frame.  Each entry is tagged with this index so the output
 * writer can render the correct "Via <port-neighbor>" for each route
 * (v0.7.1 fix — previously all records landed with port=0 and thus
 * rendered as "Via IW2OHX-14" regardless of actual source peer).
 * Pass -1 to preserve legacy behavior (entry.port left at 0 after
 * memset).
 *
 * Returns number of entries parsed into out[], -1 on error.
 */
int ce_parse_compact_records(const uint8_t *data, int len,
                             DestEntry *out, int max_entries,
                             int port_idx)
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
        /* v0.7.1: tag this entry with the port it arrived on so the
         * destinations file can show the correct Via neighbor */
        out[count].port        = (port_idx >= 0) ? port_idx : 0;

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

/* ── Path query protocol (CE type-6 / type-7) ─────────────────────── */

/*
 * ce_build_path_request — build a type-6 Route or Traceroute REQUEST.
 *
 * Wire:  '6' HOP(1) QSO(5) ORIGIN ' ' TARGET
 *
 * Initially emitted with HOP=0x20 (hop_count=0) — each forwarder
 * increments the byte before re-emitting.
 *
 * The QSO field is sprintf("%5u", qso) — right-aligned decimal in 5
 * bytes.  For Traceroute (kind == CE_PATH_KIND_TRACE), the caller wants
 * bit 0x40 set in the first byte of QSO; we OR it in after sprintf.
 * '%5u' produces ' ' (0x20) for small numbers, so the OR produces 0x60.
 */
int ce_build_path_request(uint8_t *buf, int buflen,
                          int qso, int kind,
                          const char *origin_call,
                          const char *target_call)
{
    if (!buf || !origin_call || !target_call)
        return -1;
    if (qso < 0) qso = 0;

    char qso_field[CE_PATH_QSO_FIELD_LEN + 1];
    snprintf(qso_field, sizeof(qso_field), "%5u", (unsigned)(qso % 100000));

    if (kind == CE_PATH_KIND_TRACE)
        qso_field[0] |= CE_PATH_TRACE_BIT;

    /* compose: '6' + hop_byte + qso(5) + origin + ' ' + target
     * hop_byte starts at 0x20 (hop count = 0) */
    int needed = 2 + CE_PATH_QSO_FIELD_LEN +
                 (int)strlen(origin_call) + 1 +
                 (int)strlen(target_call);
    if (needed > buflen) {
        LOG_ERR("ce_build_path_request: buffer too small (%d need %d)",
                buflen, needed);
        return -1;
    }

    int pos = 0;
    buf[pos++] = '6';
    buf[pos++] = CE_PATH_HOP_BYTE_BASE;  /* 0x20 — hop_count = 0 */
    memcpy(buf + pos, qso_field, CE_PATH_QSO_FIELD_LEN);
    pos += CE_PATH_QSO_FIELD_LEN;

    int ol = (int)strlen(origin_call);
    memcpy(buf + pos, origin_call, (size_t)ol);
    pos += ol;
    buf[pos++] = ' ';
    int tl = (int)strlen(target_call);
    memcpy(buf + pos, target_call, (size_t)tl);
    pos += tl;

    LOG_INF("ce_build_path_request: qso=%d kind=%s %s -> %s (%d bytes)",
            qso,
            kind == CE_PATH_KIND_TRACE ? "TRACE" : "ROUTE",
            origin_call, target_call, pos);
    return pos;
}

/*
 * ce_build_path_reply — build a type-7 REPLY with the accumulated hops.
 *
 * Wire:  '7' HOP(1) QSO(5) ' ' HOP_1 ' ' HOP_2 ... HOP_N
 *
 * HOP byte is derived from n_hops.
 */
int ce_build_path_reply(uint8_t *buf, int buflen,
                        int qso, int kind,
                        const char *const *hops, int n_hops)
{
    if (!buf || n_hops < 0) return -1;
    if (n_hops > CE_PATH_MAX_HOPS) n_hops = CE_PATH_MAX_HOPS;

    char qso_field[CE_PATH_QSO_FIELD_LEN + 1];
    snprintf(qso_field, sizeof(qso_field), "%5u", (unsigned)(qso % 100000));
    if (kind == CE_PATH_KIND_TRACE)
        qso_field[0] |= CE_PATH_TRACE_BIT;

    int pos = 0;

    /* prelude sanity check: 2 + 5 bytes */
    if (buflen < 2 + CE_PATH_QSO_FIELD_LEN) return -1;
    buf[pos++] = '7';
    buf[pos++] = (uint8_t)(CE_PATH_HOP_BYTE_BASE + n_hops);
    memcpy(buf + pos, qso_field, CE_PATH_QSO_FIELD_LEN);
    pos += CE_PATH_QSO_FIELD_LEN;

    /* Hops are space-separated AFTER the QSO field — but there is NO
     * separator between QSO and the first hop (the fixed 5-byte QSO
     * width tells the parser where to split).  E.g. captured reply:
     *   "7$    1IW2OHX-14 IR3UHU-2 IQ5KG-7 IR5S"
     *             ^^^^^^^ QSO(5)  ^ first hop (no leading space)
     */
    int first = 1;
    for (int i = 0; i < n_hops; i++) {
        const char *h = hops ? hops[i] : NULL;
        if (!h || !*h) continue;
        int hl = (int)strlen(h);
        int need = (first ? 0 : 1) + hl;
        if (pos + need >= buflen) {
            LOG_ERR("ce_build_path_reply: buffer overflow at hop %d", i);
            return -1;
        }
        if (!first) buf[pos++] = ' ';
        memcpy(buf + pos, h, (size_t)hl);
        pos += hl;
        first = 0;
    }

    LOG_INF("ce_build_path_reply: qso=%d kind=%s hops=%d (%d bytes)",
            qso, kind == CE_PATH_KIND_TRACE ? "TRACE" : "ROUTE",
            n_hops, pos);
    return pos;
}

/*
 * ce_parse_path_frame — parse a type-6 or type-7 frame.
 *
 * Depending on which, populates a subset of the output pointers.
 * Returns CE_FRAME_PATH_REQUEST, CE_FRAME_PATH_REPLY, or -1.
 *
 * Output conventions:
 *   for type-6 (request): origin_out, target_out, qso_out, kind_out,
 *                         hop_count_out populated; reply_out untouched.
 *   for type-7 (reply):   reply_out populated; origin_out/target_out
 *                         may be ignored (can be NULL).
 */
int ce_parse_path_frame(const uint8_t *data, int len,
                        PathReply *reply_out,
                        int *qso_out, int *kind_out, int *hop_count_out,
                        char *origin_out, char *target_out)
{
    if (!data || len < 2 + CE_PATH_QSO_FIELD_LEN) return -1;
    if (data[0] != '6' && data[0] != '7') return -1;

    int is_reply = (data[0] == '7');

    /* HOP_BYTE */
    int hop_byte = data[1];
    int hop_count = hop_byte - CE_PATH_HOP_BYTE_BASE;
    if (hop_count < 0) hop_count = 0;

    /* QSO_FIELD: 5 bytes, first byte has traceroute flag */
    char qso_field_raw[CE_PATH_QSO_FIELD_LEN + 1];
    memcpy(qso_field_raw, data + 2, CE_PATH_QSO_FIELD_LEN);
    qso_field_raw[CE_PATH_QSO_FIELD_LEN] = '\0';

    int kind = (qso_field_raw[0] & CE_PATH_TRACE_BIT)
               ? CE_PATH_KIND_TRACE : CE_PATH_KIND_ROUTE;

    /* mask the trace bit before parsing the number */
    char qso_field_masked[CE_PATH_QSO_FIELD_LEN + 1];
    memcpy(qso_field_masked, qso_field_raw, CE_PATH_QSO_FIELD_LEN + 1);
    qso_field_masked[0] = (char)(qso_field_masked[0] & ~CE_PATH_TRACE_BIT);

    /* atoi ignores leading spaces; but if we stripped 0x40 from a
     * space-padded first byte (0x20 | 0x40 = 0x60 '`'), we need to
     * convert that back to a space so atoi sees a valid number. */
    for (int i = 0; i < CE_PATH_QSO_FIELD_LEN; i++) {
        if (qso_field_masked[i] < '0' || qso_field_masked[i] > '9')
            qso_field_masked[i] = ' ';
    }
    int qso = atoi(qso_field_masked);

    if (qso_out)        *qso_out = qso;
    if (kind_out)       *kind_out = kind;
    if (hop_count_out)  *hop_count_out = hop_count;

    /* ── Type-6 request: origin + ' ' + target, no hop list ─────── */
    if (!is_reply) {
        int remain_off = 2 + CE_PATH_QSO_FIELD_LEN;
        /* copy remainder to a mutable buffer, strip CR/LF */
        char rem[256];
        int rlen = len - remain_off;
        if (rlen < 0) rlen = 0;
        if (rlen >= (int)sizeof(rem)) rlen = (int)sizeof(rem) - 1;
        memcpy(rem, data + remain_off, (size_t)rlen);
        rem[rlen] = '\0';
        char *nl = strpbrk(rem, "\r\n");
        if (nl) *nl = '\0';

        /* split on space: first token = origin, rest = target */
        char *sp = strchr(rem, ' ');
        if (!sp) {
            LOG_WRN("ce_parse_path_frame: type-6 missing target (%s)", rem);
            if (origin_out) snprintf(origin_out, MAX_CALLSIGN_LEN, "%s", rem);
            if (target_out) target_out[0] = '\0';
            return CE_FRAME_PATH_REQUEST;
        }
        *sp = '\0';
        /* strip trailing whitespace from target (be defensive) */
        char *tend = sp + strlen(sp + 1);
        while (tend > sp + 1 && (*tend == ' ' || *tend == '\t'))
            *tend-- = '\0';
        if (origin_out) snprintf(origin_out, MAX_CALLSIGN_LEN, "%s", rem);
        if (target_out) snprintf(target_out, MAX_CALLSIGN_LEN, "%s", sp + 1);
        LOG_INF("ce_parse_path_frame: type-6 qso=%d kind=%s hops=%d "
                "origin=%s target=%s",
                qso, kind == CE_PATH_KIND_TRACE ? "TRACE" : "ROUTE",
                hop_count, rem, sp + 1);
        return CE_FRAME_PATH_REQUEST;
    }

    /* ── Type-7 reply: accumulated hop list ─────────────────────── */
    if (!reply_out) return CE_FRAME_PATH_REPLY;

    memset(reply_out, 0, sizeof(*reply_out));
    reply_out->qso = qso;
    reply_out->kind = kind;
    reply_out->hop_count = hop_count;
    reply_out->received = time(NULL);

    int off = 2 + CE_PATH_QSO_FIELD_LEN;
    /* optional leading space(s) */
    while (off < len && data[off] == ' ') off++;

    /* copy remainder, strip CR/LF */
    char rem[512];
    int rlen = len - off;
    if (rlen < 0) rlen = 0;
    if (rlen >= (int)sizeof(rem)) rlen = (int)sizeof(rem) - 1;
    memcpy(rem, data + off, (size_t)rlen);
    rem[rlen] = '\0';
    char *nl = strpbrk(rem, "\r\n");
    if (nl) *nl = '\0';

    /* tokenize on space */
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(rem, " ", &save);
         tok && n < CE_PATH_MAX_HOPS;
         tok = strtok_r(NULL, " ", &save))
    {
        if (!*tok) continue;
        snprintf(reply_out->hop_callsigns[n], MAX_CALLSIGN_LEN, "%s", tok);
        n++;
    }
    reply_out->hop_callsign_count = n;

    LOG_INF("ce_parse_path_frame: type-7 qso=%d kind=%s hops=%d callsigns=%d",
            qso, kind == CE_PATH_KIND_TRACE ? "TRACE" : "ROUTE",
            hop_count, n);
    for (int i = 0; i < n; i++)
        LOG_DBG("  hop[%d] = %s", i, reply_out->hop_callsigns[i]);

    return CE_FRAME_PATH_REPLY;
}

/* ── Pending query tracking ────────────────────────────────────────── */

PathPending g_path_pending[CE_PATH_MAX_PENDING];
static int  g_path_qso_counter = 0;

void path_pending_init(void)
{
    memset(g_path_pending, 0, sizeof(g_path_pending));
    g_path_qso_counter = 0;
}

int path_pending_next_qso(void)
{
    /* monotonic, modulo 100000 so it fits %5u */
    g_path_qso_counter = (g_path_qso_counter + 1) % 100000;
    if (g_path_qso_counter == 0) g_path_qso_counter = 1;
    return g_path_qso_counter;
}

int path_pending_add(int qso, int kind, const char *target)
{
    path_pending_sweep();
    for (int i = 0; i < CE_PATH_MAX_PENDING; i++) {
        if (!g_path_pending[i].active) {
            g_path_pending[i].active = 1;
            g_path_pending[i].qso = qso;
            g_path_pending[i].kind = kind;
            g_path_pending[i].sent = time(NULL);
            snprintf(g_path_pending[i].target, MAX_CALLSIGN_LEN, "%s",
                     target ? target : "");
            LOG_DBG("path_pending_add: slot %d qso=%d target=%s", i, qso,
                    target ? target : "?");
            return 0;
        }
    }
    LOG_WRN("path_pending_add: no free slots (max=%d)", CE_PATH_MAX_PENDING);
    return -1;
}

int path_pending_find(int qso)
{
    for (int i = 0; i < CE_PATH_MAX_PENDING; i++) {
        if (g_path_pending[i].active && g_path_pending[i].qso == qso)
            return i;
    }
    return -1;
}

void path_pending_remove(int qso)
{
    int i = path_pending_find(qso);
    if (i >= 0) {
        LOG_DBG("path_pending_remove: slot %d qso=%d target=%s",
                i, qso, g_path_pending[i].target);
        g_path_pending[i].active = 0;
    }
}

void path_pending_sweep(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < CE_PATH_MAX_PENDING; i++) {
        if (g_path_pending[i].active &&
            (now - g_path_pending[i].sent) > CE_PATH_TIMEOUT_SEC)
        {
            LOG_INF("path_pending_sweep: timeout slot %d qso=%d target=%s",
                    i, g_path_pending[i].qso, g_path_pending[i].target);
            g_path_pending[i].active = 0;
        }
    }
}

/*
 * path_pending_dump — log the current pending-query table.
 * Only emits at DEBUG log level.  Useful when diagnosing why
 * flexdest -r shows no cache or why probes are not getting replies.
 */
void path_pending_dump(void)
{
    if (g_log_level < LOG_LEVEL_DEBUG) return;
    time_t now = time(NULL);
    int active = 0;
    for (int i = 0; i < CE_PATH_MAX_PENDING; i++)
        if (g_path_pending[i].active) active++;

    LOG_DBG("path_pending_dump: %d/%d active, qso_counter=%d",
            active, CE_PATH_MAX_PENDING, g_path_qso_counter);
    for (int i = 0; i < CE_PATH_MAX_PENDING; i++) {
        if (!g_path_pending[i].active) continue;
        long age = (long)(now - g_path_pending[i].sent);
        LOG_DBG("  [%2d] qso=%-5d kind=%s target=%-9s age=%lds (ttl=%lds)",
                i,
                g_path_pending[i].qso,
                g_path_pending[i].kind == CE_PATH_KIND_TRACE
                    ? "TRACE" : "ROUTE",
                g_path_pending[i].target,
                age,
                (long)CE_PATH_TIMEOUT_SEC - age);
    }
}
