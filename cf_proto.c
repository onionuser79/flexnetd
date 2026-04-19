/*
 * cf_proto.c — PID=CF NET/ROM-compatible FlexNet protocol
 *
 * Handles L3RTT probe/reply and D-table destination record parsing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "flexnetd.h"

/* Fixed hardware serial used on this node */
#define NODE_SERIAL  "3076541136"

/*
 * cf_build_l3rtt — build an L3RTT probe/reply payload.
 *
 * Wire format:
 *   "L3RTT:%11lu%11lu%11lu%11lu %-6.6s LEVEL3_V2.1 <ver> $M<max_dest> $N\r"
 *
 * Four %11lu fields: counter1..counter4 (RTT timing counters).
 *   Probe:  c1=sender_tick, c2=0,  c3=0,         c4=0
 *   Reply:  c1=sender_tick, c2=0,  c3=recv_tick,  c4=send_tick
 *
 * val1/val2 semantics (c3/c4 in reply):
 *   c3=0, c4=0    when link is down ($M=60000 or no routes)
 *   c3>0, c4>0    when link is active (peer uses these for RTT calc)
 * Setting non-zero c3/c4 while the link is effectively down will mislead
 * the peer's Q/T convergence.
 *
 * $M = reachable destination count (not an RTT value).
 *
 * Returns bytes written, -1 on error.
 */
int cf_build_l3rtt(uint8_t *buf, int buflen,
                   uint32_t counter1, uint32_t counter2,
                   uint32_t counter3, uint32_t counter4,
                   const char *alias, const char *version,
                   uint32_t max_dest)
{
    char payload[256];
    int len = snprintf(payload, sizeof(payload),
        "L3RTT:%11lu%11lu%11lu%11lu %-6.6s LEVEL3_V2.1 %s $M%u $N\r",
        (unsigned long)counter1, (unsigned long)counter2,
        (unsigned long)counter3, (unsigned long)counter4,
        alias   ? alias   : "NONE  ",
        version ? version : LEVEL3_VERSION_STR,
        (unsigned)max_dest);

    if (len < 0 || len >= (int)sizeof(payload)) {
        LOG_ERR("cf_build_l3rtt: snprintf overflow");
        return -1;
    }
    if (len >= buflen) {
        LOG_ERR("cf_build_l3rtt: output buffer too small (%d need %d)",
                buflen, len);
        return -1;
    }

    memcpy(buf, payload, (size_t)len);
    LOG_DBG("cf_build_l3rtt: c1=%u c2=%u c3=%u c4=%u $M=%u len=%d",
            counter1, counter2, counter3, counter4, max_dest, len);
    return len;
}

/*
 * cf_parse_l3rtt — parse a received L3RTT frame payload.
 *
 * Wire format: "L3RTT:%11lu%11lu%11lu%11lu %-6.6s LEVEL3_V2.1 <ver> $M<n> $N\r"
 * Four counter fields (11 chars wide each), alias, version, $M=max dest count.
 *
 * Handles the 80-char monitor wrap that splits $M across lines by
 * first joining all whitespace runs into single spaces.
 *
 * Returns 0 on success, -1 on parse failure.
 */
