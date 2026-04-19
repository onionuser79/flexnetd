/*
 * test_multi_bind.c — probe whether Linux AX.25 allows binding the
 * same callsign on two different AX.25 interfaces when SO_BINDTODEVICE
 * is used.
 *
 * This settles the M6 (multi-neighbor / multi-port) architecture
 * question: if the kernel accepts both binds, we can have one
 * listen socket per port with the same listen callsign.  If it
 * rejects the second bind with EADDRINUSE, we need an alternative
 * design (SOCK_RAW or per-port distinct callsigns).
 *
 * Build:
 *     gcc -Wall -O2 -o test_multi_bind test_multi_bind.c -lax25
 *     (or via the provided Makefile: `make test_multi_bind`)
 *
 * Run (needs CAP_NET_RAW, typically sudo):
 *     sudo ./test_multi_bind
 *     sudo ./test_multi_bind IW2OHX-3 ax1 ax2
 *     sudo ./test_multi_bind <callsign> <iface1> <iface2>
 *
 * Output tells you exactly which phase failed and why.
 * Exit 0 on full success, 1 otherwise.
 *
 * Four phases are tested, and each reports PASS/FAIL with errno:
 *   [1] bind <callsign> on <iface1> WITH    SO_BINDTODEVICE
 *   [2] bind <callsign> on <iface2> WITH    SO_BINDTODEVICE (on 2nd sock)
 *   [3] listen() on both sockets
 *   [4] (control)  bind <callsign> on <iface2> WITHOUT SO_BINDTODEVICE
 *                  on a 3rd socket — expected to fail if phase 2 passed
 *                  (shows the kernel IS checking device scope).
 *
 * Interpretation matrix:
 *     phase 1 & 2 PASS  →  multi-port-same-callsign WORKS with
 *                          SO_BINDTODEVICE.  M6 architecture A.
 *     phase 1 PASS, 2 FAIL (EADDRINUSE)  →  multi-bind FORBIDDEN.
 *                                            M6 architecture B needed.
 *     phase 1 FAIL  →  environment/permissions issue, not what we're
 *                      testing.
 *
 * Author: IW2OHX, April 2026
 * License: GPL v3
 */

#include <errno.h>
#include <net/if.h>
#include <netax25/ax25.h>
#include <netax25/axlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ANSI colours for a readable report when run in a terminal */
#define ANSI_GREEN  "\x1b[32m"
#define ANSI_RED    "\x1b[31m"
#define ANSI_YELLOW "\x1b[33m"
#define ANSI_BOLD   "\x1b[1m"
#define ANSI_RESET  "\x1b[0m"

static void pass(const char *fmt, ...)
{
    va_list ap;
    fputs(ANSI_GREEN "PASS" ANSI_RESET "  ", stdout);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

static void fail(const char *fmt, ...)
{
    va_list ap;
    fputs(ANSI_RED "FAIL" ANSI_RESET "  ", stdout);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

static void note(const char *fmt, ...)
{
    va_list ap;
    fputs(ANSI_YELLOW "info" ANSI_RESET "  ", stdout);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

/*
 * try_bind — attempt to open an AX.25 SEQPACKET socket and bind
 * `call` on it.  If `ifname` is non-NULL, SO_BINDTODEVICE is applied
 * first.  Returns the fd on success, -1 on failure (with *err_out
 * set to errno).
 */
static int try_bind(const char *call, const char *ifname, int *err_out)
{
    int fd = socket(AF_AX25, SOCK_SEQPACKET, 0);
    if (fd < 0) {
        *err_out = errno;
        return -1;
    }

    if (ifname) {
        struct ifreq ifr = {0};
        snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
        if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE,
                       ifr.ifr_name, strlen(ifr.ifr_name)) < 0) {
            *err_out = errno;
            close(fd);
            return -1;
        }
    }

    struct full_sockaddr_ax25 sa;
    memset(&sa, 0, sizeof(sa));

    /* ax25_aton expects a mutable buffer */
    char callbuf[32];
    snprintf(callbuf, sizeof(callbuf), "%s", call);
    if (ax25_aton(callbuf, &sa) < 0) {
        *err_out = EINVAL;
        close(fd);
        return -1;
    }
    sa.fsa_ax25.sax25_family = AF_AX25;

    if (bind(fd, (struct sockaddr *)&sa,
             sizeof(struct full_sockaddr_ax25)) < 0) {
        *err_out = errno;
        close(fd);
        return -1;
    }
    *err_out = 0;
    return fd;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [callsign] [iface1] [iface2]\n"
        "Defaults: IW2OHX-3 ax1 ax2\n", prog);
}

