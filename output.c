/*
 * output.c — write flex state files
 *
 * Produces files that URONode and flexdest read for FlexNet information:
 *   /usr/local/var/lib/ax25/flex/gateways       — neighbor gateway
 *   /usr/local/var/lib/ax25/flex/destinations    — destination table
 *   /usr/local/var/lib/ax25/flex/linkstats       — link health (L-table)
 *   /usr/local/var/lib/ax25/flex/paths           — path query cache (M5.3)
 *
 * Writes atomically via temp file + rename().
 *
 * NOTE: The 'dev' field in gateways is the axports port name,
 * NOT the kernel interface name (ax1). URONode uses it for display only.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/file.h>       /* for flock() */
#include "flexnetd.h"

LinkStats g_link_stats;

/* Path cache — in-memory table, flushed to disk on every change */
static PathCacheEntry g_path_cache[CE_PATH_MAX_CACHE];
static int            g_path_cache_count = 0;

int output_write_gateways(void)
{
    char tmp[MAX_PATH_LEN + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_cfg.gateways_file);

    LOG_INF("output_write_gateways: writing to %s", g_cfg.gateways_file);

    /* Resolve axports name → kernel interface.
     * URONode uses the dev field to open an AX.25 socket — it must be
     * the kernel interface name, not the axports port name.  The old
     * flexd used ax25_config_get_dev() which does the same thing. */
    char dev[16] = {0};
    if (ax25_get_ifname(g_cfg.port_name, dev, sizeof(dev)) < 0) {
        LOG_WRN("output_write_gateways: cannot resolve '%s' to interface, "
                "falling back to port name", g_cfg.port_name);
        snprintf(dev, sizeof(dev), "%s", g_cfg.port_name);
    }

    FILE *f = fopen(tmp, "w");
    if (!f) {
        LOG_ERR("output_write_gateways: cannot open '%s': %s",
                tmp, strerror(errno));
        return -1;
    }

    /* flexd gateways format: addr callsign dev [digipeaters...]
     * URONode's read_flex_gt() (procinfo.c:113) uses strtok to parse:
     *   field 1: addr (int)
     *   field 2: call (gateway callsign, max 9 chars)
     *   field 3: dev  (interface name, max 4 chars)
     *   field 4+: digipeaters (each max 9 chars)
     *
     * Include our callsign as digipeater so outbound FlexNet connects
     * show our node in the via-list (digipeater path preservation). */
    fprintf(f, "addr  callsign  dev  digipeaters\n");
    fprintf(f, "%05d %s %s %s\n",
            0, g_cfg.neighbor, dev, g_cfg.flex_listen_call);

    fclose(f);

    if (rename(tmp, g_cfg.gateways_file) < 0) {
        LOG_ERR("output_write_gateways: rename failed: %s", strerror(errno));
        unlink(tmp);
        return -1;
    }

    LOG_INF("output_write_gateways: written (gateway=%s port=%s digi=%s)",
            g_cfg.neighbor, g_cfg.port_name, g_cfg.flex_listen_call);
    return 0;
}

/*
 * format_dest_row — render one destination entry as a text line.
 * Used by both the per-port write and the merge.
 *
 * Output (padded, fixed-width for readability):
 *   "%-9s %-5s %5d %-9s\n"  -> "DK0WUE    0-13    219 IW2OHX-12\n"
 */
static int format_dest_row(char *out, int outlen, const DestEntry *e)
{
    char ssid_range[12];
    snprintf(ssid_range, sizeof(ssid_range), "%d-%d",
             e->ssid_lo, e->ssid_hi);

    /* VIA: resolution order —
     *   1. explicit via_callsign (set by CE type-6 path queries)
     *   2. per-port neighbor based on entry.port (set by compact-record
     *      parser to match the CE session that received this route)
     *   3. legacy global g_cfg.neighbor (= ports[0].neighbor) for
     *      entries with no port/via context (e.g., D-command loads)
     * v0.7.1 fix: step 2 is new; before, all compact-record entries
     * defaulted to g_cfg.neighbor regardless of which peer sent them. */
    const char *via;
    if (e->via_callsign[0]) {
        via = e->via_callsign;
    } else if (e->port >= 0 && e->port < g_cfg.num_ports &&
               g_cfg.ports[e->port].neighbor[0]) {
        via = g_cfg.ports[e->port].neighbor;
    } else {
        via = g_cfg.neighbor;
    }

    return snprintf(out, (size_t)outlen, "%-9s %-5s %5d %-9s\n",
                    e->callsign, ssid_range, e->rtt, via);
}

