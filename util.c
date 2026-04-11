/*
 * util.c — callsign utilities and misc helpers
 */

#include <stdio.h>
#include <stdlib.h>     /* atoi, isprint via ctype */
#include <string.h>
#include <ctype.h>
#include "flexnetd.h"

/* uppercase a callsign in-place */
void callsign_upper(char *call)
{
    for (; *call; call++)
        *call = (char)toupper((unsigned char)*call);
}

/*
 * callsign_parse_ssid — split "IW2OHX-7" into base="IW2OHX" ssid=7
 * Returns 0 on success, -1 on error.
 */
int callsign_parse_ssid(const char *call, char *base_out, int *ssid_out)
{
    const char *dash = strrchr(call, '-');
    if (dash && isdigit((unsigned char)*(dash + 1))) {
        int len = (int)(dash - call);
        if (len <= 0 || len >= MAX_CALLSIGN_LEN) return -1;
        strncpy(base_out, call, (size_t)len);
        base_out[len] = '\0';
        *ssid_out = atoi(dash + 1);
    } else {
        strncpy(base_out, call, MAX_CALLSIGN_LEN - 1);
        base_out[MAX_CALLSIGN_LEN - 1] = '\0';
        *ssid_out = 0;
    }
    return 0;
}

/* human-readable RTT string for debug output */
const char *rtt_str(int rtt)
{
    static char buf[32];
    if (rtt >= RTT_INFINITY)
        snprintf(buf, sizeof(buf), "INF(60000)");
    else
        snprintf(buf, sizeof(buf), "%d(%.1fs)", rtt, rtt / 10.0);
    return buf;
}

/*
 * hex_dump — debug helper: dump a byte buffer in hex + ASCII.
 * Only prints when log level >= DEBUG.
 */
void hex_dump(const char *label, const uint8_t *data, int len)
{
    if (g_log_level < LOG_LEVEL_DEBUG) return;

    FLOG_DBG("%s [%d bytes]:", label, len);

    char hex[49];
    char asc[17];
    int  i;

    for (i = 0; i < len; i++) {
        int col = i % 16;
        snprintf(hex + col * 3, 4, "%02X ", data[i]);
        asc[col] = isprint((unsigned char)data[i]) ? (char)data[i] : '.';

        if (col == 15 || i == len - 1) {
            asc[col + 1] = '\0';
            /* pad the last incomplete line */
            int pad = col;
            while (pad < 15) {
                hex[(pad + 1) * 3 + 0] = ' ';
                hex[(pad + 1) * 3 + 1] = ' ';
                hex[(pad + 1) * 3 + 2] = ' ';
                pad++;
            }
            hex[48] = '\0';
            FLOG_DBG("  %04X  %s |%s|", i - (i % 16), hex, asc);
        }
    }
}
