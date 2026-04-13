/*
 * output.c — write URONode flex files
 *
 * Produces three files that URONode reads for FlexNet information:
 *   /usr/local/var/lib/ax25/flex/gateways       — neighbor gateway
 *   /usr/local/var/lib/ax25/flex/destinations    — destination table
 *   /usr/local/var/lib/ax25/flex/linkstats       — link health (L-table)
 *
 * Writes atomically via temp file + rename().
 *
 * NOTE: The 'dev' field in gateways is the axports port name (e.g. "xnet"),
 * NOT the kernel interface name (ax1). URONode uses it for display only.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "flexnetd.h"

LinkStats g_link_stats;

int output_write_gateways(void)
{
    char tmp[MAX_PATH_LEN + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_cfg.gateways_file);

    LOG_INF("output_write_gateways: writing to %s", g_cfg.gateways_file);

    /* Resolve axports name → kernel interface (e.g. "xnet" → "ax1").
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

    /* Match flexd format: "%05d %-8s %4s %s\n" */
    fprintf(f, "addr  callsign  dev  digipeaters\n");
    fprintf(f, "%05d %-8s %4s\n", 0, g_cfg.neighbor, dev);

    fclose(f);

    if (rename(tmp, g_cfg.gateways_file) < 0) {
        LOG_ERR("output_write_gateways: rename failed: %s", strerror(errno));
        unlink(tmp);
        return -1;
    }

    LOG_INF("output_write_gateways: written (gateway=%s port=%s)",
            g_cfg.neighbor, g_cfg.port_name);
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

/* ── Link stats output (xnet L-table format) ────────────────────────── */

/* Format duration as "Xh YYm" or "Xm YYs" like xnet */
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

/* Format byte count as "1.2K", "37K", "1.5M" like xnet */
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

    /* Header matching xnet L command */
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
