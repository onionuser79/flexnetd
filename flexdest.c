/*
 * flexdest.c — FlexNet destination query tool
 *
 * Standalone tool that reads the flexnetd destinations file and
 * supports pattern matching: exact, prefix wildcard, SSID-specific.
 *
 * Usage:
 *   flexdest              — list all destinations
 *   flexdest IR5S         — exact callsign match
 *   flexdest IW*          — prefix wildcard (all starting with IW)
 *   flexdest IR5S-7       — SSID-specific (entries containing SSID 7)
 *
 * Reads: /usr/local/var/lib/ax25/flex/destinations (or -f <file>)
 *
 * Author: IW2OHX, April 2026
 * License: GPL v3
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define DEFAULT_DEST_FILE  "/usr/local/var/lib/ax25/flex/destinations"
#define MAX_LINE           256
#define MAX_CALL           10
#define MAX_ENTRIES        2000

typedef struct {
    char  callsign[MAX_CALL];
    int   ssid_lo;
    int   ssid_hi;
    int   rtt;
    char  via[MAX_CALL];
} DestEntry;

/* uppercase a string in-place */
static void str_upper(char *s)
{
    for (; *s; s++)
        *s = (char)toupper((unsigned char)*s);
}

/*
 * parse_dest_line — parse one line from the destinations file.
 * Expected format: "%-9s %-5s %5d %-9s"
 *   e.g. "IR5S      0-15      4 IW2OHX-14"
 * Returns 0 on success, -1 on skip (header or invalid).
 */
static int parse_dest_line(const char *line, DestEntry *e)
{
    /* skip header line */
    if (strncmp(line, "Dest", 4) == 0) return -1;
    if (strncmp(line, "callsign", 8) == 0) return -1;  /* legacy header */
    if (strncmp(line, "--------", 8) == 0) return -1;
    if (line[0] == '\0' || line[0] == '\n') return -1;

    /* tokenize: callsign  ssid_range  rtt  via */
    char call[MAX_CALL] = {0};
    char ssid_str[16] = {0};
    int  rtt = 0;
    char via[MAX_CALL] = {0};

    int n = sscanf(line, "%9s %15s %d %9s", call, ssid_str, &rtt, via);
    if (n < 3) return -1;

    /* parse ssid range "LO-HI" */
    int lo = 0, hi = 0;
    char *dash = strchr(ssid_str, '-');
    if (dash) {
        lo = atoi(ssid_str);
        hi = atoi(dash + 1);
    } else {
        lo = hi = atoi(ssid_str);
    }

    str_upper(call);
    snprintf(e->callsign, MAX_CALL, "%s", call);
    e->ssid_lo = lo;
    e->ssid_hi = hi;
    e->rtt     = rtt;
    if (n >= 4)
        snprintf(e->via, MAX_CALL, "%s", via);
    else
        e->via[0] = '\0';

    return 0;
}

/*
 * match_pattern — check if a destination entry matches the query.
 *
 * Pattern types:
 *   "IR5S"    exact callsign match (case-insensitive)
 *   "IW*"     prefix wildcard
 *   "IR5S-7"  exact callsign + SSID 7 must be in ssid_lo..ssid_hi range
 */
static int match_pattern(const DestEntry *e, const char *pattern)
{
    if (!pattern || !pattern[0])
        return 1;  /* no pattern = match all */

    char pat[MAX_CALL + 4];
    snprintf(pat, sizeof(pat), "%s", pattern);
    str_upper(pat);

    /* wildcard: "IW*" → prefix match */
    char *star = strchr(pat, '*');
    if (star) {
        int prefix_len = (int)(star - pat);
        return (strncmp(e->callsign, pat, (size_t)prefix_len) == 0);
    }

    /* SSID-specific: "IR5S-7" → exact call + SSID in range */
    char *dash = strchr(pat, '-');
    if (dash && isdigit((unsigned char)*(dash + 1))) {
        *dash = '\0';
        int ssid = atoi(dash + 1);
        return (strcmp(e->callsign, pat) == 0 &&
                ssid >= e->ssid_lo && ssid <= e->ssid_hi);
    }

    /* exact match */
    return (strcmp(e->callsign, pat) == 0);
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-f file] [pattern]\n"
        "\n"
        "Query FlexNet destinations with pattern matching.\n"
        "\n"
        "Patterns:\n"
        "  IR5S       exact callsign match\n"
        "  IW*        prefix wildcard (all starting with IW)\n"
        "  IR5S-7     SSID-specific (entries containing SSID 7)\n"
        "  (none)     list all destinations\n"
        "\n"
        "Options:\n"
        "  -f file    destinations file (default: %s)\n",
        prog, DEFAULT_DEST_FILE);
}

int main(int argc, char *argv[])
{
    const char *dest_file = DEFAULT_DEST_FILE;
    const char *pattern   = NULL;

    /* parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            dest_file = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            pattern = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    FILE *f = fopen(dest_file, "r");
    if (!f) {
        fprintf(stderr, "flexdest: cannot open '%s': ", dest_file);
        perror(NULL);
        return 1;
    }

    /* read and filter entries */
    DestEntry matches[MAX_ENTRIES];
    int       count = 0;
    char      line[MAX_LINE];

    while (fgets(line, sizeof(line), f) && count < MAX_ENTRIES) {
        DestEntry e;
        if (parse_dest_line(line, &e) < 0) continue;
        if (match_pattern(&e, pattern))
            matches[count++] = e;
    }
    fclose(f);

    if (count == 0) {
        if (pattern)
            printf("FlexNet: no destinations matching '%s'\n", pattern);
        else
            printf("FlexNet: no destinations\n");
        return 0;
    }

    /* header */
    if (pattern) {
        char pat_upper[MAX_CALL + 4];
        snprintf(pat_upper, sizeof(pat_upper), "%s", pattern);
        str_upper(pat_upper);

        if (strchr(pat_upper, '*') || count > 1)
            printf("FlexNet Destinations matching %s:\n", pat_upper);
        else
            printf("FlexNet Destination %s:\n", pat_upper);
    } else {
        printf("FlexNet Destinations:\n");
    }

    printf("Dest     SSID    RTT Via\n");

    for (int i = 0; i < count; i++) {
        DestEntry *e = &matches[i];
        char ssid_range[12];
        snprintf(ssid_range, sizeof(ssid_range), "%d-%d",
                 e->ssid_lo, e->ssid_hi);
        printf("%-9s %-5s %5d %-9s\n",
               e->callsign, ssid_range, e->rtt,
               e->via[0] ? e->via : "(direct)");
    }

    printf("\n%d destination%s\n", count, count == 1 ? "" : "s");
    return 0;
}