/*
 * destinations_merge — read the per-port destinations files
 * (destinations.<port>) and produce a single unified destinations file.
 *
 * Merge rule: for each (callsign, ssid_range) key, keep the row with
 * the LOWEST RTT.  When RTTs tie, keep the first one seen (stable).
 * This mirrors best-path routing — users following our destinations
 * file connect through whichever peer has the shorter RTT.
 *
 * Serialised via flock on a sibling .lock file.
 *
 * File layout (M6.5):
 *   destinations.xnet   ← written by CE child for port xnet (g_port_idx=0)
 *   destinations.pcf    ← written by CE child for port pcf  (g_port_idx=1)
 *   destinations        ← merged, with header
 *   destinations.lock   ← flock sentinel
 */

#define DEST_MERGE_MAX  2048
typedef struct {
    char call[MAX_CALLSIGN_LEN];
    char ssid_range[12];
    int  rtt;
    char via[MAX_CALLSIGN_LEN];
} MergedDestRow;

static int merge_dest_cmp_key(const MergedDestRow *a, const MergedDestRow *b)
{
    int r = strcmp(a->call, b->call);
    if (r != 0) return r;
    return strcmp(a->ssid_range, b->ssid_range);
}

static int merge_dest_sort_cmp(const void *a, const void *b)
{
    return merge_dest_cmp_key((const MergedDestRow *)a,
                              (const MergedDestRow *)b);
}

static int destinations_merge(void)
{
    char lock_path[MAX_PATH_LEN + 8];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", g_cfg.dest_file);
    int lfd = open(lock_path, O_CREAT | O_WRONLY, 0644);
    if (lfd < 0) {
        LOG_ERR("destinations_merge: open lock '%s': %s",
                lock_path, strerror(errno));
        return -1;
    }
    if (flock(lfd, LOCK_EX) < 0) {
        LOG_ERR("destinations_merge: flock: %s", strerror(errno));
        close(lfd);
        return -1;
    }

    /* Accumulate best-rtt rows in memory, keyed by (call, ssid_range). */
    static MergedDestRow rows[DEST_MERGE_MAX];
    int nrows = 0;

    for (int i = 0; i < g_cfg.num_ports; i++) {
        char per_port[MAX_PATH_LEN + 24];
        snprintf(per_port, sizeof(per_port), "%s.%s",
                 g_cfg.dest_file, g_cfg.ports[i].name);
        FILE *pf = fopen(per_port, "r");
        if (!pf) continue;

        char line[256];
        while (fgets(line, sizeof(line), pf)) {
            /* skip header line */
            if (!strncmp(line, "Dest ", 5)) continue;

            MergedDestRow r;
            memset(&r, 0, sizeof(r));
            int rtt = 0;
            /* tolerate variable whitespace via sscanf */
            if (sscanf(line, "%9s %11s %d %9s",
                       r.call, r.ssid_range, &rtt, r.via) != 4)
                continue;
            r.rtt = rtt;

            /* look up existing key */
            int slot = -1;
            for (int k = 0; k < nrows; k++) {
                if (merge_dest_cmp_key(&rows[k], &r) == 0) { slot = k; break; }
            }
            if (slot < 0) {
                if (nrows >= DEST_MERGE_MAX) {
                    LOG_WRN("destinations_merge: row buffer full "
                            "(MAX=%d), dropping remaining", DEST_MERGE_MAX);
                    break;
                }
                rows[nrows++] = r;
            } else if (r.rtt < rows[slot].rtt) {
                /* better path via this peer — replace */
                rows[slot] = r;
            }
            /* else: keep existing (lower or equal RTT) */
        }
        fclose(pf);
    }

    /* Sort for deterministic output */
    if (nrows > 1)
        qsort(rows, (size_t)nrows, sizeof(MergedDestRow),
              merge_dest_sort_cmp);

    /* Write unified file atomically */
    char tmp[MAX_PATH_LEN + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_cfg.dest_file);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        LOG_ERR("destinations_merge: cannot open '%s': %s",
                tmp, strerror(errno));
        flock(lfd, LOCK_UN); close(lfd);
        return -1;
    }

    fprintf(f, "Dest     SSID    RTT Via\n");
    for (int i = 0; i < nrows; i++) {
        fprintf(f, "%-9s %-5s %5d %-9s\n",
                rows[i].call, rows[i].ssid_range, rows[i].rtt, rows[i].via);
    }
    fclose(f);

    if (rename(tmp, g_cfg.dest_file) < 0) {
        LOG_ERR("destinations_merge: rename: %s", strerror(errno));
        unlink(tmp);
        flock(lfd, LOCK_UN); close(lfd);
        return -1;
    }

    LOG_DBG("destinations_merge: wrote unified file with %d row%s "
            "(merged from %d ports)",
            nrows, nrows == 1 ? "" : "s", g_cfg.num_ports);
    flock(lfd, LOCK_UN);
    close(lfd);
    return 0;
}

