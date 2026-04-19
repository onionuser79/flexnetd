/*
 * poll_cycle.c — FlexNet polling and native session handler
 *
 * Two modes:
 *
 * D-COMMAND MODE (daemon, fd > 0):
 *   wait for => prompt, send "d", collect D-table text, parse, write files.
 *
 * NATIVE MODE (stdin -s, fd = 0):
 *   Handle inbound CE/CF FlexNet protocol from IW2OHX-14.
 *   ax25d passes data via pipe — use read()/write() not recv()/send().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include "flexnetd.h"

/* ── D-COMMAND MODE ──────────────────────────────────────────────────── */

static int wait_for_prompt(int fd, int timeout_ms)
{
    char buf[4096] = {0};
    int  total = 0;
    int  deadline = timeout_ms;

    LOG_INF("wait_for_prompt: waiting for '=>' prompt (%dms timeout)",
            timeout_ms);

    while (deadline > 0) {
        uint8_t chunk[512];
        uint8_t pid = 0;
        int chunk_timeout = (deadline < 500) ? deadline : 500;

        int len = ax25_recv(fd, &pid, chunk, (int)sizeof(chunk) - 1,
                            chunk_timeout);
        if (len < 0)  return -1;
        if (len == 0) { deadline -= chunk_timeout; continue; }

        chunk[len] = '\0';

        if (total + len < (int)sizeof(buf) - 1) {
            memcpy(buf + total, chunk, (size_t)len);
            total += len;
            buf[total] = '\0';
        } else {
            int keep = (int)sizeof(buf) - len - 1;
            if (keep > 0) memmove(buf, buf + total - keep, (size_t)keep);
            else          keep = 0;
            memcpy(buf + keep, chunk, (size_t)len);
            total = keep + len;
            buf[total] = '\0';
        }

        LOG_DBG("wait_for_prompt: received %d bytes (total=%d)", len, total);

        if (strstr(buf, "=>") != NULL) {
            LOG_INF("wait_for_prompt: prompt found after %d bytes", total);
            return 0;
        }

        deadline -= chunk_timeout;
    }

    LOG_ERR("wait_for_prompt: timeout — no prompt received");
    return -1;
}

static int collect_response(int fd, const char *cmd,
                             char *out_buf, int out_buflen,
                             int timeout_ms)
{
    uint8_t cmd_buf[32];
    int cmd_len = snprintf((char *)cmd_buf, sizeof(cmd_buf), "%s\r", cmd);
    LOG_INF("collect_response: sending command '%s'", cmd);

    if (ax25_send(fd, PID_F0, cmd_buf, cmd_len) < 0) {
        LOG_ERR("collect_response: failed to send '%s'", cmd);
        return -1;
    }

    int total = 0;
    int deadline = timeout_ms;
    memset(out_buf, 0, (size_t)out_buflen);

    while (deadline > 0) {
        uint8_t chunk[1024];
        uint8_t pid = 0;
        int chunk_timeout = (deadline < 500) ? deadline : 500;

        int len = ax25_recv(fd, &pid, chunk, (int)sizeof(chunk) - 1,
                            chunk_timeout);
        if (len < 0)  return -1;
        if (len == 0) { deadline -= chunk_timeout; continue; }

        chunk[len] = '\0';

        int space = out_buflen - total - 1;
        if (space > 0) {
            int copy = (len < space) ? len : space;
            memcpy(out_buf + total, chunk, (size_t)copy);
            total += copy;
            out_buf[total] = '\0';
        }

        LOG_DBG("collect_response: +%d bytes (total=%d)", len, total);

        if (strstr(out_buf + (total > 4 ? total - 4 : 0), "=>") != NULL) {
            char *prompt = strstr(out_buf, "\n=>");
            if (!prompt) prompt = strstr(out_buf, "\r=>");
            if (!prompt) prompt = strstr(out_buf, "=>");
            if (prompt) *prompt = '\0';
            LOG_INF("collect_response: complete, %d bytes", total);
            return total;
        }

        deadline -= chunk_timeout;
    }

    LOG_WRN("collect_response: timeout after %d bytes", total);
    return total;
}

