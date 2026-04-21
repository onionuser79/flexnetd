/*
 * config.c — configuration file parser
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "flexnetd.h"

FlexConfig g_cfg;

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = '\0';
    return s;
}

static void set_defaults(FlexConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->mycall,           "NOCALL-7");
    strcpy(cfg->alias,            "NONE");
    cfg->min_ssid             = 0;
    cfg->max_ssid             = 15;
    strcpy(cfg->neighbor,         "NOCALL-14");
    strcpy(cfg->port_name,        "xnet");
    strcpy(cfg->flex_listen_call, "IW2OHX-9");
    cfg->role                 = 1;   /* default: server mode */
    cfg->poll_interval        = DEFAULT_POLL_INTERVAL;
    cfg->keepalive_interval   = DEFAULT_KEEPALIVE_S;
    cfg->beacon_interval      = DEFAULT_BEACON_S;
    cfg->trigger_threshold    = DEFAULT_TRIGGER_THRESH;
    cfg->infinity             = RTT_INFINITY;
    strcpy(cfg->gateways_file,    "/usr/local/var/lib/ax25/flex/gateways");
    strcpy(cfg->dest_file,        "/usr/local/var/lib/ax25/flex/destinations");
    strcpy(cfg->linkstats_file,   "/usr/local/var/lib/ax25/flex/linkstats");
    strcpy(cfg->paths_file,       "/usr/local/var/lib/ax25/flex/paths");
    cfg->log_level            = LOG_LEVEL_INFO;
    cfg->use_syslog           = 0;
    cfg->probe_count          = DEFAULT_PROBE_COUNT;
    cfg->path_probe_interval  = CE_PATH_PROBE_SEC;
    cfg->route_advert_interval = DEFAULT_ROUTE_ADVERT_S;
    cfg->lt_reply_interval    = DEFAULT_LT_REPLY_S;
}

