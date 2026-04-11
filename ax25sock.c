/*
 * ax25sock.c — AX.25 L2 session management
 *
 * SERVER MODE: ax25_listen() + ax25_accept() for native FlexNet peering.
 *   Bind a dedicated SSID (FlexListenCall, e.g. IW2OHX-9) not owned by ax25d.
 *   IW2OHX-14 connects directly; we receive CE/CF frames as a real socket
 *   (no ax25d pipe in the way — full PID access).
 *
 * CLIENT MODE: ax25_connect() for outbound D-command polling.
 *
 * PIPE MODE detection: fd_is_socket() via fstat() — used when ax25d
 *   spawns us via -s flag (legacy, kept for compatibility).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <net/if.h>

#include <netax25/ax25.h>
#include <netax25/axlib.h>

#include "flexnetd.h"

#ifndef AX25_PID
#define AX25_PID  10
#endif

#ifndef AX25_PIDINCL
#define AX25_PIDINCL  1
#endif

/*
 * Enable AX25_PIDINCL on a connected socket.
 *
 * With PIDINCL enabled:
 *   send(): first byte of data = PID for the I-frame L2 header
 *   recv(): PID byte is prepended to received data
 *
 * This is the only reliable way to send non-F0 PIDs on Linux
 * SEQPACKET sockets. setsockopt(AX25_PID) silently fails on
 * many kernels.
 */
int ax25_enable_pidincl(int fd)
{
    int flag = 1;
    if (setsockopt(fd, SOL_AX25, AX25_PIDINCL,
                   &flag, sizeof(flag)) < 0) {
        LOG_ERR("ax25_enable_pidincl: setsockopt(AX25_PIDINCL): %s",
                strerror(errno));
        return -1;
    }
    LOG_INF("ax25_enable_pidincl: enabled on fd=%d", fd);
    return 0;
}

#define FLEXNETD_AX25_MAX_DIGIS  8
typedef struct {
    ax25_address  port_addr;
    ax25_address  dest_addr;
    unsigned char digi_count;
    ax25_address  digi_addr[FLEXNETD_AX25_MAX_DIGIS];
} flexnetd_ax25_route_t;

#ifndef SIOCADDRT
#define SIOCADDRT  0x890B
#endif

/* ── axports parser ──────────────────────────────────────────────────── */
static int axports_get_callsign(const char *axports_file,
                                const char *port_name,
                                char *buf, int buflen)
{
    FILE *f = fopen(axports_file, "r");
    if (!f) {
        LOG_ERR("axports_get_callsign: cannot open '%s': %s",
                axports_file, strerror(errno));
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0' || *p == '\n') continue;

        char name[32] = {0};
        char call[MAX_CALLSIGN_LEN] = {0};
        if (sscanf(p, "%31s %9s", name, call) < 2) continue;

        if (strcasecmp(name, port_name) == 0) {
            fclose(f);
            for (char *c = call; *c; c++)
                *c = (char)toupper((unsigned char)*c);
            snprintf(buf, (size_t)buflen, "%s", call);
            LOG_DBG("axports_get_callsign: port '%s' -> '%s'",
                    port_name, buf);
            return 0;
        }
    }

    fclose(f);
    LOG_ERR("axports_get_callsign: port '%s' not found in '%s'",
            port_name, axports_file);
    return -1;
}

/* ── decode_ax25_hwaddr ──────────────────────────────────────────────── */
static int decode_ax25_hwaddr(const char *hw, char *call_out, int buflen)
{
    unsigned int b[7] = {0};
    if (sscanf(hw, "%x:%x:%x:%x:%x:%x:%x",
               &b[0],&b[1],&b[2],&b[3],&b[4],&b[5],&b[6]) != 7)
        return -1;

    char call[8] = {0};
    int ci = 0;
    for (int i = 0; i < 6; i++) {
        char ch = (char)(b[i] >> 1);
        if (ch != ' ' && ch != '\0')
            call[ci++] = (char)toupper((unsigned char)ch);
    }
    call[ci] = '\0';
    if (ci == 0) return -1;

    int ssid = (int)((b[6] >> 1) & 0x0F);
    if (ssid > 0)
        snprintf(call_out, (size_t)buflen, "%s-%d", call, ssid);
    else
        snprintf(call_out, (size_t)buflen, "%s", call);
    return 0;
}