/*
 * output_write_destinations — called by each CE child whenever its
 * local dtable changes (new routes received).
 *
 * M6.5 (v0.7.1): the child writes a per-port file
 * `destinations.<port_name>` containing its own dtable, then calls
 * destinations_merge() which reads ALL per-port files and produces
 * the unified `destinations` with best-RTT winners per destination.
 *
 * Two CE children peering with different neighbors no longer clobber
 * each other — whichever peer has the shorter RTT to a given destination
 * wins in the unified output, and both peers' unique routes are
 * represented.  Example: `Via IW2OHX-12` for routes where pcf is faster,
 * `Via IW2OHX-14` for routes where xnet is faster.
 *
 * Single-port / legacy fallback: if g_port_idx is invalid, write
 * directly to the unified file (preserving v0.6.0 behaviour).
 */
int output_write_destinations(void)
{
    dtable_sort();

    /* Legacy / parent fallback: no port context → write unified directly. */
    if (g_port_idx < 0 || g_port_idx >= g_cfg.num_ports) {
        char tmp[MAX_PATH_LEN + 8];
        snprintf(tmp, sizeof(tmp), "%s.tmp", g_cfg.dest_file);
        LOG_INF("output_write_destinations: writing to %s (legacy)",
                g_cfg.dest_file);
        FILE *f = fopen(tmp, "w");
        if (!f) {
            LOG_ERR("output_write_destinations: cannot open '%s': %s",
                    tmp, strerror(errno));
            return -1;
        }
        fprintf(f, "Dest     SSID    RTT Via\n");
        int written = 0, skipped = 0;
        for (int i = 0; i < g_table.count; i++) {
            DestEntry *e = &g_table.entries[i];
            if (e->rtt >= g_cfg.infinity) { skipped++; continue; }
            char row[256];
            format_dest_row(row, sizeof(row), e);
            fputs(row, f);
            written++;
        }
        fclose(f);
        if (rename(tmp, g_cfg.dest_file) < 0) {
            LOG_ERR("output_write_destinations: rename: %s", strerror(errno));
            unlink(tmp);
            return -1;
        }
        LOG_INF("output_write_destinations: written=%d skipped=%d(infinity)",
                written, skipped);
        return 0;
    }

    /* Multi-port path: write per-port file + merge unified */
    const PortCfg *pc = &g_cfg.ports[g_port_idx];

    char per_port[MAX_PATH_LEN + 24];
    char per_tmp[MAX_PATH_LEN + 32];
    snprintf(per_port, sizeof(per_port), "%s.%s",
             g_cfg.dest_file, pc->name);
    snprintf(per_tmp, sizeof(per_tmp), "%s.tmp", per_port);

    FILE *pf = fopen(per_tmp, "w");
    if (!pf) {
        LOG_ERR("output_write_destinations: per-port '%s': %s",
                per_tmp, strerror(errno));
        return -1;
    }
    /* Header goes in the per-port file too — makes it readable standalone
     * and the merge skips any "Dest " line. */
    fprintf(pf, "Dest     SSID    RTT Via\n");
    int written = 0, skipped = 0;
    for (int i = 0; i < g_table.count; i++) {
        DestEntry *e = &g_table.entries[i];
        if (e->rtt >= g_cfg.infinity) { skipped++; continue; }
        char row[256];
        format_dest_row(row, sizeof(row), e);
        fputs(row, pf);
        written++;
    }
    fclose(pf);
    if (rename(per_tmp, per_port) < 0) {
        LOG_ERR("output_write_destinations: rename per-port: %s",
                strerror(errno));
        unlink(per_tmp);
        return -1;
    }

    /* Merge all per-port files into the unified output */
    destinations_merge();

    LOG_INF("output_write_destinations: port=%s written=%d skipped=%d(infinity)",
            pc->name, written, skipped);
    return 0;
}