int config_load(const char *path, FlexConfig *cfg)
{
    set_defaults(cfg);

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "config_load: cannot open '%s': %m\n", path);
        return -1;
    }

    char line[512];
    int  lineno = 0;

    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *p = trim(line);
        if (*p == '#' || *p == '\0') continue;

        char *key = p;
        char *val = p;
        while (*val && !isspace((unsigned char)*val)) val++;
        if (*val) { *val++ = '\0'; val = trim(val); }
        else       { val = ""; }

        char *comment = strchr(val, '#');
        if (comment) { *comment = '\0'; trim(val); }

        LOG_DBG("config[%d]: key='%s' val='%s'", lineno, key, val);

        if      (!strcasecmp(key, "MyCall"))
            snprintf(cfg->mycall, MAX_CALLSIGN_LEN, "%s", val),
            callsign_upper(cfg->mycall);
        else if (!strcasecmp(key, "Alias"))
            snprintf(cfg->alias, MAX_ALIAS_LEN, "%s", val);
        else if (!strcasecmp(key, "MinSSID"))
            cfg->min_ssid = atoi(val);
        else if (!strcasecmp(key, "MaxSSID"))
            cfg->max_ssid = atoi(val);
        else if (!strcasecmp(key, "Neighbor"))
            snprintf(cfg->neighbor, MAX_CALLSIGN_LEN, "%s", val),
            callsign_upper(cfg->neighbor);
        else if (!strcasecmp(key, "PortName"))
            snprintf(cfg->port_name, MAX_IFACE_LEN, "%s", val);
        else if (!strcasecmp(key, "Interface"))         /* legacy alias */
            snprintf(cfg->port_name, MAX_IFACE_LEN, "%s", val);
        else if (!strcasecmp(key, "FlexListenCall"))
            snprintf(cfg->flex_listen_call, MAX_CALLSIGN_LEN, "%s", val),
            callsign_upper(cfg->flex_listen_call);
        else if (!strcasecmp(key, "Role")) {
            if (!strcasecmp(val, "server") ||
                !strcasecmp(val, "initiator")) cfg->role = 1;
            else                               cfg->role = 0;
        }
        else if (!strcasecmp(key, "PollInterval"))
            cfg->poll_interval = atoi(val);
        else if (!strcasecmp(key, "KeepaliveInterval"))
            cfg->keepalive_interval = atoi(val);
        else if (!strcasecmp(key, "BeaconInterval"))
            cfg->beacon_interval = atoi(val);
        else if (!strcasecmp(key, "TriggerThreshold"))
            cfg->trigger_threshold = atoi(val);
        else if (!strcasecmp(key, "Infinity"))
            cfg->infinity = atoi(val);
        else if (!strcasecmp(key, "GatewaysFile"))
            snprintf(cfg->gateways_file, MAX_PATH_LEN, "%s", val);
        else if (!strcasecmp(key, "DestFile"))
            snprintf(cfg->dest_file, MAX_PATH_LEN, "%s", val);
        else if (!strcasecmp(key, "LinkStatsFile"))
            snprintf(cfg->linkstats_file, MAX_PATH_LEN, "%s", val);
        else if (!strcasecmp(key, "PathsFile"))
            snprintf(cfg->paths_file, MAX_PATH_LEN, "%s", val);
        else if (!strcasecmp(key, "LogLevel"))
            cfg->log_level = atoi(val);
        else if (!strcasecmp(key, "Syslog"))
            cfg->use_syslog = (!strcasecmp(val, "yes") || !strcmp(val, "1"));
        else if (!strcasecmp(key, "ProbeCount"))
            cfg->probe_count = atoi(val);
        else if (!strcasecmp(key, "PathProbeInterval"))
            cfg->path_probe_interval = atoi(val);
        else if (!strcasecmp(key, "RouteAdvertInterval"))
            cfg->route_advert_interval = atoi(val);
        else if (!strcasecmp(key, "LinkTimeReplyInterval"))
            cfg->lt_reply_interval = atoi(val);
        else if (!strcasecmp(key, "Port")) {
            /* M6 multi-port syntax:
             *   Port <name> <neighbor> <listen_call> [opt=val]...
             * e.g.
             *   Port xnet  IW2OHX-14  IW2OHX-3  route_advert=60  lt_reply=20
             *   Port pcf   IW2OHX-12  IW2OHX-3  route_advert=0   lt_reply=320
             *
             * Optional trailing tokens override global settings for THIS
             * port only.  Recognised options:
             *   route_advert=<sec>   periodic route re-advert interval
             *                        (xnet tolerates ≥ 60; PCFlexnet
             *                        must stay at 0 or it DMs the link)
             *   lt_reply=<sec>       link-time reply interval
             *                        (xnet wants ≤ 20 for smoothed RTT
             *                        convergence; PCFlexnet needs 320
             *                        to match its ts_ahead window)
             *
             * Bare integer in the 4th position is interpreted as
             * route_advert=<N> for back-compat.  Unknown options are
             * logged and ignored.  Use -1 (or omit) to mean "follow
             * the global default".
             */
            if (cfg->num_ports >= MAX_PORTS) {
                LOG_WRN("config[%d]: Port table full (MAX_PORTS=%d), "
                        "ignoring: Port %s", lineno, MAX_PORTS, val);
                continue;
            }
            char pname[MAX_IFACE_LEN] = {0};
            char nbr[MAX_CALLSIGN_LEN] = {0};
            char lcall[MAX_CALLSIGN_LEN] = {0};
            int fields = sscanf(val, "%7s %9s %9s", pname, nbr, lcall);
            if (fields < 3) {
                LOG_WRN("config[%d]: Port needs at least 3 fields "
                        "(<name> <neighbor> <listen_call>): '%s'",
                        lineno, val);
                continue;
            }
            PortCfg *pc = &cfg->ports[cfg->num_ports++];
            snprintf(pc->name,        sizeof(pc->name),        "%s", pname);
            snprintf(pc->neighbor,    sizeof(pc->neighbor),    "%s", nbr);
            snprintf(pc->listen_call, sizeof(pc->listen_call), "%s", lcall);
            callsign_upper(pc->neighbor);
            callsign_upper(pc->listen_call);
            pc->min_ssid = cfg->min_ssid;
            pc->max_ssid = cfg->max_ssid;
            pc->route_advert_interval = -1;   /* inherit global */
            pc->lt_reply_interval     = -1;   /* inherit global */
            pc->advert_mode           = -1;   /* -1 = default (FULL) */

            /* Walk any remaining tokens (opt=val or bare first-position
             * integer for back-compat). */
            const char *p = val;
            /* skip past the three already-parsed tokens */
            for (int tok = 0; tok < 3 && *p; tok++) {
                while (*p == ' ' || *p == '\t') p++;
                while (*p && *p != ' ' && *p != '\t') p++;
            }
            int opt_pos = 0;
            while (*p) {
                while (*p == ' ' || *p == '\t') p++;
                if (!*p) break;
                char optbuf[64] = {0};
                int j = 0;
                while (*p && *p != ' ' && *p != '\t' && j < (int)sizeof(optbuf) - 1)
                    optbuf[j++] = *p++;
                optbuf[j] = '\0';
                if (!optbuf[0]) break;
                opt_pos++;

                if (!strncasecmp(optbuf, "route_advert=", 13)) {
                    pc->route_advert_interval = atoi(optbuf + 13);
                    LOG_INF("config[%d]: port '%s' route_advert = %d s",
                            lineno, pname, pc->route_advert_interval);
                } else if (!strncasecmp(optbuf, "lt_reply=", 9)) {
                    pc->lt_reply_interval = atoi(optbuf + 9);
                    LOG_INF("config[%d]: port '%s' lt_reply = %d s",
                            lineno, pname, pc->lt_reply_interval);
                } else if (!strncasecmp(optbuf, "advert_mode=", 12)) {
                    const char *v = optbuf + 12;
                    if (!strcasecmp(v, "full"))
                        pc->advert_mode = ADVERT_MODE_FULL;
                    else if (!strcasecmp(v, "record"))
                        pc->advert_mode = ADVERT_MODE_RECORD;
                    else
                        pc->advert_mode = atoi(v);
                    LOG_INF("config[%d]: port '%s' advert_mode = %s",
                            lineno, pname,
                            pc->advert_mode == ADVERT_MODE_FULL   ? "full"   :
                            pc->advert_mode == ADVERT_MODE_RECORD ? "record" :
                            "default");
                } else if (opt_pos == 1 &&
                           (optbuf[0] == '-' || (optbuf[0] >= '0' && optbuf[0] <= '9'))) {
                    /* back-compat: bare integer as 4th field = route_advert */
                    pc->route_advert_interval = atoi(optbuf);
                    LOG_INF("config[%d]: port '%s' route_advert = %d s "
                            "(bare-integer form)",
                            lineno, pname, pc->route_advert_interval);
                } else {
                    LOG_WRN("config[%d]: port '%s' unknown option '%s' "
                            "(ignored)", lineno, pname, optbuf);
                }
            }
        }
        else
            LOG_WRN("config[%d]: unknown keyword '%s'", lineno, key);
    }

    fclose(f);

    /* ── M6 backward compatibility ─────────────────────────────────
     * If the config used only the legacy flat keywords (Neighbor,
     * PortName, FlexListenCall) and had no 'Port' line, synthesise a
     * single ports[0] entry from them.  Otherwise, copy ports[0] back
     * into the legacy fields so existing code that still reads them
     * (e.g. output.c, legacy call sites) keeps working unchanged.
     */
    if (cfg->num_ports == 0) {
        /* legacy-only config → synthesize ports[0] */
        PortCfg *pc = &cfg->ports[0];
        snprintf(pc->name,        sizeof(pc->name),        "%s", cfg->port_name);
        snprintf(pc->neighbor,    sizeof(pc->neighbor),    "%s", cfg->neighbor);
        snprintf(pc->listen_call, sizeof(pc->listen_call), "%s", cfg->flex_listen_call);
        pc->min_ssid = cfg->min_ssid;
        pc->max_ssid = cfg->max_ssid;
        pc->route_advert_interval = -1;   /* inherit global */
        pc->lt_reply_interval     = -1;   /* inherit global */
        pc->advert_mode           = -1;   /* default FULL */
        cfg->num_ports = 1;
        LOG_DBG("config: synthesised ports[0] from legacy flat keywords");
    } else {
        /* Port-block config → mirror ports[0] into legacy fields so
         * code that still reads cfg->neighbor / cfg->port_name /
         * cfg->flex_listen_call continues to see the primary port. */
        const PortCfg *pc = &cfg->ports[0];
        snprintf(cfg->neighbor,         sizeof(cfg->neighbor),         "%s", pc->neighbor);
        snprintf(cfg->port_name,        sizeof(cfg->port_name),        "%s", pc->name);
        snprintf(cfg->flex_listen_call, sizeof(cfg->flex_listen_call), "%s", pc->listen_call);
        LOG_DBG("config: mirrored ports[0] → legacy flat fields");
    }

    return 0;
}

