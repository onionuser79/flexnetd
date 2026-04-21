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
 * mode = 0 → record only (no '3+', no '3-')
 *          Used for periodic refresh (M6.7).  The compact record frame
 *          is processed unconditionally by peer (no state check), so
 *          this path never triggers any token-state machine transitions.
 *          This is the safest option for keeping our route fresh in
 *          the peer's table without disturbing its token state.
 *
 * mode = 1 → record + '3-' (reply-mode, no '3+')
 *          Used when replying to the peer's '3+' request.  The peer
 *          has already taken the token; we send our records and close
 *          with '3-' so the peer can release and resume its own
 *          send phase.
 *
 * mode = 2 → '3+' + record + '3-' (full we-initiate)
 *          Reserved for future use (currently no caller).  Only valid
 *          when the peer's token state is 0 — which we cannot observe
 *          remotely, so we no longer use this mode.
 *
 * Rationale (M6.9.4 — observed 2026-04-19):
 *   Session 717 showed that even AFTER M6.9.3 (reply-without-'3+'),
 *   our M6.7 periodic re-advertisement still sent '3+' every 60 s.
 *   PCFlexnet's token state after the earlier exchange was 3 (not 0),
 *   so our '3+' hit its reject path and pcf DM'd the link ~2 s later.
 *   Session lived only 1 min 07 s before DISC.
 *
 * PCFlexnet '3+' acceptance rule: the '+' handler only accepts a
 * '3+' when the peer's internal token state is 0; otherwise it
 * rejects and eventually issues DM.  We cannot observe the peer's
 * token state remotely, so we never send '3+'.  For periodic refresh,
 * plain compact records (no token) are always safe.
 *
 * Port-aware (M6.7): when g_port_idx is valid, picks the listen_call and
 * SSID range from g_cfg.ports[g_port_idx] so each CE child advertises
 * with the correct port-local identity.  Both peers (IW2OHX-14 on xnet
 * and IW2OHX-12 on pcf) therefore see our callsign (IW2OHX-3) re-advertised
 * on their respective links.  Falls back to legacy flat fields when no
 * port context is set (e.g. pipe-mode handler via ax25d).
 */
#define SEND_ROUTES_RECORD_ONLY   0
#define SEND_ROUTES_REPLY_CLOSE   1
#define SEND_ROUTES_FULL_INITIATE 2