/* ── Link stats output (L-table format) ─────────────────────────────── */

/* Format duration as "Xh YYm" or "Xm YYs" */
static void fmt_duration(long secs, char *buf, int buflen)
{
    if (secs < 0) secs = 0;
    if (secs >= 3600)
        snprintf(buf, buflen, "%ldh %02ldm", secs / 3600, (secs % 3600) / 60);
    else if (secs >= 60)
        snprintf(buf, buflen, "%ldm %02lds", secs / 60, secs % 60);
    else
        snprintf(buf, buflen, "%lds", secs);
}

/* Format byte count as "1.2K", "37K", "1.5M" */
static void fmt_bytes(long bytes, char *buf, int buflen)
{
    if (bytes < 0) bytes = 0;
    if (bytes >= 1048576)
        snprintf(buf, buflen, "%.1fM", bytes / 1048576.0);
    else if (bytes >= 1024)
        snprintf(buf, buflen, "%.0fK", bytes / 1024.0);
    else
        snprintf(buf, buflen, "%ld", bytes);
}

/*
 * format_linkstats_row — render this process's g_link_stats into `out`
 * as a single line in the L-table format.  Returns number of chars
 * written (not including trailing nul), or -1 on error.
 *
 * Rendered line has NO trailing newline; caller adds one if needed
 * (the per-port file stores the raw line with '\n', the merged main
 * file adds them too).
 */
static int format_linkstats_row(char *out, int outlen)
{
    long elapsed = (long)(time(NULL) - g_link_stats.connect_time);
    if (elapsed < 1) elapsed = 1;

    char dur[16], tx_str[12], rx_str[12];
    fmt_duration(elapsed, dur, sizeof(dur));
    fmt_bytes(g_link_stats.tx_bytes, tx_str, sizeof(tx_str));
    fmt_bytes(g_link_stats.rx_bytes, rx_str, sizeof(rx_str));

    int txq = (g_link_stats.tx_frames > 0) ? 100 : 0;
    int rxq = (g_link_stats.rx_frames > 0) ? 100 : 0;
    long bps = (g_link_stats.tx_bytes + g_link_stats.rx_bytes) / elapsed * 8;

    return snprintf(out, (size_t)outlen,
            "%2d:%-9s %3d F %3d %3d/%-3d   %3d %7s %5s %5s"
            "   %3d/%3d   %3.1f  %5ld\n",
            g_link_stats.port_num,
            g_link_stats.neighbor,
            g_link_stats.dst_count,
            g_link_stats.qt,
            g_link_stats.rtt_last,
            g_link_stats.rtt_smoothed,
            0,                          /* tx connect count (not tracked) */
            dur, tx_str, rx_str,
            txq, rxq,
            0.0,                        /* rr+% (L2 stat, not visible at L7) */
            bps);
}

/*
 * linkstats_merge — read the per-port file for each configured port
 * and emit a single unified linkstats file with one row per port.
 * Serialised via flock on a sibling .lock file so concurrent CE
 * children don't stomp on each other.
 *
 * File layout:
 *   /usr/local/var/lib/ax25/flex/linkstats.xnet   ← written by CE child #0
 *   /usr/local/var/lib/ax25/flex/linkstats.pcf    ← written by CE child #1
 *   /usr/local/var/lib/ax25/flex/linkstats        ← merged, with header
 *   /usr/local/var/lib/ax25/flex/linkstats.lock   ← flock sentinel
 */
