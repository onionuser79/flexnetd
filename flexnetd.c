/*
 * flexnetd.c — FlexNet routing daemon
 *
 * SERVER MODE (Role server — default):
 *   Binds the xnet port callsign (IW2OHX-3) directly — no ax25d.
 *   [IW2OHX-3 VIA xnet] must be ABSENT from ax25d.conf.
 *   Dispatches by first-frame PID:
 *     pid=CE/CF -> native FlexNet session (IW2OHX-14 peering)
 *     pid=F0    -> fork+exec uronode (regular user sessions)
 *   Xnet config: ro flexnet add xnet IW2OHX-3
 *
 * CLIENT MODE (Role client):
 *   Periodically connects to Neighbor, sends "d" command, parses
 *   D-table text response, writes output files.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <syslog.h>
#include "flexnetd.h"

static volatile int  g_running   = 1;
static const char   *g_config_file =
    "/usr/local/etc/ax25/flexnetd.conf";

static void sig_term(int sig) { (void)sig; g_running = 0; }
static void sig_hup(int sig)  { (void)sig; }

static void daemonise(void)
{
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid > 0)   exit(0);
    if (setsid() < 0) { perror("setsid"); exit(1); }
    freopen("/dev/null", "r", stdin);
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-c config] [-d] [-f] [-v[vv]] [-V]\n"
        "  -c file  config file (default: %s)\n"
        "  -d       daemon mode (syslog)\n"
        "  -f       foreground, stderr\n"
        "  -v       verbose; repeat for more (-vvv = DEBUG)\n"
        "  -V       print version and exit\n",
        prog, g_config_file);
}

static int ensure_output_dir(const char *filepath)
{
    char dir[MAX_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s", filepath);
    char *slash = strrchr(dir, '/');
    if (!slash) return 0;
    *slash = '\0';
    struct stat st;
    if (stat(dir, &st) != 0) {
        LOG_ERR("Output dir '%s' missing", dir);
        return -1;
    }
    return S_ISDIR(st.st_mode) ? 0 : -1;
}

/* ── SERVER MODE ─────────────────────────────────────────────────────── */
/*
 * Bind FlexListenCall (xnet port callsign, e.g. IW2OHX-3) directly.
 * Dispatch by first-frame PID:
 *   CE/CF -> native FlexNet session handler
 *   F0    -> fork + exec uronode
 */