static int send_own_routes(int fd, int mode)
{
    /* Pick the source callsign (listen_call) and SSID range for THIS port. */
    const char *listen_call = NULL;
    int         min_ssid    = g_cfg.min_ssid;
    int         max_ssid    = g_cfg.max_ssid;

    if (g_port_idx >= 0 && g_port_idx < g_cfg.num_ports) {
        const PortCfg *pc = &g_cfg.ports[g_port_idx];
        if (pc->listen_call[0]) listen_call = pc->listen_call;
        if (pc->min_ssid || pc->max_ssid) {
            min_ssid = pc->min_ssid;
            max_ssid = pc->max_ssid;
        }
    }
    if (!listen_call || !listen_call[0])
        listen_call = g_cfg.flex_listen_call[0] ? g_cfg.flex_listen_call
                                                : g_cfg.mycall;

    char base[MAX_CALLSIGN_LEN] = {0};
    int  dummy_ssid = 0;
    callsign_parse_ssid(listen_call, base, &dummy_ssid);

    /* Request token — only in FULL_INITIATE mode (currently unused) */
    if (mode == SEND_ROUTES_FULL_INITIATE) {
        uint8_t req[] = { '3', '+', '\r' };
        ax25_send(fd, PID_CE, req, 3);
    }

    /* Our route via ce_build_record (single wire-format source) */
    uint8_t route[64];
    int rlen = ce_build_record(route, sizeof(route),
                               base, min_ssid, max_ssid,
                               1, 0);  /* RTT=1 (local/direct), not indirect */
    if (rlen > 0)
        ax25_send(fd, PID_CE, route, rlen);

    /* Release token — only when closing an active exchange (REPLY_CLOSE
     * or FULL_INITIATE).  RECORD_ONLY mode omits it to avoid touching
     * the peer's state machine on periodic refreshes. */
    if (mode != SEND_ROUTES_RECORD_ONLY) {
        uint8_t rel[] = { '3', '-', '\r' };
        ax25_send(fd, PID_CE, rel, 3);
    }

    const char *port_name = (g_port_idx >= 0 && g_port_idx < g_cfg.num_ports)
                            ? g_cfg.ports[g_port_idx].name
                            : g_cfg.port_name;
    const char *mode_str = (mode == SEND_ROUTES_RECORD_ONLY) ? "record only (refresh)"
                         : (mode == SEND_ROUTES_REPLY_CLOSE) ? "record + '3-' (reply)"
                         :                                     "full '3+'/record/'3-'";
    LOG_INF("send_own_routes: port=%s advertised %s SSID %d-%d RTT=1 [%s]",
            port_name, base, min_ssid, max_ssid, mode_str);
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

    /*
     * v0.7.1 fix — scrub any stale per-port destinations file left
     * behind by a previous CE session on this port.  Without this,
     * the M6.5 merge reads OLD routes from the on-disk file and
     * presents them in the unified destinations as if they came from
     * the current session.  Symptom: `fl` says dst=0 via this peer
     * but `fld` lists 100+ routes Via this peer — from the previous
     * session that did receive routes.
     *
     * Truncating to an empty file (rather than unlink) keeps the
     * node/ownership stable; the next output_write_destinations()
     * from this child rewrites it with fresh data.
     */
    if (g_port_idx >= 0 && g_port_idx < g_cfg.num_ports) {
        char per_port[MAX_PATH_LEN + 24];
        snprintf(per_port, sizeof(per_port), "%s.%s",
                 g_cfg.dest_file, g_cfg.ports[g_port_idx].name);
        FILE *pf = fopen(per_port, "w");
        if (pf) {
            fclose(pf);
            LOG_DBG("run_native_ce_session: truncated stale per-port "
                    "file '%s'", per_port);
        }
    }

    int     got_setup    = 0;
    int     merged_total = 0;
    time_t  t_start      = time(NULL);
    int     keepalive_count = 0;
    int     frame_count  = 0;
    long    link_time_ms = 0;       /* from CE type-1 link time frames */
    int     last_token   = 0;       /* from CE type-4 seq frames (our value last advertised to peer) */
    /* v0.7.2 added proactive type-4 TX tracking with these locals;
     * v0.7.7 disabled the TX after production xnet V1.39 was found
     * to label type-4 as "unknown packet type" and withdraw routes
     * ~20 s later.  Locals retained (unused) for easy reactivation
     * once a V2.1+ peer is available to test against. */
    int      last_seen_dest_count = -1;
    unsigned route_seq            = 1;
    time_t   last_seq_tx          = 0;
    int     got_peer_init = 0;      /* received peer's init handshake   */
    int     sent_routes   = 0;      /* advertised our routes to peer    */
    time_t  last_dest_write = 0;    /* last time we wrote destinations  */
    time_t  last_stats_write = 0;   /* last time we wrote linkstats     */
    time_t  last_probe_sent  = 0;   /* last time we sent a type-6 probe */
    int     probe_idx        = 0;   /* round-robin dtable index         */
    time_t  last_keepalive_tx = time(NULL);   /* proactive keepalive timer */
    time_t  last_routes_tx   = 0;      /* M6.7: last time we sent 3+/route/3- */
    /*
     * M6.9.1: seed last_lt_tx to (now - interval) so that the FIRST
     * link-time send (typically during the init handshake reply to
     * the peer's initial '1' frame) passes the rate-limit gate, but
     * any SUBSEQUENT send within the same interval window is blocked.
     *
     * Without this seed, peer-side init sequences that bounce 2-3 '1'
     * frames back to back all got through (last_lt_tx=0 produced
     * "now - 0 = huge >> interval" for every call in the first second).
     * Those rapid-fire replies were exactly what caused the 4095
     * sample we still see pinning the avg high post-convergence.
     */
    time_t  last_lt_tx       = (g_cfg.lt_reply_interval > 0)
                                ? time(NULL) - g_cfg.lt_reply_interval
                                : 0;
    /*
     * v0.7.1 timing fix: non-drifting link-time scheduler.
     *
     * Before: we gated on `now - last_lt_tx >= interval`.  Since the
     * proactive timer only fires every 20 s, and last_lt_tx absorbs
     * any late-fire drift (`last_lt_tx = now` after the send), each
     * cycle can stretch from 320 s up to ~340 s.  At pcf, that 10-20 s
     * slippage produces `delta = actual_interval - 320 = 10-20 s`
     * samples (= 100-200 ticks) pinning its displayed RTT well above
     * the ideal 1 tick.
     *
     * After: we track `next_lt_tx` as the absolute wall-clock time
     * when the next link-time should fire.  On each send we advance
     * `next_lt_tx += interval` (and catch up past `now` if we're late).
     * This eliminates cumulative drift — sends land exactly one
     * interval apart on the wall clock, independent of when the 20 s
     * proactive timer happens to tick.
     *
     * Initial schedule: next_lt_tx = now, so the first send (during
     * init handshake) passes immediately, same as the previous seed.
     */
    time_t  next_lt_tx       = time(NULL);
    struct timespec lt_sent_ts = {0};  /* when we last sent a link-time frame */
    int     lt_sent_pending = 0;       /* 1 = waiting for peer's link-time reply */

    /* v0.7.3: effective per-port lt_reply_interval.
     *
     * A port's PortCfg.lt_reply_interval overrides the global
     * LinkTimeReplyInterval.  Use a small value (e.g. 20) on xnet ports
     * so peer's smoothed RTT measurement converges, and keep the
     * global default (320) on PCFlexnet ports where a short interval
     * would saturate pcf's RTT field at 4095.  See PROTOCOL_SPEC.md §2.4
     * and the linbpq-flexnet reference implementation which uses 0
     * (reply on every keepalive) as the xnet-friendly pattern. */
    int     effective_lt_reply;
    if (g_port_idx >= 0 && g_port_idx < g_cfg.num_ports &&
        g_cfg.ports[g_port_idx].lt_reply_interval >= 0) {
        effective_lt_reply = g_cfg.ports[g_port_idx].lt_reply_interval;
        LOG_INF("run_native_ce_session: port '%s' lt_reply=%d s (port override)",
                g_cfg.ports[g_port_idx].name, effective_lt_reply);
    } else {
        effective_lt_reply = g_cfg.lt_reply_interval;
        LOG_INF("run_native_ce_session: lt_reply=%d s (global)",
                effective_lt_reply);
    }

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

            /* v0.7.7: CE type-4 routing-seq TX is DISABLED.
             *
             * v0.7.2 added a proactive type-4 TX when our dtable count
             * changes, based on RE of the xnet `linuxnet` V2.1 binary
             * which includes a type-4 dispatcher handler (slot 4 of the
             * jump table at rodata 0x0808fca4).
             *
             * Production xnet is V1.39 (per "(X)NET/DLC7 V1.39" banner),
             * which does NOT have a type-4 handler.  Live monitor
             * captures our type-4 frame as "FlexNet: unknown packet
             * type" — and ~20 seconds later xnet withdraws every route
             * it just advertised to us.  The linbpq-flexnet reference
             * (running on IW2OHX-13 and keeping 60+ routes stable for
             * 8+ days with xnet) never emits type-4.
             *
             * Removing our proactive type-4 TX matches linbpq-flexnet's
             * behaviour.  Routes advertised by xnet should now persist.
             *
             * We still PARSE received type-4 in ce_parse_frame() —
             * harmless, and keeps us forward-compatible with V2.1
             * peers that do emit it. */
            (void)route_seq;              /* counter still tracked for logs / future use */
            (void)last_seen_dest_count;
            (void)last_seq_tx;

            /* Proactive CE keepalive timer.  Some peers (e.g. PCFlexnet
             * on the pcf port) do NOT send CE keepalives autonomously —
             * they expect us to initiate and respond to theirs.  If the
             * peer is silent we would never send either, the RTT timer
             * on the peer's side expires (4095 sample), and the session
             * gets torn down and reconnected.
             *
             * Send a keepalive every 20 s since the last one we sent.
             * (X)Net (which DOES send keepalives of its own — every
             * 180 s per spec) still works because our extra ones are
             * harmless — it just responds.
             *
             * We also send a link-time probe after the keepalive so the
             * peer's RTT cycle has something to measure against. */
            if (now - last_keepalive_tx >= 20) {
                LOG_DBG("run_native_ce_session: proactive keepalive "
                        "(quiet %lds)", (long)(now - last_keepalive_tx));
                send_ce_keepalive(fd);
                g_link_stats.tx_bytes += CE_KEEPALIVE_LEN;
                g_link_stats.tx_frames++;

                /* M6.9: rate-limit link-time frames.
                 *
                 * PCFlexnet sets its internal expected-reply timestamp to
                 *   link.ts = now + (smoothed+4)*32   (if smoothed < 96)
                 *   link.ts = now + 3200              (otherwise)
                 *
                 * Ticks are 100ms, so that's now + 12.8s  to  now + 320s.
                 * If our link-time arrives BEFORE link.ts, PCFlexnet
                 * computes delta = now - link.ts = NEGATIVE, which wraps
                 * and clamps to 4095 (its 12-bit RTT field cap).
                 * That's exactly what we saw: pcf.delay saturates at 4095.
                 *
                 * (X)Net doesn't hit this because it initiates its own
                 * link-time exchanges on its cycle.  PCFlexnet only
                 * replies — so WE must respect the interval. */
                if (effective_lt_reply <= 0 || now >= next_lt_tx)
                {
                    uint8_t lt_buf[32];
                    /* Link-time value carries our smoothed RTT.  We
                     * hard-code 2 (= 200 ms wire) because our actual
                     * smoothed measurement is not yet implemented; this
                     * matches linbpq-flexnet's value and is accepted
                     * by both (X)Net and PCFlexnet. */
                    int lt_len = ce_build_link_time(lt_buf, sizeof(lt_buf), 2);
                    if (lt_len > 0) {
                        ax25_send(fd, PID_CE, lt_buf, lt_len);
                        g_link_stats.tx_bytes += lt_len;
                        g_link_stats.tx_frames++;
                        clock_gettime(CLOCK_MONOTONIC, &lt_sent_ts);
                        lt_sent_pending = 1;
                        last_lt_tx = now;
                        /* Advance schedule by one interval.  If we fell
                         * behind (now well past next_lt_tx), skip forward
                         * to the next future slot — we don't burst. */
                        if (effective_lt_reply > 0) {
                            next_lt_tx += effective_lt_reply;
                            while (next_lt_tx <= now)
                                next_lt_tx += effective_lt_reply;
                        }
                        LOG_DBG("run_native_ce_session: proactive link-time "
                                "sent (interval=%ds, next at +%lds)",
                                effective_lt_reply,
                                (long)(next_lt_tx - now));
                    }
                } else {
                    LOG_DBG("run_native_ce_session: skipping proactive "
                            "link-time (%lds until next scheduled send)",
                            (long)(next_lt_tx - now));
                }
                last_keepalive_tx = now;
            }

            /* M6.7: periodic re-advertisement of our routes.
             *
             * FlexNet peers (xnet, PCFlexnet) age out type-3 route records
             * after roughly 60–120 s if not refreshed.  Without periodic
             * re-advertisement, our own callsign (IW2OHX-3) drops out of
             * the peer's destination table after ~2 min — observed on
             * IW2OHX-14 as "dst via IW2OHX-3 = 0" while the link stays up.
             *
             * This also affects M2 transit: nodes upstream of the peer
             * that ask "how do I reach IW2OHX-3?" get an unreachable
             * reply once our route has aged out.
             *
             * The re-advert runs per-CE-child, so IW2OHX-14 (xnet) and
             * IW2OHX-12 (pcf) each receive their own periodic refresh.
             * send_own_routes() is port-aware and advertises using the
             * port's listen_call and SSID range.
             *
             * Only starts after the initial advertisement (sent_routes=1)
             * so we don't race the handshake.  Interval=0 disables
             * re-advertisement (once-only, legacy behaviour). */
            /* v0.7.1 Priority 2: per-port route_advert_interval override.
             * Evidence from 2026-04-20 capture: even M6.9.4 "record-only"
             * re-advertisement triggers PCFlexnet to DM the L2 link
             * within 10-15 ms.  After PCFlexnet processes ANY compact
             * record it checks its token state — if 0 (idle), it tears
             * down the link.
             *
             * (X)Net doesn't have this behavior, so the per-port knob
             * lets us enable M6.7 for xnet (where it prevents xnet aging
             * our route out) and disable it for pcf (where it kills
             * links). */
            int port_advert = -1;
            if (g_port_idx >= 0 && g_port_idx < g_cfg.num_ports)
                port_advert = g_cfg.ports[g_port_idx].route_advert_interval;
            int effective_advert = (port_advert >= 0)
                                   ? port_advert
                                   : g_cfg.route_advert_interval;

            if (sent_routes && effective_advert > 0 &&
                (now - last_routes_tx >= effective_advert))
            {
                LOG_DBG("run_native_ce_session: periodic route re-advert "
                        "(%lds since last, interval=%ds)",
                        (long)(now - last_routes_tx), effective_advert);
                /* M6.9.4: record-only refresh.  No '3+' (peer state may
                 * not be 0 → would trigger DM).  No '3-' either (no
                 * state transition to signal).  The bare compact record
                 * is processed unconditionally by peer. */
                send_own_routes(fd, SEND_ROUTES_RECORD_ONLY);
                last_routes_tx = now;
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

        /* Keepalive: '2' + N spaces (PROTOCOL_SPEC.md §2.5).
         * (X)Net uses N=240 (241 bytes total), PCFlexnet uses N=200
         * (201 bytes total) — both all-spaces, no trailer.  Accept any
         * length ≥ 2 whose body after the '2' is pure space. */
        int is_keepalive = 0;
        if (len >= 2 && buf[0] == '2') {
            is_keepalive = 1;
            for (int kai = 1; kai < len; kai++) {
                if (buf[kai] != ' ') { is_keepalive = 0; break; }
            }
        }
        if (is_keepalive) {
            keepalive_count++;
            LOG_INF("run_native_ce_session: CE keepalive #%d", keepalive_count);
            send_ce_keepalive(fd);
            g_link_stats.tx_bytes += CE_KEEPALIVE_LEN;
            g_link_stats.tx_frames++;
            /* Reset proactive-keepalive timer — we just sent one. */
            last_keepalive_tx = time(NULL);

            /* M6.9 + v0.7.1 non-drift: piggy-back a link-time send here
             * ONLY if the scheduled send time has arrived.  Uses the
             * same `next_lt_tx` wall-clock schedule as the proactive
             * timer branch so link-times never fire twice per window. */
            {
                time_t now = time(NULL);
                if (effective_lt_reply <= 0 || now >= next_lt_tx)
                {
                    uint8_t lt_buf[32];
                    int lt_len = ce_build_link_time(lt_buf,
                                    (int)sizeof(lt_buf), 2);
                    if (lt_len > 0) {
                        ax25_send(fd, PID_CE, lt_buf, lt_len);
                        g_link_stats.tx_bytes += lt_len;
                        g_link_stats.tx_frames++;
                        clock_gettime(CLOCK_MONOTONIC, &lt_sent_ts);
                        lt_sent_pending = 1;
                        last_lt_tx = now;
                        if (effective_lt_reply > 0) {
                            next_lt_tx += effective_lt_reply;
                            while (next_lt_tx <= now)
                                next_lt_tx += effective_lt_reply;
                        }
                    }
                } else {
                    LOG_DBG("run_native_ce_session: skipping link-time "
                            "after peer keepalive (%lds until next scheduled)",
                            (long)(next_lt_tx - now));
                }
            }

            /* After first keepalive exchange with peer, the link is
             * stable — proactively advertise our routes.  The peer
             * does not send '3+' (request token) unprompted, so we
             * initiate the route advertisement ourselves.
             *
             * v0.7.8: use per-port advert_mode to decide whether to
             * wrap in '3+' ... '3-' (token exchange) or emit record-
             * only.  Default is FULL (xnet-friendly, required for
             * xnet's token-exchange cycle to complete and for its
             * smoothed-RTT loop to converge).  PCFlexnet ports should
             * explicitly set advert_mode=record to avoid the pcf DM
             * path on unsolicited '3+' (M6.9.4). */
            if (!sent_routes && got_peer_init && got_setup) {
                int port_advert_mode = ADVERT_MODE_FULL;   /* default */
                if (g_port_idx >= 0 && g_port_idx < g_cfg.num_ports &&
                    g_cfg.ports[g_port_idx].advert_mode >= 0) {
                    port_advert_mode = g_cfg.ports[g_port_idx].advert_mode;
                }
                int send_mode = (port_advert_mode == ADVERT_MODE_FULL)
                                ? SEND_ROUTES_FULL_INITIATE
                                : SEND_ROUTES_RECORD_ONLY;
                LOG_INF("run_native_ce_session: link stable — "
                        "sending our routes (%s)",
                        send_mode == SEND_ROUTES_FULL_INITIATE
                            ? "full '3+'/record/'3-' exchange"
                            : "record only, pcf-safe");
                send_own_routes(fd, send_mode);
                sent_routes    = 1;
                last_routes_tx = time(NULL);   /* M6.7: seed re-advert timer */
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

        /* Link time: '1' prefix + decimal + '\r' (PROTOCOL_SPEC.md §2.4).
         * Accept any length ≥ 3 — "10\r"/"11\r"/"12\r" are valid
         * type-1 frames with decimal values 0/1/2. */
        if (buf[0] == '1' && len >= 3) {
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

                /* v0.7.3: reply to peer's link-time inline, gated by
                 * the effective per-port lt_reply_interval.
                 *
                 * Historical note (M6.9.2, v0.7.0): this inline path
                 * was REMOVED as a pcf-specific optimisation — the
                 * reply fired on peer-frame arrival (arbitrary phase),
                 * not on a clean 320 s boundary, producing delta ≈ 100
                 * tick samples in pcf's RTT history instead of the
                 * clean "1" samples we wanted.
                 *
                 * That fix turned out to be too blunt: xnet peers need
                 * the inline reply (matching their own ~20 s keepalive
                 * cadence) or their smoothed RTT for us stays stuck at
                 * the 60000 infinity sentinel and their `L` display
                 * shows Q=301, RTT=600/2.  Linbpq-flexnet (which works
                 * cleanly with xnet) ALWAYS replies inline.
                 *
                 * v0.7.3 restores the inline reply but gates it by
                 * next_lt_tx, so on pcf (interval=320) it still fires
                 * only once per 320 s window, while on xnet (interval
                 * ≤ 20 or 0) it fires on every peer keepalive cycle. */
                time_t now_lt = time(NULL);
                if (effective_lt_reply <= 0 || now_lt >= next_lt_tx) {
                    uint8_t lt_buf[32];
                    int lt_len = ce_build_link_time(lt_buf,
                                    (int)sizeof(lt_buf), 2);
                    if (lt_len > 0) {
                        ax25_send(fd, PID_CE, lt_buf, lt_len);
                        g_link_stats.tx_bytes += lt_len;
                        g_link_stats.tx_frames++;
                        clock_gettime(CLOCK_MONOTONIC, &lt_sent_ts);
                        lt_sent_pending = 1;
                        last_lt_tx = now_lt;
                        if (effective_lt_reply > 0) {
                            next_lt_tx += effective_lt_reply;
                            while (next_lt_tx <= now_lt)
                                next_lt_tx += effective_lt_reply;
                        }
                        LOG_DBG("run_native_ce_session: replied inline to "
                                "peer link-time (interval=%ds)",
                                effective_lt_reply);
                    }
                } else {
                    LOG_DBG("run_native_ce_session: peer link-time received, "
                            "reply gated (%lds until next scheduled)",
                            (long)(next_lt_tx - now_lt));
                }
            }
            t_start = time(NULL);
            continue;
        }

        /* Type-4 routing-seq gossip: '4' + decimal + '\r' (PROTOCOL_SPEC.md §2.7).
         *
         * Per spec, the RX handler parses the decimal value and stores
         * it as the peer's current sequence.  No reply, no echo.
         * Purpose is purely informational: when the peer sees a seq
         * higher than what it last requested routes for, it knows to
         * issue a '3+' to pull fresh records.
         *
         * Previous flexnetd versions echoed the frame back — that
         * caused unnecessary RF churn and, worse, PCFlexnet
         * interpreted the echo as "your routes changed" and re-ran
         * its own '3+' gate.  v0.7.2: drop the echo. */
        if (buf[0] == '4') {
            int token_val = 0;
            /* ce_parse_frame writes the (now unused) flag char into
             * callsign_out[0]; pass NULL since we don't need it. */
            int r = ce_parse_frame(buf, len, NULL, NULL, &token_val);
            if (r == CE_FRAME_TOKEN) {
                last_token = token_val;
                LOG_INF("run_native_ce_session: peer routing-seq=%u "
                        "(stored, no reply)", (unsigned)token_val);
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
                /* '3+' = "request token" — peer is asking us to send
                 * our routes.  Reply with records + '3-', but do NOT
                 * echo '3+' back.
                 *
                 * M6.9.3 bug fix: previously we sent '3+' + records + '3-'
                 * in both branches.  PCFlexnet's '+' handler only
                 * accepts '3+' when its internal token state is 0, but
                 * the peer set that state to non-zero when IT sent its
                 * '3+'.  Our echo therefore hit pcf's reject path and
                 * pcf DISCONNECTED the link within 2 ms of our '3-'.
                 * Captured 2026-04-19 21:43:12.543 → 21:43:12.549 in
                 * the "L 2 iw2ohx-3" trace.
                 *
                 * Always respond to peer's '3+' with records + '3-' only.
                 * Update the sent_routes / last_routes_tx bookkeeping so
                 * the M6.7 re-advert timer doesn't fire redundantly. */
                LOG_INF("run_native_ce_session: '3+' received from peer "
                        "— replying with records + '3-' (no '3+' echo)");
                send_own_routes(fd, SEND_ROUTES_REPLY_CLOSE);  /* record + '3-' */
                sent_routes    = 1;
                last_routes_tx = time(NULL);
                t_start = time(NULL);
                continue;
            }

            if (r == CE_FRAME_STATUS_NEG) {
                LOG_INF("run_native_ce_session: status '3-' — "
                        "end of routing batch");
                t_start = time(NULL);
                continue;
            }

            /* Note: CE_FRAME_STATUS_10 was removed in v0.7.2 after the
             * protocol spec was corrected to classify '10\r' as an
             * ordinary CE_FRAME_LINK_TIME with decimal value 0 (a
             * legitimate link-time reply, not a
             * distinct "status 10" frame).  It now falls through into
             * the LINK_TIME path above. */

            if (r == CE_FRAME_COMPACT) {
                /* Multi-entry compact records: CALL(6)+SSID_LO+SSID_HI+RTT.
                 * Pass g_port_idx so each entry is tagged with the port it
                 * arrived on (v0.7.1 — fixes "Via always IW2OHX-14" bug). */
                DestEntry entries[64];
                int n = ce_parse_compact_records(buf, len, entries, 64,
                                                 g_port_idx);
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

                /* Write destinations after every compact-record batch.
                 *
                 * HISTORY (v0.3.0 - v0.7.3): this write was rate-limited
                 * to once per 60 s.  That was benign while the xnet peer
                 * advertised routes in a slow trickle, but became
                 * load-bearing in exactly the wrong way once the session
                 * setup was fixed enough for xnet to dump its entire
                 * 100-120-entry dtable in a 20-30 s initial burst:
                 * only the first ~20 records reached disk, the next
                 * 5 batches were merged into g_table in memory but
                 * never flushed.  Since xnet then goes silent (normal
                 * behaviour — routes are advertised once at session
                 * setup), the gate never reopened.  Users saw only 20
                 * entries in `fld` for the entire session lifetime.
                 *
                 * Fix (v0.7.4): write on every batch.  Cost is ~20 KB
                 * of I/O per compact frame — negligible — and after the
                 * initial burst xnet stops sending compact records, so
                 * the writes naturally stop too. */
                if (n > 0) {
                    output_write_destinations();
                    last_dest_write = time(NULL);
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