/* ── iface_from_callsign ─────────────────────────────────────────────── */
static int iface_from_callsign(const char *callsign,
                                char *iface_out, int iface_buflen)
{
    DIR *d = opendir("/sys/class/net");
    if (!d) {
        LOG_ERR("iface_from_callsign: opendir('/sys/class/net'): %s",
                strerror(errno));
        return -1;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char addr_path[256];
        snprintf(addr_path, sizeof(addr_path),
                 "/sys/class/net/%s/address", ent->d_name);

        FILE *af = fopen(addr_path, "r");
        if (!af) continue;

        char hw[32] = {0};
        if (!fgets(hw, sizeof(hw), af)) { fclose(af); continue; }
        fclose(af);
        hw[strcspn(hw, "\n\r")] = '\0';

        char decoded[MAX_CALLSIGN_LEN] = {0};
        if (decode_ax25_hwaddr(hw, decoded, sizeof(decoded)) < 0) {
            LOG_DBG("iface_from_callsign: %s (%s) — not AX.25",
                    ent->d_name, hw);
            continue;
        }

        LOG_DBG("iface_from_callsign: %s -> '%s'", ent->d_name, decoded);

        if (strcasecmp(decoded, callsign) == 0) {
            closedir(d);
            snprintf(iface_out, (size_t)iface_buflen, "%s", ent->d_name);
            LOG_INF("iface_from_callsign: '%s' -> interface '%s'",
                    callsign, iface_out);
            return 0;
        }
    }

    closedir(d);
    LOG_ERR("iface_from_callsign: '%s' not found "
            "(are kissattach/ax25 interfaces up?)", callsign);
    return -1;
}

/* ── ax25_tune_interface ─────────────────────────────────────────────── */
/*
 * Write an integer value to a /proc/sys/net/ax25/<iface>/<param> file.
 * Returns 0 on success, -1 on error.
 */
static int write_proc_param(const char *iface, const char *param, int value)
{
    char path[256];
    snprintf(path, sizeof(path), "/proc/sys/net/ax25/%s/%s", iface, param);

    FILE *f = fopen(path, "w");
    if (!f) {
        LOG_WRN("write_proc_param: cannot write '%s': %s",
                path, strerror(errno));
        return -1;
    }
    fprintf(f, "%d\n", value);
    fclose(f);
    LOG_INF("write_proc_param: %s = %d", path, value);
    return 0;
}

/*
 * ax25_tune_interface — apply kernel AX.25 timing parameters for an AXUDP link.
 *
 * Resolves port_name → callsign → kernel interface, then sets:
 *   t2_timeout = 50ms  (ack delay — default 3000ms is far too slow)
 *   standard_window_size = 7  (match axports config)
 *
 * This eliminates REJ frames caused by xnet retransmitting at ~70ms
 * before the kernel sends RR with the default 3-second T2 delay.
 */
int ax25_tune_interface(const char *port_name)
{
    char port_call[MAX_CALLSIGN_LEN] = {0};
    const char *axports_paths[] = {
        "/usr/local/etc/ax25/axports",
        "/etc/ax25/axports",
        NULL
    };

    int found = 0;
    for (int i = 0; axports_paths[i]; i++) {
        if (axports_get_callsign(axports_paths[i], port_name,
                                 port_call, sizeof(port_call)) == 0) {
            found = 1; break;
        }
    }
    if (!found) {
        LOG_WRN("ax25_tune_interface: cannot resolve port '%s'", port_name);
        return -1;
    }

    char iface[IF_NAMESIZE] = {0};
    if (iface_from_callsign(port_call, iface, sizeof(iface)) < 0) {
        LOG_WRN("ax25_tune_interface: cannot find interface for '%s'",
                port_call);
        return -1;
    }

    LOG_INF("ax25_tune_interface: tuning %s (%s / %s)",
            iface, port_name, port_call);

    int ok = 0;
    ok += write_proc_param(iface, "t2_timeout", 1);   /* immediate ack */
    ok += write_proc_param(iface, "standard_window_size", 7);

    return ok;
}

/* ── ax25_get_ifname ─────────────────────────────────────────────────── */
/*
 * Resolve an axports port name to the kernel interface name.
 *   "xnet" → "IW2OHX-3" (via axports) → "ax1" (via /sys/class/net)
 * Returns 0 on success, -1 on failure.
 */