int cf_parse_l3rtt(const uint8_t *data, int len,
                   uint32_t *counter1_out, uint32_t *counter2_out,
                   uint32_t *counter3_out, uint32_t *counter4_out,
                   int *lt_out,
                   char *alias_out, char *version_out,
                   uint32_t *max_dest_out)
{
    /* copy to C string and replace newlines with spaces */
    char text[1024];
    int copy_len = (len < 1023) ? len : 1023;
    memcpy(text, data, (size_t)copy_len);
    text[copy_len] = '\0';
    for (int i = 0; i < copy_len; i++)
        if (text[i] == '\n') text[i] = ' ';

    LOG_DBG("cf_parse_l3rtt: raw='%.200s'", text);

    if (!strstr(text, "L3RTT:")) {
        LOG_DBG("cf_parse_l3rtt: not an L3RTT frame");
        return -1;
    }

    /* LT from L3 header prefix (monitor display only, not in wire payload) */
    if (lt_out) {
        *lt_out = 0;
        const char *lt_p = strstr(text, "LT ");
        if (lt_p) *lt_out = atoi(lt_p + 3);
        LOG_DBG("cf_parse_l3rtt: LT=%d", lt_out ? *lt_out : 0);
    }

    /* find L3RTT: token and advance */
    const char *p = strstr(text, "L3RTT:");
    if (!p) return -1;
    p += 6;
    while (*p == ' ') p++;

    /* counter1 */
    uint32_t c1 = (uint32_t)strtoul(p, (char **)&p, 10);
    if (counter1_out) *counter1_out = c1;

    /* counter2 */
    while (*p == ' ') p++;
    uint32_t c2 = (uint32_t)strtoul(p, (char **)&p, 10);
    if (counter2_out) *counter2_out = c2;

    /* counter3 */
    while (*p == ' ') p++;
    uint32_t c3 = (uint32_t)strtoul(p, (char **)&p, 10);
    if (counter3_out) *counter3_out = c3;

    /* counter4 */
    while (*p == ' ') p++;
    uint32_t c4 = (uint32_t)strtoul(p, (char **)&p, 10);
    if (counter4_out) *counter4_out = c4;

    /* alias */
    while (*p == ' ') p++;
    char alias[MAX_ALIAS_LEN];
    int ai = 0;
    while (*p && !isspace((unsigned char)*p) && ai < (int)sizeof(alias) - 1)
        alias[ai++] = *p++;
    alias[ai] = '\0';
    if (alias_out) snprintf(alias_out, MAX_ALIAS_LEN, "%s", alias);

    /* skip LEVEL3_V2.1 */
    p = strstr(p, "LEVEL3_V2.1");
    if (!p) { LOG_DBG("cf_parse_l3rtt: no LEVEL3_V2.1"); return -1; }
    p += 11;

    /* version string */
    while (*p == ' ') p++;
    char ver[16];
    int vi = 0;
    while (*p && !isspace((unsigned char)*p) && vi < 15)
        ver[vi++] = *p++;
    ver[vi] = '\0';
    if (version_out) snprintf(version_out, 16, "%s", ver);

    /* $M value (max dest count) — may be split by the 80-char monitor wrap */
    const char *m_p = strstr(p, "$M");
    if (!m_p) { LOG_DBG("cf_parse_l3rtt: no $M token"); return -1; }
    m_p += 2;

    char md_buf[12];
    int ri = 0;
    const char *scan = m_p;
    while (*scan && ri < 11) {
        if (isdigit((unsigned char)*scan))
            md_buf[ri++] = *scan++;
        else if (*scan == ' ' || *scan == '\r' || *scan == '\n')
            scan++;     /* skip wrap whitespace between digit groups */
        else
            break;
    }
    md_buf[ri] = '\0';
    uint32_t max_dest = ri ? (uint32_t)strtoul(md_buf, NULL, 10) : 0;
    if (max_dest_out) *max_dest_out = max_dest;

    LOG_DBG("cf_parse_l3rtt: c1=%u c2=%u c3=%u c4=%u "
            "alias='%s' ver='%s' $M=%u",
            c1, c2, c3, c4, alias, ver, max_dest);
    return 0;
}

/*
 * cf_parse_dtable_line — parse one D-table destination record.
 *
 * Monitor format:
 *   "CALLSIGN-SSID   RTT/SSID_MAX  [PORT[TYPE] 'ALIAS']  ..."
 *
 * Returns 0 on success, -1 if line is not a destination record.
 */
int cf_parse_dtable_line(const char *line, DestEntry *e)
{
    memset(e, 0, sizeof(*e));

    while (*line == ' ' || *line == '\t') line++;

    if (!isupper((unsigned char)*line)) return -1;

    /* callsign */
    int ci = 0;
    while (*line && !isspace((unsigned char)*line) &&
           ci < MAX_CALLSIGN_LEN - 1)
        e->callsign[ci++] = *line++;
    e->callsign[ci] = '\0';
    if (ci == 0) return -1;

    while (*line == ' ' || *line == '\t') line++;

    /* RTT/SSID_MAX */
    if (!isdigit((unsigned char)*line)) return -1;
    e->rtt = (int)strtol(line, (char **)&line, 10);
    e->is_infinity = (e->rtt >= RTT_INFINITY);

    if (*line != '/') return -1;
    line++;
    e->ssid_hi = (int)strtol(line, (char **)&line, 10);
    e->ssid_lo = 0;

    /* optional PORT[TYPE] 'ALIAS' */
    while (*line == ' ' || *line == '\t') line++;
    if (isdigit((unsigned char)*line)) {
        e->port = (int)strtol(line, (char **)&line, 10);
        if (*line == '[') {
            line++;
            e->node_type = (int)strtol(line, (char **)&line, 10);
            if (*line == ']') line++;
        }
        while (*line == ' ') line++;
        if (*line == '\'') {
            line++;
            int ai = 0;
            while (*line && *line != '\'' && ai < MAX_ALIAS_LEN - 1)
                e->node_alias[ai++] = *line++;
            e->node_alias[ai] = '\0';
        }
    }

    LOG_DBG("cf_parse_dtable_line: %-9s ssid_hi=%2d rtt=%s "
            "port=%d type=%d alias='%s'",
            e->callsign, e->ssid_hi, rtt_str(e->rtt),
            e->port, e->node_type, e->node_alias);
    return 0;
}

/*
 * get_uptime_ticks — node uptime counter in 10ms ticks.
 * Uses CLOCK_BOOTTIME (monotonic, includes suspend time).
 */
uint32_t get_uptime_ticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (uint32_t)(ts.tv_sec * 100 + ts.tv_nsec / 10000000L);
}
