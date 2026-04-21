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
#define FLEXNETD_VERSION        "0.7.2"

/* ── Protocol constants ───────────────────────────────────────────────── */
#define PID_CF                  0xCF
#define PID_CE                  0xCE
#define PID_F0                  0xF0

#define RTT_INFINITY            60000
#define RTT_UNIT_MS             100
#define COUNTER_TICK_MS         10
#define CE_KEEPALIVE_LEN        241
#define DEFAULT_POLL_INTERVAL   240
/*
 * DEFAULT_KEEPALIVE_S — per PROTOCOL_SPEC.md §6, the keepalive period
 * is 180 seconds.  Earlier flexnetd versions used 90 s (pre-spec
 * guess) which meant we emitted 2× faster than the reference —
 * harmless but wasteful on RF.  v0.7.2: align with the spec exactly.
 */
#define DEFAULT_KEEPALIVE_S     180
#define DEFAULT_BEACON_S        120
/*
 * DEFAULT_ROUTE_ADVERT_S — global default for M6.7 periodic
 * re-advertisement (only used when a Port block doesn't override it).
 *
 * Set to 0 (disabled) as of v0.7.1 after observing that PCFlexnet
 * (IW2OHX-12) sends L2 DM within 10-15 ms of receiving any unsolicited
 * compact record.  After processing a compact record PCFlexnet checks
 * its internal token state; when state is 0 (idle) it tears down the
 * L2 link.
 *
 * (X)Net tolerates M6.7 — set RouteAdvertInterval=60 on its port block
 * to re-enable periodic re-advertisement there.
 */
#define DEFAULT_ROUTE_ADVERT_S  0   /* 0 = disabled globally; opt-in per port */
#define DEFAULT_LT_REPLY_S      90   /* M6.9: min seconds between link-time frames to a peer */
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
#define MAX_PORTS               4   /* M6: up to 4 FlexNet ports */

/* ── CE frame parse return codes ─────────────────────────────────────── */
#define CE_FRAME_KEEPALIVE      1   /* '2' + 240 pad chars = 241 bytes   */
#define CE_FRAME_STATUS_POS     2   /* '3+\r' — positive ack             */
#define CE_FRAME_STATUS_NEG     3   /* '3-\r' — negative / withdrawal    */
#define CE_FRAME_STATUS_10      4   /* '10\r'                            */
#define CE_FRAME_COMPACT        5   /* '3' prefix — compact route record */
#define CE_FRAME_LINK_TIME      6   /* '1' prefix — link time (ms)       */
#define CE_FRAME_TOKEN          7   /* '4' prefix — token/sequence       */
#define CE_FRAME_DEST_BCAST     8   /* '6' prefix — legacy (deprecated)  */
#define CE_FRAME_INIT           9   /* '0' prefix — initial handshake    */
#define CE_FRAME_PATH_REQUEST   10  /* '6' prefix — Route/Trace REQUEST  */
#define CE_FRAME_PATH_REPLY     11  /* '7' prefix — Route/Trace REPLY    */

/* ── Path query protocol constants ──────────────────────────────────── */
#define CE_PATH_MAX_HOPS        16       /* hard cap on per-reply hops    */
#define CE_PATH_QSO_FIELD_LEN   5        /* "%5u" 5-byte correlator       */
#define CE_PATH_HOP_BYTE_BASE   0x20     /* ASCII offset for HopCount     */
#define CE_PATH_TRACE_BIT       0x40     /* bit in QSO[0] = Traceroute    */
#define CE_PATH_MAX_PENDING     16       /* in-flight queries we track    */
#define CE_PATH_TIMEOUT_SEC     30       /* pending query expiry          */
#define CE_PATH_CACHE_TTL_SEC   300      /* path cache freshness          */
#define CE_PATH_MAX_CACHE       256      /* cached reply buffer entries   */
#define CE_PATH_PROBE_SEC       0        /* default: disabled. Only useful against peers that send type-7 replies (e.g. PC/FlexNet). xnet does not. */

/* Path kinds (bit 0x40 of QSO[0]) */
#define CE_PATH_KIND_ROUTE      0        /* bit clear = Route             */
#define CE_PATH_KIND_TRACE      1        /* bit set   = Traceroute        */

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

/*
 * M6: per-port configuration.  Each entry describes one AX.25 port on
 * which flexnetd binds its listen callsign, connects out to a FlexNet
 * neighbor, and accepts inbound sessions.
 *
 * All ports share the node identity (MyCall, Alias) but each port has
 * its own bind/listen callsign and neighbor.  The listen callsign is
 * typically the same across ports (e.g. IW2OHX-3 everywhere) — this is
 * supported by the kernel when each socket uses SO_BINDTODEVICE.
 */
typedef struct {
    char    name[MAX_IFACE_LEN];               /* axports port name (e.g. "xnet") */
    char    listen_call[MAX_CALLSIGN_LEN];     /* callsign bound on this port */
    char    neighbor[MAX_CALLSIGN_LEN];        /* expected FlexNet peer on this port */
    int     min_ssid;                          /* SSID range announced to peer */
    int     max_ssid;
    int     route_advert_interval;             /* per-port M6.7 override: -1=use global, 0=disabled, >0=seconds */
} PortCfg;

