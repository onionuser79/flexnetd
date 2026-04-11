/*
 * dtable.c — destination routing table
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "flexnetd.h"

DestTable g_table;

void dtable_init(void)
{
    memset(&g_table, 0, sizeof(g_table));
    LOG_INF("dtable_init: table ready (max %d entries)", MAX_DEST_ENTRIES);
}

int dtable_find(const char *callsign, int ssid_lo, int ssid_hi)
{
    for (int i = 0; i < g_table.count; i++) {
        DestEntry *e = &g_table.entries[i];
        if (strcmp(e->callsign, callsign) == 0 &&
            e->ssid_lo == ssid_lo &&
            e->ssid_hi == ssid_hi)
            return i;
    }
    return -1;
}

/*
 * dtable_merge — merge one received entry.
 * Returns: 1=new, 2=improved, 3=degraded, 0=no change, -1=table full
 */
int dtable_merge(const DestEntry *incoming)
{
    int idx = dtable_find(incoming->callsign,
                          incoming->ssid_lo, incoming->ssid_hi);

    if (idx < 0) {
        if (g_table.count >= MAX_DEST_ENTRIES) {
            LOG_WRN("dtable_merge: table full (%d entries), dropping %s",
                    MAX_DEST_ENTRIES, incoming->callsign);
            return -1;
        }
        g_table.entries[g_table.count] = *incoming;
        g_table.entries[g_table.count].last_updated = time(NULL);
        g_table.count++;
        LOG_DBG("dtable_merge: NEW  %-9s %2d-%2d  %s",
                incoming->callsign, incoming->ssid_lo, incoming->ssid_hi,
                rtt_str(incoming->rtt));
        return 1;
    }

    DestEntry *existing = &g_table.entries[idx];
    int old_rtt = existing->rtt;
    if (incoming->rtt == old_rtt) return 0;

    existing->rtt          = incoming->rtt;
    existing->port         = incoming->port;
    existing->node_type    = incoming->node_type;
    existing->is_infinity  = incoming->is_infinity;
    existing->last_updated = time(NULL);
    if (incoming->node_alias[0])
        snprintf(existing->node_alias, MAX_ALIAS_LEN,
                 "%s", incoming->node_alias);

    int improved = (incoming->rtt < old_rtt);
    LOG_DBG("dtable_merge: %s  %-9s %2d-%2d  %s -> %s",
            improved ? "IMPR" : "DEGR",
            incoming->callsign, incoming->ssid_lo, incoming->ssid_hi,
            rtt_str(old_rtt), rtt_str(incoming->rtt));
    return improved ? 2 : 3;
}

/*
 * dtable_load_from_text — parse D-command response and merge all entries.
 *
 * The Xnet D command outputs entries as whitespace-separated triplets:
 *   CALLSIGN  LO-HI  RTT   CALLSIGN  LO-HI  RTT  ...
 * Multiple entries per line, \r\n terminated. No port/type/alias fields.
 *
 * We tokenise the entire response and process triplets:
 *   token[0] = callsign  (starts with uppercase letter, 3-9 chars)
 *   token[1] = ssid range "LO-HI"  (both parts all-digits)
 *   token[2] = RTT       (all digits, < RTT_INFINITY)
 *
 * The tokeniser is self-synchronising: a token that fails validation
 * advances by 1 position rather than 3, so misalignment self-corrects.
 *
 * Returns number of new entries merged.
 */
