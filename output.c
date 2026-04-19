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

int output_write_destinations(void)
{
    char tmp[MAX_PATH_LEN + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_cfg.dest_file);

    LOG_INF("output_write_destinations: writing to %s", g_cfg.dest_file);

    dtable_sort();

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

        char ssid_range[12];
        snprintf(ssid_range, sizeof(ssid_range), "%d-%d",
                 e->ssid_lo, e->ssid_hi);

        /* VIA: use via_callsign if set, otherwise fall back to
         * the configured neighbor (all routes arrive through it) */
        const char *via = e->via_callsign[0]
                        ? e->via_callsign : g_cfg.neighbor;

        fprintf(f, "%-9s %-5s %5d %-9s\n",
                e->callsign, ssid_range, e->rtt, via);
        written++;
    }

    fclose(f);

    if (rename(tmp, g_cfg.dest_file) < 0) {
        LOG_ERR("output_write_destinations: rename failed: %s", strerror(errno));
        unlink(tmp);
        return -1;
    }

    LOG_INF("output_write_destinations: written=%d skipped=%d(infinity)",
            written, skipped);
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

int output_write_linkstats(void)
{
    if (!g_link_stats.active) return 0;

    char tmp[MAX_PATH_LEN + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_cfg.linkstats_file);

    FILE *f = fopen(tmp, "w");
    if (!f) {
        LOG_ERR("output_write_linkstats: cannot open '%s': %s",
                tmp, strerror(errno));
        return -1;
    }

    /* Header matching the L command format */
    fprintf(f, "Link to       dst    Q/T    rtt    tx connect"
               "   tx   rx   txq/rxq  rr+%%  bit/s\n");

    long elapsed = (long)(time(NULL) - g_link_stats.connect_time);
    if (elapsed < 1) elapsed = 1;

    char dur[16];
    fmt_duration(elapsed, dur, sizeof(dur));

    char tx_str[12], rx_str[12];
    fmt_bytes(g_link_stats.tx_bytes, tx_str, sizeof(tx_str));
    fmt_bytes(g_link_stats.rx_bytes, rx_str, sizeof(rx_str));

    /* tx/rx frame success: we see all frames at L7, so 100/100
     * unless we have send failures (not tracked yet) */
    int txq = (g_link_stats.tx_frames > 0) ? 100 : 0;
    int rxq = (g_link_stats.rx_frames > 0) ? 100 : 0;

    /* bit/s from total bytes / elapsed (divide first to avoid overflow) */
    long bps = (g_link_stats.tx_bytes + g_link_stats.rx_bytes) / elapsed * 8;

    /* port:neighbor  dst  mode Q/T  rtt_last/smoothed  tx  connect
     *   tx_bytes  rx_bytes  txq/rxq  rr+%  bit/s */
    fprintf(f, "%2d:%-9s %3d F %3d %3d/%-3d   %3d %7s %5s %5s"
               "   %3d/%3d   %3.1f  %5ld\n",
            g_link_stats.port_num,
            g_link_stats.neighbor,
            g_link_stats.dst_count,
            g_link_stats.qt,
            g_link_stats.rtt_last,
            g_link_stats.rtt_smoothed,
            0,                          /* tx connect count (not tracked) */
            dur,
            tx_str, rx_str,
            txq, rxq,
            0.0,                        /* rr+% (L2 stat, not visible at L7) */
            bps);

    fclose(f);

    if (rename(tmp, g_cfg.linkstats_file) < 0) {
        LOG_ERR("output_write_linkstats: rename failed: %s", strerror(errno));
        unlink(tmp);
        return -1;
    }

    LOG_DBG("output_write_linkstats: dst=%d Q/T=%d rtt=%d/%d %s tx=%s rx=%s",
            g_link_stats.dst_count, g_link_stats.qt,
            g_link_stats.rtt_last, g_link_stats.rtt_smoothed,
            dur, tx_str, rx_str);
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