typedef struct {
    char    mycall[MAX_CALLSIGN_LEN];
    char    alias[MAX_ALIAS_LEN];
    int     min_ssid;                          /* global default — used if PortCfg.min_ssid is 0 */
    int     max_ssid;

    /* M6 multi-port: array + count.  If no 'Port' blocks appear in the
     * config, legacy keywords (Neighbor/PortName/FlexListenCall) are
     * synthesised into ports[0] for backward compatibility. */
    PortCfg ports[MAX_PORTS];
    int     num_ports;

    /* Legacy single-port fields — still populated for back-compat and
     * mirror ports[0] after config_load() completes.  New code should
     * prefer the ports[] array. */
    char    neighbor[MAX_CALLSIGN_LEN];
    char    port_name[MAX_IFACE_LEN];
    char    flex_listen_call[MAX_CALLSIGN_LEN];

    int     role;                               /* 0=client (D-cmd), 1=server (native) */
    int     poll_interval;
    int     keepalive_interval;
    int     beacon_interval;
    int     trigger_threshold;
    int     infinity;
    char    gateways_file[MAX_PATH_LEN];
    char    dest_file[MAX_PATH_LEN];
    char    linkstats_file[MAX_PATH_LEN];
    char    paths_file[MAX_PATH_LEN];       /* cache file for D/trace replies */
    int     log_level;
    int     use_syslog;
    int     probe_count;
    int     path_probe_interval;    /* M5.3: seconds between type-6 probes */
    int     route_advert_interval;  /* M6.7: seconds between route re-advertisements (0=once only) */
    int     lt_reply_interval;      /* M6.9: min seconds between link-time TX (PCFlexnet expects ~320s polling; 90s is a compromise) */
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

/* M6.6 port-context global.
 *   -1 = no CE session context (parent process, or pre-fork)
 *   ≥0 = index into g_cfg.ports[] for the CE session running in this
 *        process (set in flexnetd.c accept loop right before fork).
 * Children inherit the parent's g_port_idx at fork time and keep it.
 * Used by output.c to write per-port linkstats and merge into the
 * unified linkstats output file. */
extern int g_port_idx;

/* ── Path query types (M5.3 — CE type-6/7) ──────────────────────────── */

/* Parsed type-7 reply */
typedef struct {
    int   qso;                                    /* decoded QSO id      */
    int   kind;                                   /* CE_PATH_KIND_*      */
    int   hop_count;                              /* from HOP_BYTE       */
    int   hop_callsign_count;                     /* actual callsigns    */
    char  hop_callsigns[CE_PATH_MAX_HOPS][MAX_CALLSIGN_LEN];
    time_t received;                              /* wall clock time     */
} PathReply;

/* Pending query (outstanding type-6 awaiting type-7) */
typedef struct {
    int    qso;                                   /* correlator          */
    int    kind;                                  /* CE_PATH_KIND_*      */
    char   target[MAX_CALLSIGN_LEN];              /* destination queried */
    time_t sent;                                  /* when request left   */
    int    active;                                /* 1 = in-flight, 0 = free */
} PathPending;

/* Cached reply (written to disk for flexdest consumption) */
typedef struct {
    char       target[MAX_CALLSIGN_LEN];
    int        kind;
    int        hop_callsign_count;
    char       hop_callsigns[CE_PATH_MAX_HOPS][MAX_CALLSIGN_LEN];
    time_t     cached;
} PathCacheEntry;

extern PathPending g_path_pending[CE_PATH_MAX_PENDING];

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
                             DestEntry *out, int max_entries,
                             int port_idx);
int ce_parse_dest_broadcast(const uint8_t *data, int len,
                            int *rtt_out, char *callsign_out,
                            int *ssid_lo_out, int *ssid_hi_out,
                            char *via_callsign_out, char *flag_out);

/* ── Path query (CE type-6 request / type-7 reply) ─────────────────── */
/*
 * ce_build_path_request — build a type-6 Route or Traceroute REQUEST.
 *
 * Wire format:
 *   '6' HOP_BYTE QSO_FIELD(5) ORIGIN ' ' TARGET
 *   HOP_BYTE = CE_PATH_HOP_BYTE_BASE + hop_count
 *   QSO_FIELD = sprintf("%5u", qso); if kind==TRACE, QSO_FIELD[0] |= 0x40
 *
 * Returns bytes written, -1 on error.
 */
int ce_build_path_request(uint8_t *buf, int buflen,
                          int qso, int kind,
                          const char *origin_call,
                          const char *target_call);

/*
 * ce_build_path_reply — build a type-7 reply with an accumulated hop list.
 *
 * Wire format:
 *   '7' HOP_BYTE QSO_FIELD(5) ' ' HOP_1 ' ' HOP_2 ... HOP_N
 *
 * hops[] and n_hops provide the accumulated path.  HOP_BYTE is derived
 * from n_hops.  Returns bytes written, -1 on error.
 */
int ce_build_path_reply(uint8_t *buf, int buflen,
                        int qso, int kind,
                        const char *const *hops, int n_hops);

/*
 * ce_parse_path_frame — parse a type-6 or type-7 frame.
 *
 * On success populates *reply_out for type-7 or the request-specific
 * outputs for type-6.  Returns CE_FRAME_PATH_REQUEST, CE_FRAME_PATH_REPLY,
 * or -1.
 */
int ce_parse_path_frame(const uint8_t *data, int len,
                        PathReply *reply_out,
                        int *qso_out, int *kind_out, int *hop_count_out,
                        char *origin_out, char *target_out);

/* ── Path pending-query table ──────────────────────────────────────── */
void path_pending_init(void);
int  path_pending_add(int qso, int kind, const char *target);
int  path_pending_find(int qso);
void path_pending_remove(int qso);
void path_pending_sweep(void);
int  path_pending_next_qso(void);
void path_pending_dump(void);   /* log all pending entries (DEBUG) */

/* ── Path cache ────────────────────────────────────────────────────── */
int output_write_paths_cache_add(const PathReply *r);
int output_write_paths_cache_flush(void);

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