void config_dump(const FlexConfig *cfg)
{
    LOG_INF("=== flexnetd configuration ===");
    LOG_INF("  MyCall            : %s", cfg->mycall);
    LOG_INF("  Alias             : %s", cfg->alias);
    LOG_INF("  SSID range        : %d-%d", cfg->min_ssid, cfg->max_ssid);
    LOG_INF("  Role              : %s", cfg->role ? "server (native)" : "client (D-cmd)");
    LOG_INF("  Ports             : %d configured", cfg->num_ports);
    for (int i = 0; i < cfg->num_ports; i++) {
        const PortCfg *pc = &cfg->ports[i];
        int eff_ra = (pc->route_advert_interval >= 0)
                     ? pc->route_advert_interval
                     : cfg->route_advert_interval;
        int eff_lt = (pc->lt_reply_interval >= 0)
                     ? pc->lt_reply_interval
                     : cfg->lt_reply_interval;
        int eff_am = (pc->advert_mode >= 0)
                     ? pc->advert_mode
                     : ADVERT_MODE_FULL;   /* default: xnet-friendly */
        LOG_INF("    [%d] %-8s  neighbor=%-9s  listen=%-9s  ssid=%d-%d  "
                "route_advert=%ds%s  lt_reply=%ds%s  advert_mode=%s%s",
                i, pc->name, pc->neighbor, pc->listen_call,
                pc->min_ssid, pc->max_ssid,
                eff_ra,
                pc->route_advert_interval >= 0 ? " (port)" : " (global)",
                eff_lt,
                pc->lt_reply_interval     >= 0 ? " (port)" : " (global)",
                eff_am == ADVERT_MODE_FULL ? "full" : "record",
                pc->advert_mode >= 0 ? " (port)" : " (default)");
    }
    LOG_INF("  PollInterval      : %d s", cfg->poll_interval);
    LOG_INF("  KeepaliveInterval : %d s", cfg->keepalive_interval);
    LOG_INF("  BeaconInterval    : %d s", cfg->beacon_interval);
    LOG_INF("  TriggerThreshold  : %d", cfg->trigger_threshold);
    LOG_INF("  Infinity          : %d", cfg->infinity);
    LOG_INF("  GatewaysFile      : %s", cfg->gateways_file);
    LOG_INF("  DestFile          : %s", cfg->dest_file);
    LOG_INF("  LinkStatsFile     : %s", cfg->linkstats_file);
    LOG_INF("  PathsFile         : %s", cfg->paths_file);
    LOG_INF("  PathProbeInterval : %d s", cfg->path_probe_interval);
    LOG_INF("  RouteAdvertInterval: %d s%s", cfg->route_advert_interval,
            cfg->route_advert_interval <= 0 ? " (once only)" : "");
    LOG_INF("  LinkTimeReplyInterval: %d s%s", cfg->lt_reply_interval,
            cfg->lt_reply_interval <= 0 ? " (no rate limit)" : "");
    LOG_INF("  LogLevel          : %d", cfg->log_level);
    LOG_INF("  Syslog            : %s", cfg->use_syslog ? "yes" : "no");
    LOG_INF("==============================");
}