int ax25_get_ifname(const char *port_name, char *iface_out, int buflen)
{
    char port_call[MAX_CALLSIGN_LEN] = {0};
    const char *axports_paths[] = {
        "/usr/local/etc/ax25/axports",
        "/etc/ax25/axports",
        NULL
    };

    int found = 0;
    for (int i = 0; axports_paths[i]; i++) {
        if (axports_get_callsign(axports_paths[i], port_name,
                                 port_call, sizeof(port_call)) == 0) {
            found = 1; break;
        }
    }
    if (!found) {
        LOG_WRN("ax25_get_ifname: cannot resolve port '%s'", port_name);
        return -1;
    }

    return iface_from_callsign(port_call, iface_out, buflen);
}

/* ── encode_call ─────────────────────────────────────────────────────── */
static int encode_call(const char *callstr, ax25_address *addr)
{
    if (ax25_aton_entry(callstr, (char *)addr) < 0) {
        LOG_ERR("encode_call: ax25_aton_entry failed for '%s'", callstr);
        return -1;
    }
    LOG_DBG("encode_call: '%s' -> %02X %02X %02X %02X %02X %02X %02X",
            callstr,
            ((uint8_t *)addr)[0], ((uint8_t *)addr)[1],
            ((uint8_t *)addr)[2], ((uint8_t *)addr)[3],
            ((uint8_t *)addr)[4], ((uint8_t *)addr)[5],
            ((uint8_t *)addr)[6]);
    return 0;
}

/* ── ax25_add_route ──────────────────────────────────────────────────── */
static int ax25_add_route(const char *port_callsign, const char *dest)
{
    LOG_INF("ax25_add_route: %s via %s", dest, port_callsign);

    int fd = socket(AF_AX25, SOCK_DGRAM, 0);
    if (fd < 0) {
        LOG_ERR("ax25_add_route: socket(): %s", strerror(errno));
        return -1;
    }

    flexnetd_ax25_route_t route;
    memset(&route, 0, sizeof(route));
    LOG_DBG("ax25_add_route: struct size=%zu bytes", sizeof(route));

    if (encode_call(port_callsign, &route.port_addr) < 0 ||
        encode_call(dest,          &route.dest_addr) < 0) {
        close(fd); return -1;
    }
    route.digi_count = 0;

    if (ioctl(fd, SIOCADDRT, &route) < 0) {
        if (errno == EEXIST) {
            LOG_INF("ax25_add_route: route already exists (OK)");
            close(fd); return 0;
        }
        LOG_ERR("ax25_add_route: ioctl(SIOCADDRT): %s (errno=%d)",
                strerror(errno), errno);
        close(fd); return -1;
    }

    close(fd);
    LOG_INF("ax25_add_route: route added: %s -> %s", dest, port_callsign);
    return 0;
}

/* ── ax25_connect ────────────────────────────────────────────────────── */
int ax25_connect(const char *mycall, const char *neighbor,
                 const char *port_name)
{
    LOG_INF("ax25_connect: neighbor=%s  port=%s", neighbor, port_name);
    LOG_INF("ax25_connect: mycall=%s (L3 payload only)", mycall);

    char port_call[MAX_CALLSIGN_LEN] = {0};
    const char *axports_paths[] = {
        "/usr/local/etc/ax25/axports",
        "/etc/ax25/axports",
        NULL
    };
    int found = 0;
    for (int i = 0; axports_paths[i]; i++) {
        if (axports_get_callsign(axports_paths[i], port_name,
                                 port_call, sizeof(port_call)) == 0) {
            LOG_INF("ax25_connect: port '%s' -> callsign '%s'",
                    port_name, port_call);
            found = 1; break;
        }
    }
    if (!found) {
        LOG_ERR("ax25_connect: cannot resolve port '%s'", port_name);
        return -1;
    }

    if (ax25_add_route(port_call, neighbor) < 0)
        LOG_WRN("ax25_connect: ax25_add_route failed — trying anyway");

    char iface[IF_NAMESIZE] = {0};
    int have_iface = (iface_from_callsign(port_call, iface, sizeof(iface)) == 0);

    int fd = socket(AF_AX25, SOCK_SEQPACKET, 0);
    if (fd < 0) {
        LOG_ERR("ax25_connect: socket(): %s", strerror(errno));
        return -1;
    }

    if (have_iface) {
        if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE,
                       iface, (socklen_t)(strlen(iface) + 1)) < 0)
            LOG_WRN("ax25_connect: SO_BINDTODEVICE('%s'): %s", iface, strerror(errno));
        else
            LOG_INF("ax25_connect: SO_BINDTODEVICE='%s' OK", iface);
    }

    struct sockaddr_ax25 dst;
    memset(&dst, 0, sizeof(dst));
    dst.sax25_family = AF_AX25;
    dst.sax25_ndigis = 0;
    if (encode_call(neighbor, &dst.sax25_call) < 0) { close(fd); return -1; }

    LOG_INF("ax25_connect: connect() to %s ...", neighbor);
    if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        LOG_ERR("ax25_connect: connect('%s'): %s", neighbor, strerror(errno));
        close(fd); return -1;
    }

    ax25_enable_pidincl(fd);
    LOG_INF("ax25_connect: L2 session established fd=%d", fd);
    return fd;
}