static int run_server(void)
{
    LOG_INF("flexnetd: SERVER MODE");
    LOG_INF("flexnetd: listening on %s (port %s)",
            g_cfg.flex_listen_call, g_cfg.port_name);
    LOG_INF("flexnetd: FlexNet from %s -> native CE/CF handler",
            g_cfg.neighbor);
    LOG_INF("flexnetd: user sessions -> exec uronode");

    int listen_fd = ax25_listen(g_cfg.flex_listen_call, g_cfg.port_name);
    if (listen_fd < 0) {
        LOG_ERR("flexnetd: ax25_listen('%s') failed",
                g_cfg.flex_listen_call);
        LOG_ERR("flexnetd: ensure [%s VIA %s] is ABSENT from ax25d.conf",
                g_cfg.flex_listen_call, g_cfg.port_name);
        return -1;
    }

    /* Tune kernel AX.25 timing for this AXUDP port:
     * t2_timeout=50ms (fast ack), window=7 (match axports) */
    ax25_tune_interface(g_cfg.port_name);

    output_write_gateways();
    int session_count = 0;

    while (g_running) {

        fd_set rfds;
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);

        int ret = select(listen_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG_ERR("flexnetd: select(): %s", strerror(errno));
            break;
        }
        if (ret == 0) continue;

        char peer_call[MAX_CALLSIGN_LEN] = {0};
        int  conn_fd = ax25_accept(listen_fd, peer_call, sizeof(peer_call));
        if (conn_fd < 0) {
            LOG_ERR("flexnetd: accept(): %s", strerror(errno));
            sleep(1);
            continue;
        }

        session_count++;
        LOG_INF("flexnetd: === SESSION #%d from %s ===",
                session_count, peer_call);

        /*
         * Determine protocol: FlexNet neighbor or user session.
         *
         * If the peer is our configured neighbor, this is a FlexNet
         * session — send a CE keepalive to kick-start the exchange
         * (Xnet may wait for us to send first after a fresh L2 link).
         *
         * Otherwise, peek at the first byte to classify.
         */
        uint8_t first_pid = 0;
        int is_neighbor = (strcasecmp(peer_call, g_cfg.neighbor) == 0);

        if (is_neighbor) {
            /* FlexNet neighbor — enable PIDINCL for CE/CF framing */
            ax25_enable_pidincl(conn_fd);

            /* Send CE link setup (SSID range) */
            LOG_INF("flexnetd: neighbor %s connected — "
                    "sending CE link setup (SSID %d-%d)",
                    peer_call, g_cfg.min_ssid, g_cfg.max_ssid);
            uint8_t setup[8];
            int slen = ce_build_link_setup(setup, sizeof(setup),
                                           g_cfg.min_ssid, g_cfg.max_ssid);
            if (slen > 0)
                ax25_send(conn_fd, PID_CE, setup, slen);
            /* Send keepalive to kick-start the exchange —
             * xnet waits for this before sending routing data */
            send_ce_keepalive(conn_fd);
            first_pid = PID_CE;  /* treat as FlexNet session */
        } else {
            /* Non-neighbor peer — always a user session.
             *
             * FlexNet L3 routed connections arrive with the actual
             * user callsign (e.g. IW7BIA-15), not the neighbor call.
             * No need to peek/wait — dispatch to uronode immediately
             * so the user sees the MOTD without having to send CR. */
            LOG_INF("flexnetd: session #%d — non-neighbor %s, "
                    "dispatching to uronode", session_count, peer_call);
            first_pid = PID_F0;  /* force user session path */
        }

        LOG_INF("flexnetd: session #%d first_pid=0x%02X from %s",
                session_count, first_pid, peer_call);

        if (first_pid == PID_CE || first_pid == PID_CF) {
            /* Native FlexNet peering — fork so the accept loop stays free
             * for user connections while the CE session runs (minutes). */
            LOG_INF("flexnetd: FlexNet session (pid=0x%02X) from %s",
                    first_pid, peer_call);
            pid_t ce_child = fork();
            if (ce_child < 0) {
                LOG_ERR("flexnetd: fork(CE): %s", strerror(errno));
                ax25_disconnect(conn_fd);
            } else if (ce_child == 0) {
                /* Child: run the CE session, write output, exit */
                close(listen_fd);
                dtable_init();
                int r = poll_cycle_run_mode(conn_fd, 1);
                ax25_disconnect(conn_fd);
                if (r == 0 && g_table.count > 0) {
                    output_write_destinations();
                    LOG_INF("flexnetd: CE child done — %d routes written",
                            g_table.count);
                } else {
                    LOG_WRN("flexnetd: CE child done — no routes (r=%d)", r);
                }
                if (g_log_level >= LOG_LEVEL_DEBUG) dtable_dump();
                _exit(0);
            } else {
                /* Parent: close conn_fd, CE child owns it */
                close(conn_fd);
                LOG_INF("flexnetd: CE session pid=%d for session #%d",
                        (int)ce_child, session_count);
            }

        } else {
            /* User session — dispatch to uronode */
            LOG_INF("flexnetd: user session (pid=0x%02X) -> uronode", first_pid);
            pid_t child = fork();
            if (child < 0) {
                LOG_ERR("flexnetd: fork(): %s", strerror(errno));
                ax25_disconnect(conn_fd);
            } else if (child == 0) {
                /* Child: wire conn_fd to stdin/stdout/stderr, exec uronode */
                dup2(conn_fd, 0);
                dup2(conn_fd, 1);
                dup2(conn_fd, 2);
                close(conn_fd);
                close(listen_fd);
                execl("/usr/local/sbin/uronode", "uronode", (char *)NULL);
                LOG_ERR("flexnetd: execl(uronode): %s", strerror(errno));
                _exit(1);
            } else {
                /* Parent: close conn_fd, uronode child owns it now */
                close(conn_fd);
                LOG_INF("flexnetd: uronode pid=%d for session #%d",
                        (int)child, session_count);
            }
        }
    }

    close(listen_fd);
    LOG_INF("flexnetd: server stopped (sessions: %d)", session_count);
    return 0;
}

