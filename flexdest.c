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
 *   flexdest -r IR5S      — also show route path (from path cache)
 *
 * Reads:
 *   /usr/local/var/lib/ax25/flex/destinations (-f <file> to override)
 *   /usr/local/var/lib/ax25/flex/paths        (-p <file> to override)
 *
 * Author: IW2OHX, April 2026
 * License: GPL v3
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#define DEFAULT_DEST_FILE   "/usr/local/var/lib/ax25/flex/destinations"
#define DEFAULT_PATHS_FILE  "/usr/local/var/lib/ax25/flex/paths"
#define MAX_LINE            1024
#define MAX_CALL            10
#define MAX_ENTRIES         2000
#define MAX_PATH_HOPS       16

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
        "Usage: %s [-f file] [-p file] [-r] [pattern]\n"
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
        "  -f file    destinations file (default: %s)\n"
        "  -p file    path cache file   (default: %s)\n"
        "  -r         show route path after each matched destination\n"
        "             (reads the path cache populated by flexnetd from\n"
        "              CE type-7 replies)\n",
        prog, DEFAULT_DEST_FILE, DEFAULT_PATHS_FILE);
}

/*
 * Path cache entry — one line from the paths file.
 * File format (space-separated, # comment):
 *   <target> <kind_char> <n_hops> <unix_ts> <hop1> <hop2> ... <hopN>
 * kind_char: 'R' = Route, 'T' = Traceroute
 */
typedef struct {
    char  target[MAX_CALL];
    char  kind;
    int   n_hops;
    long  cached_ts;
    char  hops[MAX_PATH_HOPS][MAX_CALL];
} PathCache;

static PathCache g_cache[MAX_ENTRIES];
static int       g_cache_count = 0;

/* Load path cache file.  Returns count of entries loaded, or -1 on error. */
static int load_paths_cache(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;  /* silent — cache may not exist yet */

    char line[MAX_LINE];
    g_cache_count = 0;
    while (fgets(line, sizeof(line), f) && g_cache_count < MAX_ENTRIES) {
        /* trim newline */
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        PathCache *e = &g_cache[g_cache_count];
        memset(e, 0, sizeof(*e));

        /* scanf up to the fixed header: target kind_char n_hops ts */
        char kind = '?';
        int  n_hops = 0;
        long ts = 0;
        int  consumed = 0;
        if (sscanf(line, "%9s %c %d %ld %n",
                   e->target, &kind, &n_hops, &ts, &consumed) < 4) {
            continue;
        }
        str_upper(e->target);
        e->kind      = kind;
        e->n_hops    = n_hops;
        e->cached_ts = ts;

        /* remaining tokens are the hops */
        int hi = 0;
        char *p = line + consumed;
        char *tok = strtok(p, " \t");
        while (tok && hi < MAX_PATH_HOPS && hi < n_hops) {
            snprintf(e->hops[hi++], MAX_CALL, "%s", tok);
            tok = strtok(NULL, " \t");
        }
        e->n_hops = hi;
        g_cache_count++;
    }
    fclose(f);
    return g_cache_count;
}

/* Find the cached path for a given destination callsign.
 * Returns pointer into g_cache, or NULL if not found. */
static const PathCache *find_cached_path(const char *dest_call)
{
    if (!dest_call || !dest_call[0]) return NULL;
    for (int i = 0; i < g_cache_count; i++) {
        if (strcasecmp(g_cache[i].target, dest_call) == 0)
            return &g_cache[i];
        /* also match if the cached target has an SSID that matches
         * the query (e.g. cache has "IR5S-3", query was "IR5S") */
        char base[MAX_CALL];
        snprintf(base, sizeof(base), "%s", g_cache[i].target);
        char *dash = strchr(base, '-');
        if (dash) {
            *dash = '\0';
            if (strcasecmp(base, dest_call) == 0)
                return &g_cache[i];
        }
    }
    return NULL;
}

/* Render human-friendly age for cache entry */
static void fmt_age(long seconds, char *buf, int buflen)
{
    if (seconds < 60)
        snprintf(buf, buflen, "%lds", seconds);
    else if (seconds < 3600)
        snprintf(buf, buflen, "%ldm%02lds", seconds / 60, seconds % 60);
    else
        snprintf(buf, buflen, "%ldh%02ldm", seconds / 3600,
                 (seconds % 3600) / 60);
}

int main(int argc, char *argv[])
{
    const char *dest_file  = DEFAULT_DEST_FILE;
    const char *paths_file = DEFAULT_PATHS_FILE;
    const char *pattern    = NULL;
    int         show_route = 0;

    /* parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            dest_file = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            paths_file = argv[++i];
        } else if (strcmp(argv[i], "-r") == 0 ||
                   strcmp(argv[i], "--route") == 0) {
            show_route = 1;
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

    /* Optional: load path cache (silently tolerate missing file) */
    if (show_route) {
        load_paths_cache(paths_file);
    }

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

    time_t now = time(NULL);
    for (int i = 0; i < count; i++) {
        DestEntry *e = &matches[i];
        char ssid_range[12];
        snprintf(ssid_range, sizeof(ssid_range), "%d-%d",
                 e->ssid_lo, e->ssid_hi);
        printf("%-9s %-5s %5d %-9s\n",
               e->callsign, ssid_range, e->rtt,
               e->via[0] ? e->via : "(direct)");

        /* Optional: route line from the path cache */
        if (show_route) {
            const PathCache *pc = find_cached_path(e->callsign);
            if (pc && pc->n_hops > 0) {
                char age_buf[16];
                fmt_age(now - pc->cached_ts, age_buf, sizeof(age_buf));
                printf("*** route:");
                for (int k = 0; k < pc->n_hops; k++)
                    printf(" %s", pc->hops[k]);
                printf("   [%s %s ago]\n",
                       pc->kind == 'T' ? "trace" : "route",
                       age_buf);
            } else {
                /* no cached full path — fall back to partial route
                 * from destinations file (our_call unknown, so show
                 * what we know: via_callsign -> destination) */
                if (e->via[0])
                    printf("*** route: ... %s %s   [no cache; run a "
                           "route query to populate]\n",
                           e->via, e->callsign);
                else
                    printf("*** route: %s   [no cache]\n", e->callsign);
            }
        }
    }

    printf("\n%d destination%s\n", count, count == 1 ? "" : "s");
    return 0;
}
