/*
 * flexnetd.h — shared types, constants and declarations
 *
 * FlexNet routing daemon for URONode / LinBPQ
 * Author: IW2OHX, April 2026
 * License: GPL v3
 */

#ifndef FLEXNETD_H
#define FLEXNETD_H

#include <stdint.h>
#include <time.h>

#ifdef LOG_ERR
#  undef LOG_ERR
#endif
#ifdef LOG_WARNING
#  undef LOG_WARNING
#endif
#ifdef LOG_INFO
#  undef LOG_INFO
#endif
#ifdef LOG_DEBUG
#  undef LOG_DEBUG
#endif

/* ── Version ──────────────────────────────────────────────────────────── */
#define FLEXNETD_VERSION        "0.5.0"

/* ── Protocol constants ───────────────────────────────────────────────── */
#define PID_CF                  0xCF
#define PID_CE                  0xCE
#define PID_F0                  0xF0

#define RTT_INFINITY            60000
#define RTT_UNIT_MS             100
#define COUNTER_TICK_MS         10
#define CE_KEEPALIVE_LEN        241
#define DEFAULT_POLL_INTERVAL   240
#define DEFAULT_KEEPALIVE_S     90
#define DEFAULT_BEACON_S        120
#define DEFAULT_PROBE_COUNT     19
#define DEFAULT_PROBE_INTERVAL_MS 7500
#define DEFAULT_TRIGGER_THRESH  50
#define AX25_MAX_WINDOW         7
#define AX25_RETRIES            10
#define LEVEL3_VERSION_STR      "(X)NET139"

#define MAX_CALLSIGN_LEN        10
#define MAX_ALIAS_LEN           8
#define MAX_IFACE_LEN           8
#define MAX_PATH_LEN            256
#define MAX_DEST_ENTRIES        2000
#define MAX_SSID                15

/* ── CE frame parse return codes ─────────────────────────────────────── */
#define CE_FRAME_KEEPALIVE      1   /* '2' + 240 pad chars = 241 bytes   */
#define CE_FRAME_STATUS_POS     2   /* '3+\r' — positive ack             */
#define CE_FRAME_STATUS_NEG     3   /* '3-\r' — negative / withdrawal    */
#define CE_FRAME_STATUS_10      4   /* '10\r'                            */
#define CE_FRAME_COMPACT        5   /* '3' prefix — compact route record */
#define CE_FRAME_LINK_TIME      6   /* '1' prefix — link time (ms)       */
#define CE_FRAME_TOKEN          7   /* '4' prefix — token/sequence       */
#define CE_FRAME_DEST_BCAST     8   /* '6' prefix — destination broadcast*/
#define CE_FRAME_INIT           9   /* '0' prefix — initial handshake    */

/* ── Log levels ───────────────────────────────────────────────────────── */
#define LOG_LEVEL_NONE          0
#define LOG_LEVEL_ERROR         1
#define LOG_LEVEL_WARN          2
#define LOG_LEVEL_INFO          3
#define LOG_LEVEL_DEBUG         4

extern int   g_log_level;
extern int   g_use_syslog;
extern FILE *g_log_file;

/* ── Logging macros ───────────────────────────────────────────────────── */
#define FLOG_ERR(fmt, ...)  flexnetd_log(LOG_LEVEL_ERROR, "ERROR", fmt, ##__VA_ARGS__)
#define FLOG_WRN(fmt, ...)  flexnetd_log(LOG_LEVEL_WARN,  "WARN ", fmt, ##__VA_ARGS__)
#define FLOG_INF(fmt, ...)  flexnetd_log(LOG_LEVEL_INFO,  "INFO ", fmt, ##__VA_ARGS__)
#define FLOG_DBG(fmt, ...)  flexnetd_log(LOG_LEVEL_DEBUG, "DEBUG", fmt, ##__VA_ARGS__)

#define LOG_ERR   FLOG_ERR
#define LOG_WRN   FLOG_WRN
#define LOG_INF   FLOG_INF
#define LOG_DBG   FLOG_DBG