int dtable_load_from_text(const char *text, int gw_idx)
{
    (void)gw_idx;
    int merged = 0;

    char *buf = strdup(text);
    if (!buf) {
        LOG_ERR("dtable_load_from_text: strdup failed");
        return 0;
    }

    /* normalise: replace \r and \n with spaces */
    for (char *c = buf; *c; c++)
        if (*c == '\r' || *c == '\n') *c = ' ';

    /* tokenise into a flat array (pointer into buf) */
#define MAX_TOKENS 16384
    char *tokens[MAX_TOKENS];
    int   ntok = 0;
    char *p    = buf;

    while (*p && ntok < MAX_TOKENS - 1) {
        while (*p == ' ') p++;
        if (!*p) break;
        tokens[ntok++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }

    LOG_DBG("dtable_load_from_text: %d tokens from %d bytes",
            ntok, (int)strlen(text));

    int i = 0;
    while (i + 2 < ntok) {
        const char *call   = tokens[i];
        const char *srange = tokens[i + 1];
        const char *rtt_s  = tokens[i + 2];

        /* ── validate callsign ───────────────────────────────────── */
        int clen = (int)strlen(call);
        if (clen < 3 || clen >= MAX_CALLSIGN_LEN ||
            !isupper((unsigned char)call[0])) {
            i++; continue;
        }
        /* must contain only uppercase letters, digits, '-' */
        int call_ok = 1;
        for (int k = 0; k < clen; k++) {
            char ch = call[k];
            if (!isupper((unsigned char)ch) &&
                !isdigit((unsigned char)ch) &&
                ch != '-') { call_ok = 0; break; }
        }
        if (!call_ok) { i++; continue; }

        /* ── validate ssid range "LO-HI" ────────────────────────── */
        const char *dash = strchr(srange, '-');
        if (!dash || dash == srange || *(dash + 1) == '\0') {
            i++; continue;
        }
        int lo_ok = 1, hi_ok = 1;
        for (const char *c = srange; c < dash; c++)
            if (!isdigit((unsigned char)*c)) { lo_ok = 0; break; }
        for (const char *c = dash + 1; *c; c++)
            if (!isdigit((unsigned char)*c)) { hi_ok = 0; break; }
        if (!lo_ok || !hi_ok) { i++; continue; }

        /* ── validate RTT ────────────────────────────────────────── */
        int rtt_ok = (strlen(rtt_s) > 0);
        for (const char *c = rtt_s; *c; c++)
            if (!isdigit((unsigned char)*c)) { rtt_ok = 0; break; }
        if (!rtt_ok) { i++; continue; }

        /* ── all valid — build entry ─────────────────────────────── */
        int ssid_lo = atoi(srange);
        int ssid_hi = atoi(dash + 1);
        int rtt     = atoi(rtt_s);

        if (rtt     <  0          || rtt     >= RTT_INFINITY) { i += 3; continue; }
        if (ssid_lo <  0          || ssid_lo >  MAX_SSID)     { i += 3; continue; }
        if (ssid_hi <  ssid_lo    || ssid_hi >  MAX_SSID)     { i += 3; continue; }

        DestEntry e;
        memset(&e, 0, sizeof(e));
        snprintf(e.callsign, MAX_CALLSIGN_LEN, "%s", call);
        e.ssid_lo    = ssid_lo;
        e.ssid_hi    = ssid_hi;
        e.rtt        = rtt;
        e.is_infinity = 0;

        LOG_DBG("dtable_load_from_text: %-9s %2d-%2d  rtt=%d",
                e.callsign, e.ssid_lo, e.ssid_hi, e.rtt);

        if (dtable_merge(&e) > 0) merged++;
        i += 3;
    }

    free(buf);
    LOG_INF("dtable_load_from_text: merged=%d  table_total=%d",
            merged, g_table.count);
    return merged;
}

void dtable_dump(void)
{
    if (g_log_level < LOG_LEVEL_DEBUG) return;
    LOG_DBG("dtable_dump: %d entries", g_table.count);
    for (int i = 0; i < g_table.count; i++) {
        DestEntry *e = &g_table.entries[i];
        LOG_DBG("  [%4d] %-9s %2d-%2d  %s  port=%d type=%d alias='%s'",
                i, e->callsign, e->ssid_lo, e->ssid_hi,
                rtt_str(e->rtt), e->port, e->node_type, e->node_alias);
    }
}

int dtable_count_reachable(void)
{
    int n = 0;
    for (int i = 0; i < g_table.count; i++)
        if (g_table.entries[i].rtt < g_cfg.infinity) n++;
    return n;
}

static int dest_cmp(const void *a, const void *b)
{
    const DestEntry *ea = (const DestEntry *)a;
    const DestEntry *eb = (const DestEntry *)b;
    int r = strcmp(ea->callsign, eb->callsign);
    if (r != 0) return r;
    if (ea->ssid_lo != eb->ssid_lo) return ea->ssid_lo - eb->ssid_lo;
    return ea->ssid_hi - eb->ssid_hi;
}

void dtable_sort(void)
{
    qsort(g_table.entries, (size_t)g_table.count, sizeof(DestEntry), dest_cmp);
    LOG_DBG("dtable_sort: sorted %d entries", g_table.count);
}