static int linkstats_merge(void)
{
    char lock_path[MAX_PATH_LEN + 8];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", g_cfg.linkstats_file);
    int lfd = open(lock_path, O_CREAT | O_WRONLY, 0644);
    if (lfd < 0) {
        LOG_ERR("linkstats_merge: open lock '%s': %s",
                lock_path, strerror(errno));
        return -1;
    }
    if (flock(lfd, LOCK_EX) < 0) {
        LOG_ERR("linkstats_merge: flock: %s", strerror(errno));
        close(lfd);
        return -1;
    }

    char tmp[MAX_PATH_LEN + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_cfg.linkstats_file);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        LOG_ERR("linkstats_merge: cannot open '%s': %s",
                tmp, strerror(errno));
        flock(lfd, LOCK_UN); close(lfd);
        return -1;
    }

    /* L-table header (once, at the top) */
    fprintf(f, "Link to       dst    Q/T    rtt    tx connect"
               "   tx   rx   txq/rxq  rr+%%  bit/s\n");

    int rows = 0;
    for (int i = 0; i < g_cfg.num_ports; i++) {
        char per_port[MAX_PATH_LEN + 24];
        snprintf(per_port, sizeof(per_port), "%s.%s",
                 g_cfg.linkstats_file, g_cfg.ports[i].name);
        FILE *pf = fopen(per_port, "r");
        if (!pf) continue;   /* port not active yet / no session */

        char line[512];
        if (fgets(line, sizeof(line), pf)) {
            fputs(line, f);
            rows++;
        }
        fclose(pf);
    }
    fclose(f);

    if (rename(tmp, g_cfg.linkstats_file) < 0) {
        LOG_ERR("linkstats_merge: rename: %s", strerror(errno));
        unlink(tmp);
        flock(lfd, LOCK_UN); close(lfd);
        return -1;
    }

    LOG_DBG("linkstats_merge: wrote unified file with %d row%s",
            rows, rows == 1 ? "" : "s");
    flock(lfd, LOCK_UN);
    close(lfd);
    return 0;
}

/*
 * output_write_linkstats — called periodically by each CE child to
 * refresh the link status for THIS session.
 *
 * M6.6: the child writes a single-line file named
 * `linkstats.<port_name>` (e.g. linkstats.xnet) with its own row,
 * then calls linkstats_merge() to build the unified linkstats file
 * from all known per-port files.  Two CE children peering with
 * different neighbors therefore produce TWO visible rows in URONode's
 * `fl` output instead of clobbering each other.
 *
 * If g_port_idx is not set (single-port legacy mode or parent-process
 * write), we fall back to writing the unified file directly with the
 * single row — preserving v0.6.0 behaviour for single-port setups.
 */
int output_write_linkstats(void)
{
    if (!g_link_stats.active) return 0;

    /* Legacy / parent fallback: no port context → write directly. */
    if (g_port_idx < 0 || g_port_idx >= g_cfg.num_ports) {
        char tmp[MAX_PATH_LEN + 8];
        snprintf(tmp, sizeof(tmp), "%s.tmp", g_cfg.linkstats_file);
        FILE *f = fopen(tmp, "w");
        if (!f) {
            LOG_ERR("output_write_linkstats: cannot open '%s': %s",
                    tmp, strerror(errno));
            return -1;
        }
        fprintf(f, "Link to       dst    Q/T    rtt    tx connect"
                   "   tx   rx   txq/rxq  rr+%%  bit/s\n");
        char row[512];
        format_linkstats_row(row, sizeof(row));
        fputs(row, f);
        fclose(f);
        if (rename(tmp, g_cfg.linkstats_file) < 0) {
            LOG_ERR("output_write_linkstats: rename: %s", strerror(errno));
            unlink(tmp);
            return -1;
        }
        LOG_DBG("output_write_linkstats: single-port write (no port ctx)");
        return 0;
    }

    /* Multi-port path: write per-port file + merge unified */
    const PortCfg *pc = &g_cfg.ports[g_port_idx];

    char per_port[MAX_PATH_LEN + 24];
    char per_tmp[MAX_PATH_LEN + 32];
    snprintf(per_port, sizeof(per_port), "%s.%s",
             g_cfg.linkstats_file, pc->name);
    snprintf(per_tmp, sizeof(per_tmp), "%s.tmp", per_port);

    FILE *pf = fopen(per_tmp, "w");
    if (!pf) {
        LOG_ERR("output_write_linkstats: per-port '%s': %s",
                per_tmp, strerror(errno));
        return -1;
    }
    char row[512];
    format_linkstats_row(row, sizeof(row));
    fputs(row, pf);
    fclose(pf);
    if (rename(per_tmp, per_port) < 0) {
        LOG_ERR("output_write_linkstats: rename per-port: %s", strerror(errno));
        unlink(per_tmp);
        return -1;
    }

    /* Merge all per-port files into the unified output */
    linkstats_merge();

    LOG_DBG("output_write_linkstats: port=%s dst=%d Q/T=%d rtt=%d/%d "
            "tx_bytes=%ld rx_bytes=%ld",
            pc->name, g_link_stats.dst_count, g_link_stats.qt,
            g_link_stats.rtt_last, g_link_stats.rtt_smoothed,
            g_link_stats.tx_bytes, g_link_stats.rx_bytes);
    return 0;
}