void flexnetd_log(int level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* ── Configuration ───────────────────────────────────────────────────── */
typedef struct {
    char    mycall[MAX_CALLSIGN_LEN];
    char    alias[MAX_ALIAS_LEN];
    int     min_ssid;
    int     max_ssid;
    char    neighbor[MAX_CALLSIGN_LEN];
    char    port_name[MAX_IFACE_LEN];           /* axports port name */
    char    flex_listen_call[MAX_CALLSIGN_LEN]; /* e.g. "IW2OHX-9", NOT in ax25d.conf */
    int     role;                               /* 0=client (D-cmd), 1=server (native) */
    int     poll_interval;
    int     keepalive_interval;
    int     beacon_interval;
    int     trigger_threshold;
    int     infinity;
    char    gateways_file[MAX_PATH_LEN];
    char    dest_file[MAX_PATH_LEN];
    char    linkstats_file[MAX_PATH_LEN];
    int     log_level;
    int     use_syslog;
    int     probe_count;
} FlexConfig;

extern FlexConfig g_cfg;

/* ── Destination table entry ─────────────────────────────────────────── */
typedef struct {
    char    callsign[MAX_CALLSIGN_LEN];
    int     ssid_lo;
    int     ssid_hi;
    int     rtt;
    int     port;
    int     node_type;
    char    node_alias[MAX_ALIAS_LEN];
    char    via_callsign[MAX_CALLSIGN_LEN]; /* next-hop from CE type-6 */
    char    quality_flag;                   /* 'U'pdates/'A'ccepting/'T'oken */
    int     is_infinity;
    time_t  last_updated;
} DestEntry;

typedef struct {
    DestEntry   entries[MAX_DEST_ENTRIES];
    int         count;
    time_t      last_full_update;
} DestTable;

extern DestTable g_table;

/* ── Link statistics (for L-table display) ──────────────────────────── */
typedef struct {
    char    neighbor[MAX_CALLSIGN_LEN];
    int     port_num;               /* port index from config */
    time_t  connect_time;           /* session start timestamp */
    int     qt;                     /* Q/T quality value (from CE link time) */
    int     rtt_last;               /* last RTT in 100ms ticks */
    int     rtt_smoothed;           /* smoothed RTT */
    long    tx_bytes;               /* cumulative bytes sent */
    long    rx_bytes;               /* cumulative bytes received */
    int     tx_frames;              /* frames sent */
    int     rx_frames;              /* frames received */
    int     keepalive_count;        /* keepalive exchanges */
    int     dst_count;              /* reachable destinations */
    int     active;                 /* 1 = session is live */
} LinkStats;

extern LinkStats g_link_stats;

/* ── Function declarations ───────────────────────────────────────────── */
/* flexnetd_log declared above with __attribute__((format)) */

int  config_load(const char *path, FlexConfig *cfg);
void config_dump(const FlexConfig *cfg);

void        callsign_upper(char *call);
int         callsign_parse_ssid(const char *call, char *base_out, int *ssid_out);
const char *rtt_str(int rtt);
void        hex_dump(const char *label, const uint8_t *data, int len);

/* ax25 socket operations */
int  ax25_enable_pidincl(int fd);
int  ax25_tune_interface(const char *port_name);
int  ax25_get_ifname(const char *port_name, char *iface_out, int buflen);
int  ax25_connect(const char *mycall, const char *neighbor,
                  const char *port_name);
int  ax25_listen(const char *listen_call, const char *port_name);
int  ax25_accept(int listen_fd, char *peer_call, int peer_buflen);
void ax25_disconnect(int fd);
int  ax25_send(int fd, uint8_t pid, const uint8_t *data, int len);
int  ax25_recv(int fd, uint8_t *pid_out, uint8_t *buf, int buflen,
               int timeout_ms);

int      cf_build_l3rtt(uint8_t *buf, int buflen,
                        uint32_t counter1, uint32_t counter2,
                        uint32_t counter3, uint32_t counter4,
                        const char *alias, const char *version,
                        uint32_t max_dest);
int      cf_parse_l3rtt(const uint8_t *data, int len,
                        uint32_t *counter1_out, uint32_t *counter2_out,
                        uint32_t *counter3_out, uint32_t *counter4_out,
                        int *lt_out,
                        char *alias_out, char *version_out,
                        uint32_t *max_dest_out);
int      cf_parse_dtable_line(const char *line, DestEntry *entry_out);
uint32_t get_uptime_ticks(void);

int ce_build_link_setup(uint8_t *buf, int buflen, int min_ssid, int max_ssid);
int ce_build_keepalive(uint8_t *buf, int buflen);
int ce_build_record(uint8_t *buf, int buflen,
                    const char *callsign, int ssid_lo, int ssid_hi,
                    int rtt, int indirect);
int ce_build_link_time(uint8_t *buf, int buflen, long link_time_ms);
int ce_build_token(uint8_t *buf, int buflen, int token_val, char flag);
int ce_build_dest_broadcast(uint8_t *buf, int buflen,
                            int rtt, const char *callsign,
                            const char *via_callsign);
int ce_parse_frame(const uint8_t *data, int len,
                   char *callsign_out, int *ssid_out, int *rtt_out);
int ce_parse_compact_records(const uint8_t *data, int len,
                             DestEntry *out, int max_entries);
int ce_parse_dest_broadcast(const uint8_t *data, int len,
                            int *rtt_out, char *callsign_out,
                            int *ssid_lo_out, int *ssid_hi_out,
                            char *via_callsign_out, char *flag_out);

void dtable_init(void);
int  dtable_find(const char *callsign, int ssid_lo, int ssid_hi);
int  dtable_merge(const DestEntry *incoming);
int  dtable_load_from_text(const char *text, int gw_idx);
void dtable_dump(void);
int  dtable_count_reachable(void);
void dtable_sort(void);

int output_write_gateways(void);
int output_write_destinations(void);
int output_write_linkstats(void);

int poll_cycle_run(int fd);
int poll_cycle_run_mode(int fd, int mode);
int send_ce_keepalive(int fd);

#endif /* FLEXNETD_H */