static int run_dcommand_cycle(int fd)
{
    LOG_INF("poll_cycle: D-command mode (fd=%d)", fd);

    if (wait_for_prompt(fd, 15000) < 0) {
        LOG_ERR("poll_cycle: no prompt — aborting");
        return -1;
    }

    int resp_size = 131072;
    char *resp = calloc(1, (size_t)resp_size);
    if (!resp) { LOG_ERR("poll_cycle: calloc failed"); return -1; }

    int resp_len = collect_response(fd, "d", resp, resp_size, 60000);
    if (resp_len < 0) {
        LOG_ERR("poll_cycle: failed to collect D-table");
        free(resp);
        return -1;
    }

    LOG_INF("poll_cycle: D-table response: %d bytes", resp_len);
    int sample = resp_len < 300 ? resp_len : 300;
    LOG_DBG("poll_cycle: sample:\n%.*s", sample, resp);

    int merged = dtable_load_from_text(resp, 0);
    free(resp);

    LOG_INF("poll_cycle: merged=%d  table=%d  reachable=%d",
            merged, g_table.count, dtable_count_reachable());

    output_write_destinations();
    return 0;
}

/* ── NATIVE CE/CF MODE ───────────────────────────────────────────────── */

/*
 * ce_send_init_response — send our CE init handshake to the peer.
 *
 * Always builds a proper init frame via ce_build_link_setup() with
 * byte 0 = 0x30 (init marker).  Never echoes raw received frames —
 * that would send byte 0 = 0x3E in pipe mode, which strict peers
 * misclassify as a CE type-6 destination query.
 */
static int ce_send_init_response(int fd)
{
    uint8_t frame[8];
    int len = ce_build_link_setup(frame, sizeof(frame),
                                  g_cfg.min_ssid, g_cfg.max_ssid);
    if (len < 0) {
        LOG_ERR("ce_send_init_response: ce_build_link_setup failed");
        return -1;
    }
    LOG_INF("ce_send_init_response: fd=%d bytes: %02X %02X %02X %02X %02X "
            "(SSID %d-%d)",
            fd, frame[0], frame[1], frame[2], frame[3], frame[4],
            g_cfg.min_ssid, g_cfg.max_ssid);
    return ax25_send(fd, PID_CE, frame, len);
}

/*
 * send_own_routes — advertise our local destinations to the peer.
 *
 * Sends:  3+\r  (request token)
 *         3<CALL><SSID_LO><SSID_HI><RTT> \r  (our route)
 *         3-\r  (release token)
 */
static int send_own_routes(int fd)
{
    char base[MAX_CALLSIGN_LEN] = {0};
    int  dummy_ssid = 0;
    callsign_parse_ssid(g_cfg.mycall, base, &dummy_ssid);

    /* Request token */
    uint8_t req[] = { '3', '+', '\r' };
    ax25_send(fd, PID_CE, req, 3);

    /* Our route via ce_build_record (single wire-format source) */
    uint8_t route[64];
    int rlen = ce_build_record(route, sizeof(route),
                               base, g_cfg.min_ssid, g_cfg.max_ssid,
                               1, 0);  /* RTT=1 (local/direct), not indirect */
    if (rlen > 0)
        ax25_send(fd, PID_CE, route, rlen);

    /* Release token */
    uint8_t rel[] = { '3', '-', '\r' };
    ax25_send(fd, PID_CE, rel, 3);

    LOG_INF("send_own_routes: advertised %s SSID %d-%d RTT=1",
            base, g_cfg.min_ssid, g_cfg.max_ssid);
    return 0;
}

/*
 * run_native_ce_session — handle inbound FlexNet CE/CF session.
 *
 * ax25d spawns us when IW2OHX-14 connects to IW2OHX-7.
 * fd=0 (stdin) is a PIPE from ax25d — use read()/write() not recv()/send().
 * ax25_recv/send handle the pipe vs socket detection automatically.
 */