/* ── Path cache (CE type-7 reply cache) ─────────────────────────────── */
/*
 * The cache is an in-memory ring of recent replies, flushed to a text
 * file so the standalone `flexdest` tool can read it.  Old entries
 * beyond CE_PATH_CACHE_TTL_SEC are pruned on each insert.
 *
 * File format (one line per cached path):
 *   <target> <kind> <n_hops> <unix_ts> <hop1> <hop2> ... <hopN>
 *
 * <kind> is 'R' for Route or 'T' for Traceroute.
 */
static void path_cache_prune(void)
{
    time_t now = time(NULL);
    int out = 0;
    for (int i = 0; i < g_path_cache_count; i++) {
        if ((now - g_path_cache[i].cached) <= CE_PATH_CACHE_TTL_SEC) {
            if (out != i) g_path_cache[out] = g_path_cache[i];
            out++;
        }
    }
    if (out < g_path_cache_count) {
        LOG_DBG("path_cache_prune: dropped %d stale entries",
                g_path_cache_count - out);
    }
    g_path_cache_count = out;
}

int output_write_paths_cache_add(const PathReply *r)
{
    if (!r || r->hop_callsign_count <= 0) return -1;

    path_cache_prune();

    /* last hop in the reply is the target (the destination we asked
     * about) — look it up and either update or insert. */
    const char *target = r->hop_callsigns[r->hop_callsign_count - 1];
    int slot = -1;
    for (int i = 0; i < g_path_cache_count; i++) {
        if (strcasecmp(g_path_cache[i].target, target) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (g_path_cache_count >= CE_PATH_MAX_CACHE) {
            /* evict oldest entry */
            int oldest = 0;
            for (int i = 1; i < g_path_cache_count; i++) {
                if (g_path_cache[i].cached < g_path_cache[oldest].cached)
                    oldest = i;
            }
            slot = oldest;
        } else {
            slot = g_path_cache_count++;
        }
    }

    memset(&g_path_cache[slot], 0, sizeof(PathCacheEntry));
    snprintf(g_path_cache[slot].target, MAX_CALLSIGN_LEN, "%s", target);
    g_path_cache[slot].kind   = r->kind;
    g_path_cache[slot].cached = r->received;
    int n = r->hop_callsign_count;
    if (n > CE_PATH_MAX_HOPS) n = CE_PATH_MAX_HOPS;
    g_path_cache[slot].hop_callsign_count = n;
    for (int i = 0; i < n; i++) {
        snprintf(g_path_cache[slot].hop_callsigns[i], MAX_CALLSIGN_LEN,
                 "%s", r->hop_callsigns[i]);
    }

    LOG_INF("output_write_paths_cache_add: target=%s kind=%c hops=%d",
            target,
            r->kind == CE_PATH_KIND_TRACE ? 'T' : 'R',
            n);
    return output_write_paths_cache_flush();
}

int output_write_paths_cache_flush(void)
{
    if (!g_cfg.paths_file[0]) return 0;

    char tmp[MAX_PATH_LEN + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_cfg.paths_file);

    FILE *f = fopen(tmp, "w");
    if (!f) {
        LOG_ERR("output_write_paths_cache_flush: cannot open '%s': %s",
                tmp, strerror(errno));
        return -1;
    }

    fprintf(f, "# flexnetd path cache — target kind n_hops unix_ts hops...\n");
    for (int i = 0; i < g_path_cache_count; i++) {
        const PathCacheEntry *e = &g_path_cache[i];
        fprintf(f, "%s %c %d %ld",
                e->target,
                e->kind == CE_PATH_KIND_TRACE ? 'T' : 'R',
                e->hop_callsign_count,
                (long)e->cached);
        for (int k = 0; k < e->hop_callsign_count; k++)
            fprintf(f, " %s", e->hop_callsigns[k]);
        fprintf(f, "\n");
    }
    fclose(f);

    if (rename(tmp, g_cfg.paths_file) < 0) {
        LOG_ERR("output_write_paths_cache_flush: rename failed: %s",
                strerror(errno));
        unlink(tmp);
        return -1;
    }

    LOG_DBG("output_write_paths_cache_flush: wrote %d paths to %s",
            g_path_cache_count, g_cfg.paths_file);
    return 0;
}
