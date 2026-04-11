/*
 * output.c — write URONode flex files
 *
 * Produces two files that URONode reads for FlexNet destinations:
 *   /usr/local/var/lib/ax25/flex/gateways
 *   /usr/local/var/lib/ax25/flex/destinations
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

    fprintf(f, "callsign  ssid     rtt  gateway\n");

    int written = 0, skipped = 0;

    for (int i = 0; i < g_table.count; i++) {
        DestEntry *e = &g_table.entries[i];

        if (e->rtt >= g_cfg.infinity) { skipped++; continue; }

        char ssid_range[12];
        snprintf(ssid_range, sizeof(ssid_range), "%d-%d",
                 e->ssid_lo, e->ssid_hi);

        /* Format confirmed from live file:
         *   "%-9s %-8s %5d    %05d\n"
         *   e.g. "DB0AAT    0-9        7    00000" */
        fprintf(f, "%-9s %-8s %5d    %05d\n",
                e->callsign, ssid_range, e->rtt, 0);
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