int main(int argc, char *argv[])
{
    const char *call  = (argc > 1) ? argv[1] : "IW2OHX-3";
    const char *if1   = (argc > 2) ? argv[2] : "ax1";
    const char *if2   = (argc > 3) ? argv[3] : "ax2";

    if ((argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")))) {
        usage(argv[0]);
        return 0;
    }

    printf(ANSI_BOLD
           "== flexnetd multi-port bind test ==\n"
           ANSI_RESET
           "callsign : %s\n"
           "iface #1 : %s\n"
           "iface #2 : %s\n\n",
           call, if1, if2);

    int err1 = 0, err2 = 0, err3 = 0;
    int fd1 = -1, fd2 = -1, fd3 = -1;

    /* ── Phase 1: bind callsign on iface1 WITH SO_BINDTODEVICE ───── */
    printf(ANSI_BOLD "[1] " ANSI_RESET
           "bind %s on %s (SO_BINDTODEVICE=%s)\n", call, if1, if1);
    fd1 = try_bind(call, if1, &err1);
    if (fd1 >= 0) {
        pass("fd=%d", fd1);
    } else {
        fail("errno=%d (%s)", err1, strerror(err1));
        note("Phase 1 failed — not what we're testing.  Typical causes:");
        note("  - iface '%s' does not exist (check `cat /proc/net/ax25`)", if1);
        note("  - missing CAP_NET_RAW (run under sudo)");
        note("  - another process already has %s bound on %s", call, if1);
        return 1;
    }

    /* ── Phase 2: bind callsign on iface2 WITH SO_BINDTODEVICE ───── */
    putchar('\n');
    printf(ANSI_BOLD "[2] " ANSI_RESET
           "bind %s on %s (SO_BINDTODEVICE=%s, 2nd socket)\n",
           call, if2, if2);
    fd2 = try_bind(call, if2, &err2);
    if (fd2 >= 0) {
        pass("fd=%d — SAME CALLSIGN on a DIFFERENT port WORKED",
             fd2);
    } else {
        fail("errno=%d (%s)", err2, strerror(err2));
        if (err2 == EADDRINUSE) {
            note("Kernel refused the second bind despite SO_BINDTODEVICE.");
            note("This confirms architecture B is required for M6.");
        } else if (err2 == ENODEV) {
            note("iface '%s' does not exist (check `cat /proc/net/ax25`)",
                 if2);
        }
    }

    /* ── Phase 3: listen on whichever sockets survived ───────────── */
    putchar('\n');
    printf(ANSI_BOLD "[3] " ANSI_RESET "listen() on bound sockets\n");
    if (fd1 >= 0) {
        if (listen(fd1, 5) == 0)
            pass("listen(fd1=%d on %s) OK", fd1, if1);
        else
            fail("listen(fd1=%d on %s) errno=%d (%s)",
                 fd1, if1, errno, strerror(errno));
    }
    if (fd2 >= 0) {
        if (listen(fd2, 5) == 0)
            pass("listen(fd2=%d on %s) OK", fd2, if2);
        else
            fail("listen(fd2=%d on %s) errno=%d (%s)",
                 fd2, if2, errno, strerror(errno));
    }

    /* ── Phase 4 (control): bind WITHOUT SO_BINDTODEVICE ─────────── */
    putchar('\n');
    printf(ANSI_BOLD "[4] " ANSI_RESET
           "(control) bind %s with NO SO_BINDTODEVICE (3rd socket)\n",
           call);
    fd3 = try_bind(call, NULL, &err3);
    if (fd3 >= 0) {
        note("fd=%d — unbound-to-device succeeded.  "
             "Kernel lets a wildcard-dev bind coexist with pinned binds.",
             fd3);
    } else if (err3 == EADDRINUSE) {
        note("EADDRINUSE on unbound third socket — expected if "
             "phases 1 or 2 created a colliding wildcard entry.");
    } else {
        note("errno=%d (%s)", err3, strerror(err3));
    }

    /* ── Summary ─────────────────────────────────────────────────── */
    putchar('\n');
    printf(ANSI_BOLD "== VERDICT ==" ANSI_RESET "\n");

    int ret = 1;
    if (fd1 >= 0 && fd2 >= 0) {
        printf(ANSI_GREEN ANSI_BOLD
               "Multi-port same-callsign binding WORKS "
               "with SO_BINDTODEVICE.\n"
               ANSI_RESET);
        printf("Recommendation: proceed with M6 "
               "architecture A (one listen socket per port).\n");
        ret = 0;
    } else if (fd1 >= 0 && err2 == EADDRINUSE) {
        printf(ANSI_RED ANSI_BOLD
               "Multi-port same-callsign binding is FORBIDDEN "
               "by this kernel, even with SO_BINDTODEVICE.\n"
               ANSI_RESET);
        printf("Recommendation: use M6 architecture B "
               "(single socket / per-port distinct callsigns / "
               "or SOCK_RAW).\n");
    } else {
        printf(ANSI_YELLOW
               "Test inconclusive — re-run with a clean "
               "AX.25 environment.\n"
               ANSI_RESET);
    }

    if (fd1 >= 0) close(fd1);
    if (fd2 >= 0) close(fd2);
    if (fd3 >= 0) close(fd3);
    return ret;
}