/* ── CLIENT MODE ─────────────────────────────────────────────────────── */
static int run_client(void)
{
    LOG_INF("flexnetd: CLIENT MODE (D-command polling)");
    LOG_INF("flexnetd: Neighbor=%s  Port=%s  PollInterval=%ds",
            g_cfg.neighbor, g_cfg.port_name, g_cfg.poll_interval);

    if (output_write_gateways() < 0) {
        LOG_ERR("flexnetd: cannot write gateways file");
        return -1;
    }

    int    fd          = -1;
    time_t last_poll   = 0;
    int    cycle_count = 0;

    while (g_running) {
        time_t now = time(NULL);

        if (now - last_poll >= g_cfg.poll_interval) {
            cycle_count++;
            LOG_INF("flexnetd: ── CYCLE #%d ──", cycle_count);

            if (fd >= 0) { ax25_disconnect(fd); fd = -1; }

            fd = ax25_connect(g_cfg.mycall, g_cfg.neighbor, g_cfg.port_name);
            if (fd < 0) {
                LOG_ERR("flexnetd: connect failed, retry in %ds",
                        g_cfg.poll_interval);
                last_poll = now;
                continue;
            }

            dtable_init();
            int r = poll_cycle_run(fd);
            last_poll = now;
            ax25_disconnect(fd);
            fd = -1;

            if (r == 0)
                LOG_INF("flexnetd: cycle #%d done, next in %ds",
                        cycle_count, g_cfg.poll_interval);
            else
                LOG_ERR("flexnetd: cycle #%d failed", cycle_count);

            if (g_log_level >= LOG_LEVEL_DEBUG) dtable_dump();
        }

        time_t next = last_poll + g_cfg.poll_interval;
        long sleep_s = (long)(next - time(NULL));
        if (sleep_s > 10) sleep_s = 10;
        if (sleep_s <= 0) sleep_s =  1;
        sleep((unsigned int)sleep_s);
    }

    if (fd >= 0) ax25_disconnect(fd);
    return 0;
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    int opt;
    int run_as_daemon    = 0;
    int force_foreground = 0;
    int verbosity_bump   = 0;

    while ((opt = getopt(argc, argv, "c:dfvV")) != -1) {
        switch (opt) {
        case 'c': g_config_file  = optarg; break;
        case 'd': run_as_daemon  = 1; force_foreground = 0; break;
        case 'f': run_as_daemon  = 0; force_foreground = 1; break;
        case 'v': verbosity_bump++; break;
        case 'V': printf("flexnetd v%s\n", FLEXNETD_VERSION); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    g_log_level  = LOG_LEVEL_INFO;
    g_use_syslog = 0;

    if (config_load(g_config_file, &g_cfg) < 0) {
        fprintf(stderr, "flexnetd: failed to load '%s'\n", g_config_file);
        return 1;
    }

    g_cfg.log_level += verbosity_bump;
    if (g_cfg.log_level > LOG_LEVEL_DEBUG) g_cfg.log_level = LOG_LEVEL_DEBUG;
    g_log_level = g_cfg.log_level;

    if      (force_foreground) g_use_syslog = 0;
    else if (run_as_daemon)    g_use_syslog = 1;
    else                       g_use_syslog = g_cfg.use_syslog;

    if (g_use_syslog)
        openlog("flexnetd", LOG_PID | LOG_CONS, LOG_DAEMON);

    LOG_INF("flexnetd v%s starting  (log=%d  syslog=%s)",
            FLEXNETD_VERSION, g_log_level,
            g_use_syslog ? "yes" : "no");
    config_dump(&g_cfg);

    if (strcmp(g_cfg.mycall,   "NOCALL-3")  == 0 ||
        strcmp(g_cfg.neighbor, "NOCALL-14") == 0) {
        LOG_ERR("MyCall/Neighbor not set in '%s'", g_config_file);
        return 1;
    }

    if (ensure_output_dir(g_cfg.gateways_file) < 0) return 1;
    if (ensure_output_dir(g_cfg.dest_file)     < 0) return 1;

    dtable_init();

    if (run_as_daemon) daemonise();

    signal(SIGTERM, sig_term);
    signal(SIGINT,  sig_term);
    signal(SIGHUP,  sig_hup);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);   /* auto-reap uronode children */

    int ret = (g_cfg.role == 1) ? run_server() : run_client();

    if (g_use_syslog) closelog();
    return (ret == 0) ? 0 : 1;
}