static int run_native_ce_session(int fd)
{
    LOG_INF("run_native_ce_session: started (fd=%d)", fd);
    LOG_INF("run_native_ce_session: waiting for CE frames from %s",
            g_cfg.neighbor);

    int     got_setup    = 0;
    int     merged_total = 0;
    time_t  t_start      = time(NULL);
    int     keepalive_count = 0;
    int     frame_count  = 0;
    long    link_time_ms = 0;       /* from CE type-1 link time frames */
    int     last_token   = 0;       /* from CE type-4 token frames */
    int     got_peer_init = 0;      /* received peer's init handshake   */
    int     sent_routes   = 0;      /* advertised our routes to peer    */
    time_t  last_dest_write = 0;    /* last time we wrote destinations  */
    time_t  last_stats_write = 0;   /* last time we wrote linkstats     */
    time_t  last_probe_sent  = 0;   /* last time we sent a type-6 probe */
    int     probe_idx        = 0;   /* round-robin dtable index         */
    time_t  last_keepalive_tx = time(NULL);   /* proactive keepalive timer */
    struct timespec lt_sent_ts = {0};  /* when we last sent a link-time frame */
    int     lt_sent_pending = 0;       /* 1 = waiting for peer's link-time reply */

    /* Initialize link stats for this session.
     * M6.6: use THIS CE child's port-specific neighbor (from g_port_idx)
     * instead of the legacy g_cfg.neighbor (which mirrors ports[0]). */
    memset(&g_link_stats, 0, sizeof(g_link_stats));
    if (g_port_idx >= 0 && g_port_idx < g_cfg.num_ports) {
        snprintf(g_link_stats.neighbor, MAX_CALLSIGN_LEN, "%s",
                 g_cfg.ports[g_port_idx].neighbor);
        g_link_stats.port_num = g_port_idx;
    } else {
        snprintf(g_link_stats.neighbor, MAX_CALLSIGN_LEN, "%s",
                 g_cfg.neighbor);
        g_link_stats.port_num = 0;
    }
    g_link_stats.connect_time = t_start;
    g_link_stats.qt           = 1;     /* direct link = Q/T 1 (best) */
    g_link_stats.rtt_last     = 0;
    g_link_stats.rtt_smoothed = 0;
    g_link_stats.active       = 1;

    /* Session timeout: 5 minutes of inactivity */
    while (time(NULL) - t_start < 300) {

        /* Periodic linkstats update — runs every iteration (frames + idle) */
        {
            time_t now = time(NULL);
            g_link_stats.keepalive_count = keepalive_count;
            g_link_stats.dst_count = dtable_count_reachable();
            if (now - last_stats_write >= 30) {
                output_write_linkstats();
                path_pending_sweep();  /* expire stale in-flight queries */
                path_pending_dump();   /* DEBUG-only: log table state */
                last_stats_write = now;
            }

            /* Proactive CE keepalive timer.  Some peers (e.g. PCFlexnet
             * on the pcf port) do NOT send CE keepalives autonomously —
             * they expect us to initiate and respond to theirs.  If the
             * peer is silent we would never send either, the RTT timer
             * on the peer's side expires (4095 sample), and the session
             * gets torn down and reconnected.
             *
             * Send a keepalive every 20 s since the last one we sent.
             * xnet (which DOES send keepalives of its own) still works
             * because our extra ones are harmless — it just responds.
             *
             * We also send a link-time probe after the keepalive so the
             * peer's RTT cycle has something to measure against. */
            if (now - last_keepalive_tx >= 20) {
                LOG_DBG("run_native_ce_session: proactive keepalive "
                        "(quiet %lds)", (long)(now - last_keepalive_tx));
                send_ce_keepalive(fd);
                g_link_stats.tx_bytes += CE_KEEPALIVE_LEN;
                g_link_stats.tx_frames++;

                uint8_t lt_buf[32];
                int lt_len = ce_build_link_time(lt_buf, sizeof(lt_buf), 2);
                if (lt_len > 0) {
                    ax25_send(fd, PID_CE, lt_buf, lt_len);
                    g_link_stats.tx_bytes += lt_len;
                    g_link_stats.tx_frames++;
                    clock_gettime(CLOCK_MONOTONIC, &lt_sent_ts);
                    lt_sent_pending = 1;
                }
                last_keepalive_tx = now;
            }

            /* M5.3: Periodic type-6 path probe (interval from config).
             * Round-robin through the dtable, sending one request per
             * interval.  Replies populate the path cache which flexdest
             * reads with -r.  Only start probing after the link is
             * stable (routes exchanged).  Interval of 0 disables. */
            int probe_sec = g_cfg.path_probe_interval;
            if (probe_sec > 0 && sent_routes && g_table.count > 0 &&
                (now - last_probe_sent >= probe_sec))
            {
                /* advance round-robin, skip unreachable entries */
                int tries = 0;
                while (tries < g_table.count) {
                    if (probe_idx >= g_table.count) probe_idx = 0;
                    DestEntry *e = &g_table.entries[probe_idx++];
                    tries++;
                    if (e->rtt >= g_cfg.infinity) continue;
                    if (strcasecmp(e->callsign, g_cfg.mycall) == 0) continue;
                    if (strcasecmp(e->callsign, g_cfg.flex_listen_call) == 0) continue;

                    int qso = path_pending_next_qso();
                    /* build target with mid-SSID for range entries */
                    char target_call[MAX_CALLSIGN_LEN];
                    int  mid = e->ssid_lo;
                    if (mid > 0)
                        snprintf(target_call, MAX_CALLSIGN_LEN, "%s-%d",
                                 e->callsign, mid);
                    else
                        snprintf(target_call, MAX_CALLSIGN_LEN, "%s",
                                 e->callsign);

                    const char *origin = g_cfg.flex_listen_call[0]
                                         ? g_cfg.flex_listen_call
                                         : g_cfg.mycall;
                    uint8_t pbuf[256];
                    int plen = ce_build_path_request(pbuf, sizeof(pbuf),
                                                     qso, CE_PATH_KIND_ROUTE,
                                                     origin, target_call);
                    if (plen > 0) {
                        hex_dump("TX type-6 probe", pbuf, plen);
                        ax25_send(fd, PID_CE, pbuf, plen);
                        g_link_stats.tx_bytes += plen;
                        g_link_stats.tx_frames++;
                        path_pending_add(qso, CE_PATH_KIND_ROUTE, target_call);
                        last_probe_sent = now;
                        LOG_INF("run_native_ce_session: sent type-6 "
                                "probe qso=%d target=%s (%d bytes)",
                                qso, target_call, plen);
                    }
                    break;
                }
            }
        }

        uint8_t pid = 0;
        uint8_t buf[2048];

        int len = ax25_recv(fd, &pid, buf, (int)sizeof(buf) - 1, 10000);

        if (len < 0) {
            LOG_INF("run_native_ce_session: session ended (read returned %d)",
                    len);
            break;
        }
        if (len == 0) {
            LOG_DBG("run_native_ce_session: idle (frame_count=%d)",
                    frame_count);
            continue;
        }

        frame_count++;
        buf[len] = '\0';
        g_link_stats.rx_bytes += len;
        g_link_stats.rx_frames++;

        /* Log first byte of every frame for diagnostics */
        LOG_INF("run_native_ce_session: frame #%d pid=0x%02X len=%d "
                "first=0x%02X ('%c')",
                frame_count, pid, len, buf[0],
                (buf[0] >= 32 && buf[0] < 127) ? buf[0] : '.');

        /* In pipe mode pid is always set to PID_CE by ax25_recv.
         * But the actual CE/CF distinction is in the data content
         * (CE frames start with '>', '2', '3'; CF with 'L3RTT:').
         * Accept both PID_CE and PID_CF, and also pid=0x00. */
        if (pid != PID_CE && pid != PID_CF && pid != 0x00) {
            LOG_WRN("run_native_ce_session: unexpected pid=0x%02X, "
                    "processing anyway", pid);
        }

        /* ── Detect frame type from content ─────────────────────── */

        /* Link setup (pipe mode): byte 0 = 0x3E '>' when ax25d strips PID.
         * In pipe mode the leading 0x30 (init marker) is consumed by ax25d,
         * so byte[0] is the max_ssid byte (0x30+ssid, e.g. 0x3E for ssid=14).
         * Detect by: first byte in 0x30..0x3F range, followed by 0x25 0x21 0x0D.
         */
        if (buf[0] >= 0x30 && buf[0] <= 0x3F &&
            len >= 4 && buf[1] == 0x25 && buf[2] == 0x21 && buf[3] == 0x0D) {
            int peer_max_ssid = (int)(buf[0]) - 0x30;
            LOG_INF("run_native_ce_session: CE link setup (pipe mode) "
                    "peer_max_ssid=%d (len=%d)", peer_max_ssid, len);

            if (!got_setup) {
                got_setup = 1;

                /* Send a proper init response — never echo raw frame */
                int r = ce_send_init_response(fd);
                if (r < 0)
                    LOG_WRN("run_native_ce_session: init response failed");
                else
                    LOG_INF("run_native_ce_session: init response sent "
                            "(byte0=0x30)");

                send_ce_keepalive(fd);
            } else {
                LOG_DBG("run_native_ce_session: duplicate link setup ignored");
            }
            t_start = time(NULL);
            continue;
        }

        /* Keepalive: 241 bytes starting with '2' */
        if (len == CE_KEEPALIVE_LEN && buf[0] == '2') {
            keepalive_count++;
            LOG_INF("run_native_ce_session: CE keepalive #%d", keepalive_count);
            send_ce_keepalive(fd);
            g_link_stats.tx_bytes += CE_KEEPALIVE_LEN;
            g_link_stats.tx_frames++;
            /* Reset proactive-keepalive timer — we just sent one. */
            last_keepalive_tx = time(NULL);

            /* Send link time on every keepalive cycle so the peer can
             * converge its smoothed RTT.  Without this, Q/T stays
             * frozen at 301 and rtt at 600/2 indefinitely. */
            {
                uint8_t lt_buf[32];
                int lt_len = ce_build_link_time(lt_buf,
                                (int)sizeof(lt_buf), 2);
                if (lt_len > 0) {
                    ax25_send(fd, PID_CE, lt_buf, lt_len);
                    g_link_stats.tx_bytes += lt_len;
                    g_link_stats.tx_frames++;
                    /* Record send time for RTT measurement */
                    clock_gettime(CLOCK_MONOTONIC, &lt_sent_ts);
                    lt_sent_pending = 1;
                }
            }

            /* After first keepalive exchange with peer, the link is
             * stable — proactively advertise our routes.  The peer
             * does not send '3+' (request token) unprompted, so we
             * initiate the route advertisement ourselves. */
            if (!sent_routes && got_peer_init && got_setup) {
                LOG_INF("run_native_ce_session: link stable — "
                        "sending our routes");
                send_own_routes(fd);
                sent_routes = 1;
            }

            t_start = time(NULL);
            continue;
        }

        /* Init handshake: '0' prefix ("Initial Handshake")
         *
         * In socket mode (server) the full CE payload arrives as
         * 30 3E 25 21 0D — first byte is 0x30 ('0'), NOT 0x3E.
         *
         * Do NOT send an init response here — in server mode the parent
         * already sent our init via ce_build_link_setup() before forking.
         * Sending a second init confuses the peer (resets the link state).
         * Only the pipe-mode handler sends init response (no parent). */
        if (buf[0] == '0') {
            int upper_ssid = 0;
            int r = ce_parse_frame(buf, len, NULL, &upper_ssid, NULL);
            if (r == CE_FRAME_INIT) {
                LOG_INF("run_native_ce_session: init handshake "
                        "peer_upper_ssid=%d (no reply — server already sent)",
                        upper_ssid);
                got_peer_init = 1;
                got_setup     = 1;  /* enables route advert trigger */
            }
            t_start = time(NULL);
            continue;
        }

        /* Link time: '1' prefix — link time in 100ms ticks */
        if (buf[0] == '1' && len > 3) {
            int lt_val = 0;
            int r = ce_parse_frame(buf, len, NULL, NULL, &lt_val);
            if (r == CE_FRAME_LINK_TIME) {
                link_time_ms = lt_val;
                LOG_INF("run_native_ce_session: peer link time = %ld "
                        "(100ms ticks)", link_time_ms);

                /* Measure actual RTT: time since we sent our link-time
                 * until we received the peer's link-time response.
                 * Convert to 100ms ticks to match L-table format. */
                if (lt_sent_pending) {
                    struct timespec now_ts;
                    clock_gettime(CLOCK_MONOTONIC, &now_ts);
                    long rtt_ms = (now_ts.tv_sec - lt_sent_ts.tv_sec) * 1000L
                                + (now_ts.tv_nsec - lt_sent_ts.tv_nsec) / 1000000L;
                    int rtt_ticks = (int)(rtt_ms / 100);  /* 100ms ticks */
                    g_link_stats.rtt_last = rtt_ticks;
                    /* Exponential smoothing: 75% old + 25% new */
                    if (g_link_stats.rtt_smoothed == 0 && keepalive_count <= 1)
                        g_link_stats.rtt_smoothed = rtt_ticks;
                    else
                        g_link_stats.rtt_smoothed =
                            (g_link_stats.rtt_smoothed * 3 + rtt_ticks) / 4;
                    lt_sent_pending = 0;
                    LOG_INF("run_native_ce_session: measured RTT=%ldms "
                            "(%d ticks) smoothed=%d",
                            rtt_ms, rtt_ticks, g_link_stats.rtt_smoothed);
                }
                /* Q/T = 1 for direct link (always) */
                g_link_stats.qt = 1;

                /* Reply with our own link time EVERY time.
                 * The peer uses exponential smoothing — it needs repeated
                 * measurements to converge from the initial 600
                 * (RTT_INFINITY) toward the actual value. */
                uint8_t lt_buf[32];
                int lt_len = ce_build_link_time(lt_buf,
                                (int)sizeof(lt_buf), 2);
                if (lt_len > 0) {
                    ax25_send(fd, PID_CE, lt_buf, lt_len);
                    g_link_stats.tx_bytes += lt_len;
                    g_link_stats.tx_frames++;
                    /* Also record this send for next RTT measurement */
                    clock_gettime(CLOCK_MONOTONIC, &lt_sent_ts);
                    lt_sent_pending = 1;
                    LOG_DBG("run_native_ce_session: sent link time = 2");
                }
            }
            t_start = time(NULL);
            continue;
        }

        /* Token: '4' prefix — token/sequence exchange */
        if (buf[0] == '4') {
            char flag_buf[MAX_CALLSIGN_LEN] = {0};
            int token_val = 0;
            int r = ce_parse_frame(buf, len, flag_buf, NULL, &token_val);
            if (r == CE_FRAME_TOKEN) {
                last_token = token_val;
                LOG_INF("run_native_ce_session: token val=%d flag='%c'",
                        token_val, flag_buf[0]);
                /* Echo token back (the peer may expect acknowledgement) */
                uint8_t tbuf[32];
                int tlen = ce_build_token(tbuf, sizeof(tbuf),
                                          token_val, flag_buf[0]);
                if (tlen > 0)
                    ax25_send(fd, PID_CE, tbuf, tlen);
            }
            t_start = time(NULL);
            continue;
        }

        /* Type-6: Route / Traceroute REQUEST (M5.3) */
        if (buf[0] == '6') {
            hex_dump("RX type-6 request", buf, len);
            int qso = 0, kind = 0, hop_count = 0;
            char origin[MAX_CALLSIGN_LEN] = {0};
            char target[MAX_CALLSIGN_LEN] = {0};

            int pr = ce_parse_path_frame(buf, len, NULL,
                                         &qso, &kind, &hop_count,
                                         origin, target);
            if (pr == CE_FRAME_PATH_REQUEST) {
                LOG_INF("run_native_ce_session: type-6 REQUEST qso=%d "
                        "kind=%s hops=%d origin=%s target=%s",
                        qso,
                        kind == CE_PATH_KIND_TRACE ? "TRACE" : "ROUTE",
                        hop_count, origin, target);

                /* If the target is our mycall or listen_call, the peer
                 * is asking us for a path TO us — reply with just our
                 * own callsign as the (final) hop. */
                if (strcasecmp(target, g_cfg.mycall) == 0 ||
                    strcasecmp(target, g_cfg.flex_listen_call) == 0)
                {
                    hex_dump("RX type-6 request (for us)", buf, len);
                    const char *our[1] = { g_cfg.flex_listen_call[0]
                                           ? g_cfg.flex_listen_call
                                           : g_cfg.mycall };
                    uint8_t rbuf[128];
                    int rlen = ce_build_path_reply(rbuf, sizeof(rbuf),
                                                   qso, kind, our, 1);
                    if (rlen > 0) {
                        hex_dump("TX type-7 reply", rbuf, rlen);
                        ax25_send(fd, PID_CE, rbuf, rlen);
                        g_link_stats.tx_bytes += rlen;
                        g_link_stats.tx_frames++;
                        LOG_INF("run_native_ce_session: sent type-7 "
                                "reply (qso=%d, single-hop, %d bytes)",
                                qso, rlen);
                    }
                } else {
                    /* Target is not us.  In single-neighbor setup we
                     * cannot forward — log and drop.  Cross-port
                     * forwarding is the M2.1/M6 milestone. */
                    LOG_DBG("run_native_ce_session: type-6 for %s "
                            "not us — ignored (forwarding is M2.1)",
                            target);
                }
            } else {
                LOG_DBG("run_native_ce_session: type-6 parse failed "
                        "(len=%d)", len);
            }
            t_start = time(NULL);
            continue;
        }

        /* Type-7: Route / Traceroute REPLY (M5.3) */
        if (buf[0] == '7') {
            hex_dump("RX type-7 reply", buf, len);
            PathReply reply;
            int pr = ce_parse_path_frame(buf, len, &reply,
                                         NULL, NULL, NULL, NULL, NULL);
            if (pr == CE_FRAME_PATH_REPLY) {
                LOG_INF("run_native_ce_session: type-7 REPLY qso=%d "
                        "kind=%s hops=%d",
                        reply.qso,
                        reply.kind == CE_PATH_KIND_TRACE ? "TRACE" : "ROUTE",
                        reply.hop_callsign_count);
                for (int i = 0; i < reply.hop_callsign_count; i++)
                    LOG_DBG("  hop[%d]=%s", i, reply.hop_callsigns[i]);
                /* Match to outstanding query and clear pending */
                int slot = path_pending_find(reply.qso);
                if (slot >= 0) {
                    LOG_INF("  matches pending slot=%d target=%s "
                            "(elapsed %ds)",
                            slot, g_path_pending[slot].target,
                            (int)(time(NULL) - g_path_pending[slot].sent));
                    path_pending_remove(reply.qso);
                } else {
                    LOG_WRN("  UNSOLICITED reply (qso=%d not pending)",
                            reply.qso);
                }
                /* Cache the reply so flexdest can show it */
                output_write_paths_cache_add(&reply);
            } else {
                LOG_DBG("run_native_ce_session: type-7 parse failed "
                        "(len=%d)", len);
                hex_dump("FAILED type-7 frame", buf, len);
            }
            t_start = time(NULL);
            continue;
        }

        /* Status and compact routing records: starts with '3' */
        if (buf[0] == '3') {
            int r = ce_parse_frame(buf, len, NULL, NULL, NULL);

            if (r == CE_FRAME_STATUS_POS) {
                /* '3+' = "request token" — peer is asking us to
                 * exchange routes.  Send ours if not already done. */
                LOG_INF("run_native_ce_session: '3+' received from peer");
                if (!sent_routes) {
                    send_own_routes(fd);
                    sent_routes = 1;
                } else {
                    /* Already sent — just ack the token request */
                    uint8_t ack[] = { '3', '+', '\r' };
                    ax25_send(fd, PID_CE, ack, 3);
                    uint8_t rel[] = { '3', '-', '\r' };
                    ax25_send(fd, PID_CE, rel, 3);
                    LOG_INF("run_native_ce_session: routes already "
                            "sent, acked token request");
                }
                t_start = time(NULL);
                continue;
            }

            if (r == CE_FRAME_STATUS_NEG) {
                LOG_INF("run_native_ce_session: status '3-' — "
                        "end of routing batch");
                t_start = time(NULL);
                continue;
            }

            if (r == CE_FRAME_STATUS_10) {
                LOG_DBG("run_native_ce_session: status '10'");
                t_start = time(NULL);
                continue;
            }

            if (r == CE_FRAME_COMPACT) {
                /* Multi-entry compact records: CALL(6)+SSID_LO+SSID_HI+RTT */
                DestEntry entries[64];
                int n = ce_parse_compact_records(buf, len, entries, 64);
                for (int i = 0; i < n; i++) {
                    if (dtable_merge(&entries[i]) > 0)
                        merged_total++;
                }
                LOG_INF("run_native_ce_session: compact frame — "
                        "%d entries parsed, %d total merged",
                        n, merged_total);

                /* M5.3: some peers (notably xnet) respond to our type-6
                 * REQUEST with a type-3 compact record rather than a
                 * type-7 REPLY.  When that happens, the type-3 contains
                 * the destination we asked about.  Match any entry in
                 * this frame against pending queries by target callsign;
                 * if found, clear the pending slot so the timeout does
                 * not fire and no spurious "UNSOLICITED" warning is
                 * logged later.  This gives us reachability confirmation
                 * (but not full path info — xnet doesn't support that). */
                for (int i = 0; i < n; i++) {
                    DestEntry *ent = &entries[i];
                    for (int p = 0; p < CE_PATH_MAX_PENDING; p++) {
                        if (!g_path_pending[p].active) continue;
                        /* match by base callsign (ignore SSID variations) */
                        char pbase[MAX_CALLSIGN_LEN];
                        snprintf(pbase, sizeof(pbase), "%s",
                                 g_path_pending[p].target);
                        char *dash = strchr(pbase, '-');
                        if (dash) *dash = '\0';
                        if (strcasecmp(pbase, ent->callsign) == 0) {
                            LOG_INF("path_pending: type-3 satisfies "
                                    "pending probe slot=%d qso=%d "
                                    "target=%s (peer lacks type-7, "
                                    "only reachability confirmed)",
                                    p, g_path_pending[p].qso,
                                    g_path_pending[p].target);
                            path_pending_remove(g_path_pending[p].qso);
                            break;
                        }
                    }
                }

                /* Write destinations periodically (every 60s) so
                 * uronode sees fresh data while the session is alive.
                 * Without this, the file is only written on disconnect. */
                time_t now = time(NULL);
                if (merged_total > 0 && now - last_dest_write >= 60) {
                    output_write_destinations();
                    last_dest_write = now;
                    LOG_INF("run_native_ce_session: destinations "
                            "written (%d reachable)",
                            dtable_count_reachable());
                }

                t_start = time(NULL);
                continue;
            }

            LOG_DBG("run_native_ce_session: CE '3' frame type=%d", r);
            t_start = time(NULL);
            continue;
        }

        /* CF L3RTT probe/reply — echo back with our timing counters
         * so the peer can compute the round-trip quality.
         *
         * Probe:  c1=peer_send_time  c2=0  c3=0  c4=0
         * Reply:  c1=peer_send_time  c2=0  c3=our_recv  c4=our_send
         *
         * The peer computes: RTT = (recv_back - c1) - (c4 - c3)
         */
        {
            uint32_t c1 = 0, c2 = 0, c3 = 0, c4 = 0, max_dest = 0;
            int lt = 0;
            char alias[MAX_ALIAS_LEN] = {0};
            char ver[16] = {0};

            if (cf_parse_l3rtt(buf, len, &c1, &c2, &c3, &c4,
                               &lt, alias, ver, &max_dest) == 0) {
                uint32_t recv_tick = get_uptime_ticks();
                LOG_INF("run_native_ce_session: CF L3RTT probe "
                        "c1=%u c2=%u c3=%u c4=%u $M=%u alias=%s",
                        c1, c2, c3, c4, max_dest, alias);

                /* Build reply: keep original c1/c2, fill c3/c4.
                 *
                 * L3RTT val1/val2 semantics (confirmed from Phase 3
                 * disruption capture):
                 *   c3=0, c4=0  when $M=60000 (link down, no routes)
                 *   c3=recv,c4=send  when link active (routes present)
                 * Incorrect non-zero values during link-down impair
                 * Q/T convergence at the peer. */
                uint32_t send_tick = get_uptime_ticks();
                int reachable = dtable_count_reachable();
                uint32_t reply_c3 = (reachable > 0) ? recv_tick : 0;
                uint32_t reply_c4 = (reachable > 0) ? send_tick : 0;

                LOG_INF("run_native_ce_session: L3RTT reply "
                        "reachable=%d c3=%u c4=%u",
                        reachable, reply_c3, reply_c4);

                uint8_t reply[256];
                int rlen = cf_build_l3rtt(reply, (int)sizeof(reply),
                                          c1, c2, reply_c3, reply_c4,
                                          g_cfg.alias,
                                          LEVEL3_VERSION_STR,
                                          (uint32_t)reachable);
                if (rlen > 0) {
                    ax25_send(fd, PID_CF, reply, rlen);
                    LOG_INF("run_native_ce_session: L3RTT reply sent "
                            "c3=%u c4=%u $M=%d",
                            reply_c3, reply_c4, reachable);
                }
                t_start = time(NULL);
                continue;
            }
        }

        /* CF D-table line */
        {
            DestEntry e;
            char line[256];
            int  li = len < 254 ? len : 254;
            memcpy(line, buf, (size_t)li);
            line[li] = '\0';

            if (cf_parse_dtable_line(line, &e) == 0) {
                LOG_DBG("run_native_ce_session: CF D-table %s %d-%d rtt=%d",
                        e.callsign, e.ssid_lo, e.ssid_hi, e.rtt);
                if (dtable_merge(&e) > 0) merged_total++;
                t_start = time(NULL);
                continue;
            }
        }

        LOG_DBG("run_native_ce_session: unrecognised frame "
                "first=0x%02X len=%d — ignored", buf[0], len);
    }

    LOG_INF("run_native_ce_session: done — "
            "frames=%d merged=%d table=%d keepalives=%d "
            "link_time=%ldms last_token=%d",
            frame_count, merged_total, g_table.count, keepalive_count,
            link_time_ms, last_token);

    /* Write final linkstats snapshot, then mark inactive */
    g_link_stats.dst_count = dtable_count_reachable();
    output_write_linkstats();
    g_link_stats.active = 0;

    if (merged_total > 0 || g_table.count > 0) {
        output_write_destinations();
        LOG_INF("run_native_ce_session: output files written");
    }

    return 0;
}