/* ── ax25_listen ─────────────────────────────────────────────────────── */
/*
 * Bind listen_call on the given axport and start listening.
 *
 * listen_call should be the port's own callsign from axports
 * (e.g. IW2OHX-3 for xnet). This matches the interface address,
 * so bind() works natively with SO_BINDTODEVICE.
 *
 * listen_call MUST NOT be in ax25d.conf — flexnetd owns it.
 * Configure Xnet: ro flexnet add xnet <listen_call>
 *
 * Returns listen_fd >= 0 on success, -1 on error.
 */
int ax25_listen(const char *listen_call, const char *port_name)
{
    LOG_INF("ax25_listen: binding %s on port %s", listen_call, port_name);

    char port_call[MAX_CALLSIGN_LEN] = {0};
    const char *axports_paths[] = {
        "/usr/local/etc/ax25/axports",
        "/etc/ax25/axports",
        NULL
    };
    int found = 0;
    for (int i = 0; axports_paths[i]; i++) {
        if (axports_get_callsign(axports_paths[i], port_name,
                                 port_call, sizeof(port_call)) == 0) {
            LOG_INF("ax25_listen: port '%s' -> callsign '%s'",
                    port_name, port_call);
            found = 1; break;
        }
    }
    if (!found) {
        LOG_ERR("ax25_listen: cannot resolve port '%s'", port_name);
        return -1;
    }

    char iface[IF_NAMESIZE] = {0};
    int have_iface = (iface_from_callsign(port_call, iface, sizeof(iface)) == 0);

    int fd = socket(AF_AX25, SOCK_SEQPACKET, 0);
    if (fd < 0) {
        LOG_ERR("ax25_listen: socket(): %s", strerror(errno));
        return -1;
    }

    if (have_iface) {
        if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE,
                       iface, (socklen_t)(strlen(iface) + 1)) < 0)
            LOG_WRN("ax25_listen: SO_BINDTODEVICE('%s'): %s",
                    iface, strerror(errno));
        else
            LOG_INF("ax25_listen: SO_BINDTODEVICE='%s' OK", iface);
    }

    struct sockaddr_ax25 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sax25_family = AF_AX25;
    addr.sax25_ndigis = 0;
    if (encode_call(listen_call, &addr.sax25_call) < 0) { close(fd); return -1; }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERR("ax25_listen: bind('%s'): %s", listen_call, strerror(errno));
        if (errno == EADDRINUSE)
            LOG_ERR("ax25_listen: '%s' already bound — check ax25d.conf",
                    listen_call);
        close(fd); return -1;
    }
    LOG_INF("ax25_listen: bound '%s'", listen_call);

    if (listen(fd, 5) < 0) {
        LOG_ERR("ax25_listen: listen(): %s", strerror(errno));
        close(fd); return -1;
    }

    LOG_INF("ax25_listen: ready (fd=%d)", fd);
    return fd;
}

/* ── ax25_accept ─────────────────────────────────────────────────────── */
int ax25_accept(int listen_fd, char *peer_call, int peer_buflen)
{
    struct sockaddr_ax25 peer;
    socklen_t peer_len = sizeof(peer);

    int fd = (int)accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
    if (fd < 0) {
        LOG_ERR("ax25_accept: accept(): %s", strerror(errno));
        return -1;
    }

    if (peer_call && peer_buflen > 0) {
        const char *ntoa = ax25_ntoa(&peer.sax25_call);
        snprintf(peer_call, (size_t)peer_buflen, "%s",
                 ntoa ? ntoa : "(unknown)");
    }

    /* NOTE: do NOT enable PIDINCL here — it must only be set for
     * FlexNet neighbor sessions (PID=CE/CF). User sessions need
     * normal socket I/O for uronode exec. The caller enables
     * PIDINCL selectively after identifying the peer. */
    LOG_INF("ax25_accept: connection from %s (fd=%d)",
            peer_call ? peer_call : "?", fd);
    return fd;
}

