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
 * Returns: 1=new, 2=improved, 3=degraded, 0=no change/skipped, -1=table full
 *
 * RTT=0 handling (v0.7.5): xnet advertises its FlexNet dtable in two
 * rounds after session init — the first with measured RTTs (e.g. 242,
 * 208), the second ~20 seconds later with RTT=0 for every record.
 * RTT=0 is not a "measured zero round-trip" — it is either (a) a
 * refresh/keepalive marker for destinations already in the peer's
 * dtable, or (b) xnet re-advertising back to us the routes it knows
 * via us.  Either way, letting it overwrite a real measured RTT via
 * the "0 < existing_rtt -> improved" path corrupts the display (users
 * see all RTTs as 0 in `fld` and the D-command).  Skip the update
 * when incoming rtt == 0 and keep whatever we already have.  If we
 * have no entry yet, we also skip — the destination will re-advertise
 * with a real RTT soon enough, and in the meantime we don't want to
 * pollute the table with a useless RTT=0 row.
 */
int dtable_merge(const DestEntry *incoming)
{
    int idx = dtable_find(incoming->callsign,
                          incoming->ssid_lo, incoming->ssid_hi);

    /* v0.7.5: RTT=0 is a protocol marker, not a real measurement.
     * Skip the merge entirely — preserve existing data if any,
     * don't insert a new row with RTT=0. */
    if (incoming->rtt == 0 && !incoming->is_infinity) {
        if (idx >= 0) {
            /* Touch last_updated so the entry isn't aged out, but
             * don't change rtt/port/etc. */
            g_table.entries[idx].last_updated = time(NULL);
            LOG_DBG("dtable_merge: KEEP %-9s %2d-%2d  rtt=%s "
                    "(incoming rtt=0, preserved)",
                    incoming->callsign,
                    incoming->ssid_lo, incoming->ssid_hi,
                    rtt_str(g_table.entries[idx].rtt));
        } else {
            LOG_DBG("dtable_merge: SKIP %-9s %2d-%2d  "
                    "(incoming rtt=0, no existing entry)",
                    incoming->callsign,
                    incoming->ssid_lo, incoming->ssid_hi);
        }
        return 0;
    }

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
    if (incoming->via_callsign[0])
        snprintf(existing->via_callsign, MAX_CALLSIGN_LEN,
                 "%s", incoming->via_callsign);

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
 * The D command outputs entries as whitespace-separated triplets:
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