/* ── poll_cycle_run ───────────────────────────────────────────────────── */
/*
 * mode: 0 = auto (fd==0 → native, fd>0 → D-cmd)
 *       1 = force native CE/CF session (server mode accept)
 *       2 = force D-command mode (client mode connect)
 */
int poll_cycle_run(int fd)
{
    return poll_cycle_run_mode(fd, 0);
}

int poll_cycle_run_mode(int fd, int mode)
{
    const char *mode_str = (mode == 1) ? "native(forced)"
                         : (mode == 2) ? "D-cmd(forced)"
                         : (fd == 0)   ? "native(stdin)"
                         :               "D-cmd(socket)";

    LOG_INF("poll_cycle_run: === START (fd=%d %s) ===", fd, mode_str);

    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    int use_native;
    if      (mode == 1) use_native = 1;
    else if (mode == 2) use_native = 0;
    else                use_native = (fd == 0);

    int r = use_native
            ? run_native_ce_session(fd)
            : run_dcommand_cycle(fd);

    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    int64_t ms = (int64_t)(t_end.tv_sec - t_start.tv_sec) * 1000
               + (t_end.tv_nsec - t_start.tv_nsec) / 1000000;
    LOG_INF("poll_cycle_run: === DONE in %lld ms (r=%d) ===",
            (long long)ms, r);

    return r;
}

/* ── send_ce_keepalive ───────────────────────────────────────────────── */
int send_ce_keepalive(int fd)
{
    uint8_t buf[CE_KEEPALIVE_LEN];
    int len = ce_build_keepalive(buf, sizeof(buf));
    if (len < 0) return -1;

    LOG_DBG("send_ce_keepalive: sending %d bytes on fd=%d", len, fd);
    if (ax25_send(fd, PID_CE, buf, len) < 0) {
        LOG_ERR("send_ce_keepalive: failed");
        return -1;
    }
    LOG_INF("send_ce_keepalive: sent");
    return 0;
}