/* ── ax25_disconnect ─────────────────────────────────────────────────── */
void ax25_disconnect(int fd)
{
    if (fd < 0) return;
    LOG_INF("ax25_disconnect: closing fd=%d", fd);
    shutdown(fd, SHUT_RDWR);
    close(fd);
}

/* ── fd_is_socket ────────────────────────────────────────────────────── */
static int fd_is_socket(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0) return 0;
    return S_ISSOCK(st.st_mode);
}

/* ── ax25_send ───────────────────────────────────────────────────────── */
int ax25_send(int fd, uint8_t pid, const uint8_t *data, int len)
{
    LOG_DBG("ax25_send: fd=%d pid=0x%02X len=%d", fd, pid, len);

    if (g_log_level >= LOG_LEVEL_DEBUG)
        hex_dump("ax25_send payload", data, len);

    if (!fd_is_socket(fd)) {
        int sent = (int)write(fd, data, (size_t)len);
        if (sent < 0) LOG_ERR("ax25_send: write(pipe): %s", strerror(errno));
        else          LOG_DBG("ax25_send: sent %d bytes (pipe)", sent);
        return sent;
    }

    /*
     * With AX25_PIDINCL enabled (set in ax25_accept/ax25_connect),
     * the first byte of the send buffer is the PID for the I-frame
     * L2 header. The kernel extracts it and puts it in the header,
     * sending the rest as the info field.
     *
     * This is the only reliable way to send non-F0 PIDs on Linux
     * AX.25 SEQPACKET sockets — setsockopt(AX25_PID) is broken
     * on many kernels.
     */
    uint8_t frame[2048];
    if (len + 1 > (int)sizeof(frame)) {
        LOG_ERR("ax25_send: frame too large (%d)", len + 1);
        return -1;
    }
    frame[0] = pid;
    memcpy(frame + 1, data, (size_t)len);

    int sent = (int)send(fd, frame, (size_t)(len + 1), 0);
    if (sent < 0) LOG_ERR("ax25_send: send(): %s", strerror(errno));
    else          LOG_DBG("ax25_send: sent %d bytes pid=0x%02X", sent, pid);
    return (sent > 0) ? sent - 1 : sent;
}

/* ── ax25_recv ───────────────────────────────────────────────────────── */
int ax25_recv(int fd, uint8_t *pid_out, uint8_t *buf, int buflen,
              int timeout_ms)
{
    fd_set rfds;
    struct timeval tv;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (ret < 0) {
        if (errno == EINTR) return 0;
        LOG_ERR("ax25_recv: select(): %s", strerror(errno));
        return -1;
    }
    if (ret == 0) {
        LOG_DBG("ax25_recv: timeout (%dms)", timeout_ms);
        return 0;
    }

    uint8_t raw[2048];
    int rlen;

    if (fd_is_socket(fd))
        rlen = (int)recv(fd, raw, sizeof(raw), 0);
    else
        rlen = (int)read(fd, raw, sizeof(raw));

    if (rlen <= 0) {
        if (rlen == 0) LOG_INF("ax25_recv: connection closed");
        else           LOG_ERR("ax25_recv: read/recv: %s", strerror(errno));
        return -1;
    }

    LOG_DBG("ax25_recv: %d bytes (fd=%d %s)", rlen, fd,
            fd_is_socket(fd) ? "socket" : "pipe");
    if (g_log_level >= LOG_LEVEL_DEBUG)
        hex_dump("ax25_recv raw", raw, rlen);

    if (!fd_is_socket(fd)) {
        *pid_out = PID_CE;
        int paylen = rlen < buflen ? rlen : buflen;
        memcpy(buf, raw, (size_t)paylen);
        LOG_DBG("ax25_recv: pipe pid=CE paylen=%d first=0x%02X",
                paylen, raw[0]);
        return paylen;
    }

    uint8_t first = raw[0];
    int has_pid = (first == PID_CE || first == PID_CF || first == PID_F0 ||
                   first == 0xCC  || first == 0xC8);
    if (has_pid) {
        *pid_out = first;
        int paylen = rlen - 1;
        if (paylen > buflen) paylen = buflen;
        memcpy(buf, raw + 1, (size_t)paylen);
        LOG_DBG("ax25_recv: pid=0x%02X paylen=%d", *pid_out, paylen);
        return paylen;
    }

    *pid_out = 0x00;
    int paylen = rlen < buflen ? rlen : buflen;
    memcpy(buf, raw, (size_t)paylen);
    LOG_DBG("ax25_recv: no PID (first=0x%02X) paylen=%d", first, paylen);
    return paylen;
}
