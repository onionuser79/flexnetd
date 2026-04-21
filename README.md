# flexnetd — Native FlexNet Routing Daemon for Linux AX.25

**Version 1.0.0** — production release | Author: IW2OHX | License: GPL v3 | April 2026

[![release](https://img.shields.io/badge/release-v1.0.0-brightgreen.svg)](https://github.com/onionuser79/flexnetd/releases/tag/v1.0.0)

A native FlexNet **CE/CF** routing daemon for Linux AX.25.  Peers
directly with (X)NET and PC/Flexnet nodes over AX.25 (RF, AXIP, or
AXUDP), replacing the legacy text-polling `flexd` with a full
bidirectional protocol implementation.

```
IW2OHX-14 ((X)NET V1.39) ←── FlexNet CE/CF ──→ IW2OHX-3 (flexnetd)
                                                      │
                                                      ├── full destination table
                                                      ├── our route advertised
                                                      │     (Q=2 RTT=2 in peer L)
                                                      └── user sessions → URONode
```

---

## Table of Contents

1. [Peer Compatibility](#1-peer-compatibility)
2. [Features](#2-features)
3. [Quick Start](#3-quick-start)
4. [Installation](#4-installation)
5. [Configuration](#5-configuration)
6. [Operation](#6-operation)
7. [PC/Flexnet Fine-Tuning Status](#7-pcflexnet-fine-tuning-status)
8. [Architecture](#8-architecture)
9. [Protocol Reference](#9-protocol-reference)
10. [Linux AX.25 Kernel Tuning](#10-linux-ax25-kernel-tuning)
11. [Output Files](#11-output-files)
12. [Troubleshooting](#12-troubleshooting)
13. [Source Files](#13-source-files)
14. [Changelog](#14-changelog)

---

## 1. Peer Compatibility

v1.0 has been bench-validated against two peer families:

| Peer                   | Status                      | Link metrics observed                           |
|------------------------|-----------------------------|-------------------------------------------------|
| **(X)NET V1.39**       | **Fully stable**            | Q=2 RTT=2/2 in peer's L-table (matches the      |
|                        |                             | linbpq-flexnet reference running on IW2OHX-13). |
|                        |                             | Routes persist; sync cycle completes cleanly.   |
| **PC/Flexnet 3.3g**    | Operational — fine-tuning pending | Link stays up; destination table propagates.    |
|                        |                             | pcf's displayed RTT does not yet converge to    |
|                        |                             | the ideal 1 tick.  See §7.                      |

Other (X)NET releases (1.38, 2.x) and earlier PC/Flexnet versions
are expected to work on the documented protocol subset but have
not been bench-tested.

---

## 2. Features

### Protocol

- Native FlexNet CE protocol: init handshake, keepalive, link-time
  probe/reply, compact routing records, `'3+'` / `'3-'` token flow
  control, CE type-6/7 path query
- Native FlexNet CF protocol: L3RTT probe/reply for round-trip measurement
- Bidirectional route exchange with token-based flow control
- Proactive route advertisement at session start
- Link-time measurement response for peer smoothed-RTT convergence

### Multi-port

- Up to 4 concurrent CE/CF sessions, each on its own AX.25 port
- All sessions share one listen callsign via `SO_BINDTODEVICE`
- Per-port overrides for the three peer-specific knobs
  (`route_advert`, `lt_reply`, `advert_mode`)
- Per-port destinations and linkstats files merged by a
  `flock`-serialised writer that picks the lowest RTT across peers

### Server mode

- Binds directly to AX.25 port callsigns (no `ax25d` dependency for
  the FlexNet link itself)
- Dispatches by peer identity:
  - Configured neighbor → CE/CF session handler (`poll_cycle.c`)
  - Anyone else → `fork + exec uronode` (immediate, no delay)
- CE sessions run in forked child processes so the accept loop stays
  free for incoming user connections
- `AX25_PIDINCL` enabled on neighbor sessions only (keeps URONode
  text I/O intact)

### Client mode

- Legacy `D`-command polling fallback (no peering; compatible with
  any FlexNet node that exposes the text `D` command)

### Output

- URONode-compatible `gateways` and `destinations` files
- Atomic writes (temp + rename) to prevent partial reads
- Path cache populated from CE type-7 replies, queryable via
  `flexdest -r <call>`

### Kernel tuning

- Automatically sets AX.25 `t2_timeout=1 ms` on the configured port
  at startup (default 3000 ms causes L2 REJ frames on AXUDP links)
- Adjusts `standard_window_size` to match axports

---

## 3. Quick Start

On a Linux host with AX.25 kernel support, `libax25-dev`, and axports
configured:

```bash
git clone https://github.com/onionuser79/flexnetd
cd flexnetd
make
sudo make install
sudo vi /usr/local/etc/ax25/flexnetd.conf       # set MyCall and Port
sudo systemctl enable flexnetd
sudo systemctl start flexnetd
```

Then verify on the peer's L-table that flexnetd appears with a
reasonable Q/RTT, and on the local machine that `fld` lists
destinations with real RTTs.

---

## 4. Installation

### 4.1 Prerequisites

- Linux with AX.25 kernel support (`modprobe ax25`)
- `libax25-dev` development headers
  - Debian/Ubuntu: `sudo apt install libax25-dev`
- An AX.25 port configured in `/etc/ax25/axports` (RF, AXIP, or AXUDP)
- URONode installed at `/usr/local/sbin/uronode` (only needed for
  user-session routing)

### 4.2 Build

```bash
cd flexnetd/
make clean && make
```

For debug builds with unstripped symbols and the DEBUG log level
permitted:

```bash
make debug        # produces flexnetd_debug alongside flexnetd
```

### 4.3 Install

```bash
sudo make install
```

This installs:

| Path | Purpose |
|------|---------|
| `/usr/local/sbin/flexnetd`             | main daemon |
| `/usr/local/sbin/flexnetd_debug`       | debug build (if built) |
| `/usr/local/sbin/flexdest`             | destination-query tool |
| `/usr/local/etc/ax25/flexnetd.conf`    | config (preserved if exists) |
| `/usr/local/etc/ax25/flexnetd.conf.debug` | DEBUG config template |
| `/etc/systemd/system/flexnetd.service` | systemd unit |

### 4.4 URONode patch (required for outbound transit)

flexnetd writes a `gateways` file that URONode reads for FlexNet
outbound routing.  For outbound connections to carry our node in
the digipeater via-list with the correct `H` bit, URONode needs a
small patch to `gateway.c` that sets `AX25_IAMDIGI` before `connect()`.

Without this patch, outbound FlexNet connects from URONode will be
dropped by the neighbor (SABM goes out without the `H` bit set on
our callsign, so the neighbor doesn't recognise itself as an
intermediate hop).

```bash
cd /path/to/uronode-source/
git apply /path/to/flexnetd/patches/uronode-m2-digipeater-path.patch
make clean && make
sudo make install
```

Details: [`patches/README.md`](patches/README.md).

### 4.5 ax25d.conf — IMPORTANT

The FlexNet listen callsign (e.g. `IW2OHX-3`) **must NOT** appear in
`/etc/ax25/ax25d.conf`.  flexnetd binds the listen callsign directly
via `ax25_listen()` on each port socket; if ax25d also binds it
there is a socket conflict and one of the two silently loses.

If ax25d is currently owning the callsign, remove or comment its
`[IW2OHX-3 VIA xnet]` block and reload ax25d.

### 4.6 Disabling legacy flexd

If the legacy `flexd` daemon is running, stop and disable it:

```bash
sudo systemctl stop flexd
sudo systemctl disable flexd
```

flexnetd replaces flexd entirely.

---

## 5. Configuration

All settings live in `/usr/local/etc/ax25/flexnetd.conf`.  The
shipped file is copied verbatim below with the recommended v1.0
defaults.

### 5.1 Production flexnetd.conf (annotated)

```conf
# ── Identity ─────────────────────────────────────────────────────────
MyCall          IW2OHX-3
Alias           OHXGW
MinSSID         3
MaxSSID         3

Role            server

# ── FlexNet ports ────────────────────────────────────────────────────
#
# Syntax:
#   Port <name> <neighbor_call> <listen_call>  [options]
#
# Per-port options (space-separated, any order):
#
#   route_advert=<sec>   periodic route re-advertisement interval
#                        0  — disabled (recommended)
#                        N  — every N seconds
#
#   lt_reply=<sec>       CE type-1 link-time emission interval
#                        0  — reply on every peer event   (use on (X)NET)
#                        320 — match PC/Flexnet ts_ahead (use on PC/Flexnet)
#
#   advert_mode=<full|record>
#                        full   — '3+' + record + '3-'   (use on (X)NET)
#                        record — compact record only     (use on PC/Flexnet)
#
# Recommended v1.0 settings:

Port xnet  IW2OHX-14  IW2OHX-3  route_advert=0  lt_reply=0    advert_mode=full

# Uncomment for PC/Flexnet peering:
# Port pcf   IW2OHX-12  IW2OHX-3  route_advert=0  lt_reply=320  advert_mode=record

# ── Timers ───────────────────────────────────────────────────────────
PollInterval       240       # client-mode D-command poll fallback
KeepaliveInterval   90
BeaconInterval     120

PathProbeInterval    0       # CE type-6 probes (0 = disabled; (X)NET V1.39
                             #                   doesn't reply, so probing
                             #                   only adds airtime)

RouteAdvertInterval    0     # global default; per-port overrides above
LinkTimeReplyInterval 320    # global default; per-port overrides above

# ── Output files ─────────────────────────────────────────────────────
GatewaysFile    /usr/local/var/lib/ax25/flex/gateways
DestFile        /usr/local/var/lib/ax25/flex/destinations
LinkStatsFile   /usr/local/var/lib/ax25/flex/linkstats
PathsFile       /usr/local/var/lib/ax25/flex/paths

# ── Protocol constants ───────────────────────────────────────────────
Infinity          60000
TriggerThreshold     50

# ── Logging ──────────────────────────────────────────────────────────
LogLevel          3       # 1=error 2=warn 3=info 4=debug
Syslog            yes
```

### 5.2 Command-line options

```
flexnetd [-c config] [-d] [-f] [-v[vv]] [-l logfile] [-V]

  -c file   Config file (default: /usr/local/etc/ax25/flexnetd.conf)
  -d        Daemon mode (fork, log to syslog)
  -f        Foreground mode (log to stderr)
  -v        Increase verbosity (repeat for DEBUG: -vvv)
  -l file   Also write log to file (dual console + file)
  -V        Print version and exit
```

### 5.3 Debug config and fast path probing

For protocol-level debugging there is a `flexnetd_debug` binary and a
`flexnetd.conf.debug` template that enables `PathProbeInterval=10` for
fast CE type-6/7 testing:

```bash
make debug
sudo /usr/local/sbin/flexnetd_debug -f -vvv \
    -l /tmp/flexnetd_debug.log \
    -c /usr/local/etc/ax25/flexnetd.conf.debug

# Watch the live log:
tail -F /tmp/flexnetd_debug.log

# Watch the path cache fill:
watch -n1 'cat /usr/local/var/lib/ax25/flex/paths'

# Query a specific destination:
flexdest -r ir5s
```

---

## 6. Operation

### 6.1 systemd

v1.0 ships with `flexnetd.service`:

```bash
sudo systemctl daemon-reload
sudo systemctl enable  flexnetd
sudo systemctl start   flexnetd
sudo systemctl status  flexnetd
```

Service dependencies ensure flexnetd starts **after** the AX.25 stack
(ax25ipd, ax25d) is up.

### 6.2 Logs

- systemd journal: `sudo journalctl -u flexnetd -f`
- Syslog when `Syslog yes`: typically `/var/log/syslog` or
  `/var/log/messages`, tagged `flexnetd`
- File log when `-l` given: whatever path supplied

### 6.3 Operational commands

```bash
flexdest                 # list all known destinations
flexdest IR5S            # query a specific destination
flexdest IW*             # prefix wildcard
flexdest -r IR5S         # include the recorded hop chain

cat /usr/local/var/lib/ax25/flex/linkstats    # live peer L-table
cat /usr/local/var/lib/ax25/flex/gateways     # URONode view
cat /usr/local/var/lib/ax25/flex/destinations # unified view
```

### 6.4 Verifying health

Sign of a happy link against (X)NET V1.39 — on the peer node, query
our callsign in its L-table:

```
=>l IW2OHX-3
IW2OHX-3    2 F 2 2/2 ...
                  ^^^ Q=2, RTT=2/2 — perfect convergence
```

If the link stays at Q=2 RTT=2 for several minutes you are in the
same state as the linbpq-flexnet reference implementation.

---

## 7. PC/Flexnet Fine-Tuning Status

v1.0 is **operational** on PC/Flexnet 3.3g but **not fully tuned**.

**What works:**
- Link establishment and maintenance
- Bidirectional destination-table exchange
- User-session routing through URONode
- No session resets, no L2 instability

**What is not yet ideal:**
- PC/Flexnet's displayed RTT for our callsign does not converge to
  the ideal 1 tick.  It stabilises at a higher value that reflects
  the 320-second `lt_reply` cadence matching pcf's internal
  `ts_ahead` window.
- Sending link-time frames more frequently than 320 s saturates
  pcf's 12-bit RTT field at 4095 (negative-delta wrap), which is
  why `lt_reply=320` is mandatory on PC/Flexnet ports.

**What is investigated for a future release:**
- Alternative values for our hard-coded link-time claim (currently `2`)
- Interaction between the v0.7.9 300 s proactive KA and pcf's own
  keepalive model
- Whether pcf's ts_ahead can be negotiated rather than fixed at 320 s

Operators running a PC/Flexnet peer can deploy v1.0 with confidence
for connectivity; the RTT cosmetics on pcf's side will be addressed
in a later release.

---

## 8. Architecture

### 8.1 Server mode (default)

```
                       ┌───────────────────────┐
 AX.25 port (ax1)     │       flexnetd         │
 ─────────────────────►│                        │
                       │   ax25_listen()        │
                       │   ax25_accept()        │
                       │        │               │
                       │        ├─ neighbor?    │
                       │        │  yes → fork   │──► CE/CF session
                       │        │               │    (poll_cycle.c)
                       │        │  no → fork    │──► exec uronode
                       │        │               │
                       └───────────────────────┘
```

Neighbor detection: the accepted peer callsign is compared against
each `Port` line's `neighbor` field.  On match, `AX25_PIDINCL` is
enabled and the CE/CF session handler runs.  Otherwise the
connection is immediately dispatched to URONode with no delay.

### 8.2 CE/CF session lifecycle

```
1. SABM/UA            (kernel-level L2 connection established)
2. Init exchange      (CE type '0': SSID range announcement)
3. Keepalive cycle    (CE type '2': 241-byte poll)
4. Link-time reply    (CE type '1': delay in 100-ms ticks)
5. Route exchange     (CE type '3': '3+' + records + '3-')
6. L3RTT response     (CF: echo probe with timing counters)
7. Repeat 3-6         (while the link is alive)
```

### 8.3 Session fork model

CE sessions can run for days, so they are forked into child
processes; the parent keeps `accept()`ing new connections.  User
sessions are also forked, so a slow user cannot block new peers.

```
Parent:   listen → accept → classify → fork → close(conn_fd) → accept …
Child CE: poll_cycle_run_mode(fd, native) → write destinations → exit
Child UX: dup2(fd, 0/1/2) → exec uronode
```

---

## 9. Protocol Reference

Full on-wire specification is in [`PROTOCOL_SPEC.md`](PROTOCOL_SPEC.md).
Highlights below; see the spec for exact frame layouts.

### 9.1 PID values

| PID  | Purpose                                          |
|------|--------------------------------------------------|
| 0xCE | FlexNet native — link management, keepalive, routing, path query |
| 0xCF | NET/ROM-compatible — L3RTT probe/reply, D-table  |
| 0xF0 | Plain AX.25 — user data, beacons                 |

### 9.2 CE frame dispatch

First byte of the CE payload classifies the frame:

| Byte   | Type | Purpose                                       |
|--------|------|-----------------------------------------------|
| `'0'`  | 0    | Link setup / init handshake                    |
| `'1'`  | 1    | Link-time (delay measurement, 100-ms ticks)   |
| `'2'`  | 2    | Keepalive (241-byte poll)                     |
| `'3'`  | 3    | Routing tokens (`3+`, `3-`) and compact records |
| `'4'`  | 4    | Routing-table sequence number (RX-only in v1.0) |
| `'6'`  | 6    | Path / Traceroute request                     |
| `'7'`  | 7    | Path / Traceroute reply                       |

### 9.3 FlexNet L3 connections = AX.25 digipeater chains

Key property confirmed by live captures: FlexNet L3 user connections
do NOT use CREQ/CACK session framing.  They are ordinary AX.25
connections with a digipeater via-list:

```
→ fm IW7BIA to IR5S via IW2OHX-12* IW2OHX-14* IR3UHU-2 ctl SABM+
← fm IR5S to IW7BIA via IR3UHU-2* IW2OHX-14 IW2OHX-12 ctl UA-
```

User identity (IW7BIA) is preserved end-to-end as the L2 source.
Intermediate nodes appear in the via-list with `*` marking the `H`
(has-been-repeated) bit.  This is the basis for the M2 milestone —
our node simply needs to be marked digipeater-enabled in the kernel
when the via-list includes it.

---

## 10. Linux AX.25 Kernel Tuning

### 10.1 The T2 ack-delay problem

The Linux AX.25 stack defaults `t2_timeout` to 3000 ms — it waits up
to 3 s before sending an RR for received I-frames.  (X)NET and
PC/Flexnet use aggressive retransmit timers (~70 ms on AXUDP), so
they retransmit before the RR arrives, causing the kernel to emit
REJ frames for duplicate sequence numbers.

### 10.2 flexnetd's fix

On startup, `ax25_tune_interface()` resolves the axports port name
to a kernel interface name and writes:

| Parameter                | Default | flexnetd sets | Why |
|--------------------------|---------|---------------|-----|
| `t2_timeout`             | 3000 ms | 1 ms          | Immediate ack, no piggyback delay |
| `standard_window_size`   | 2       | 7             | Match axports config |

### 10.3 AX25_PIDINCL

Linux AX.25 SEQPACKET sockets default to PID=F0 for all I-frames.
`setsockopt(SOL_AX25, AX25_PID, …)` silently fails on many kernels.

flexnetd uses `setsockopt(SOL_AX25, AX25_PIDINCL, 1)` which reliably
enables per-frame PID control — the first byte of every send buffer
is interpreted as the PID.  PIDINCL is enabled **only** on FlexNet
neighbor sessions.  User sessions that exec URONode must NOT have it
enabled (it corrupts text I/O).

---

## 11. Output Files

### 11.1 Gateways — `/usr/local/var/lib/ax25/flex/gateways`

```
addr  callsign  dev  digipeaters
00000 IW2OHX-14 ax1  IW2OHX-3
```

`dev` is the **kernel** interface name (`ax1`, not the axports port
name).  The `digipeaters` field carries our callsign so URONode
outbound connects include it in the AX.25 via-list.

### 11.2 Destinations — `/usr/local/var/lib/ax25/flex/destinations`

```
Dest     SSID    RTT Via
DB0AAT    0-9        7 IW2OHX-12
DK0WUE    0-13       2 IW2OHX-12
IR3UHU    1-1       88 IW2OHX-14
IW2OHX    3-3        1 IW2OHX-14
```

Written atomically on every incoming compact-routing batch.  Entries
with `RTT >= Infinity` (60000) are filtered.  On multi-port
deployments each CE child writes `destinations.<port>` and a
`flock`-serialised merge picks the lowest-RTT entry per
`(callsign, ssid_range)` pair across peers.

### 11.3 Linkstats — `/usr/local/var/lib/ax25/flex/linkstats`

One row per active CE peering, (X)NET `L`-table format, refreshed
every 30 s.

### 11.4 Paths — `/usr/local/var/lib/ax25/flex/paths`

In-memory cache of CE type-7 replies, flushed to disk on each new
reply.  Read by `flexdest -r <call>` to display the recorded hop
chain to a destination.

---

## 12. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Peer never shows our callsign | `[listen_call VIA port]` also in ax25d.conf | Remove/comment the duplicate ax25d entry |
| Destinations file stuck at ~20 entries | Pre-v0.7.4 rate limit | Upgrade to v1.0 |
| Peer's L-table shows Q climbing and RTT=200+ ticks | Pre-v0.7.9 proactive KA cadence | Upgrade to v1.0 |
| PC/Flexnet DMs the link after first batch | `advert_mode=full` on pcf port | Set `advert_mode=record` on pcf Port line |
| PC/Flexnet displays RTT=4095 | `lt_reply` < 320 s on pcf | Set `lt_reply=320` on pcf Port line |
| Kernel AX.25 REJ storms | T2 tuning didn't take | Check `/proc/sys/net/ax25/ax1/t2_timeout` |
| `U` output on destination doesn't show our call | URONode not patched | Apply `patches/uronode-m2-digipeater-path.patch` |

### Capture a session for debugging

```bash
# Terminal 1 — flexnetd in foreground with DEBUG log
sudo /usr/local/sbin/flexnetd_debug -f -vvv \
    -l /tmp/flex.log \
    -c /usr/local/etc/ax25/flexnetd.conf.debug

# Terminal 2 — on-wire monitor
sudo listen -a -p xnet > /tmp/xnet.txt

# Terminal 3 — state snapshots
watch -n 5 'fl; echo; fld | head -20'
```

---

## 13. Source Files

| File | Purpose |
|------|---------|
| `flexnetd.c`     | daemon main, signal handling, config loader, fork model |
| `poll_cycle.c`   | CE/CF session state machine |
| `ce_proto.c`     | CE frame builders / parsers |
| `cf_proto.c`     | CF frame builders / parsers (L3RTT, D-table) |
| `ax25sock.c`     | AX.25 socket helpers (listen, accept, connect, tuning) |
| `dtable.c`       | destination-table in-memory store and merge |
| `output.c`       | atomic file writers (gateways / destinations / linkstats / paths) |
| `config.c`       | config-file parser (flat + Port-syntax) |
| `log.c`          | syslog / stderr / file logger |
| `util.c`         | callsign parsing, misc helpers |
| `flexdest.c`     | `flexdest` standalone destination-query tool |
| `flexnetd.h`     | shared types, constants, and declarations |
| `PROTOCOL_SPEC.md` | on-wire protocol specification |
| `ROADMAP.md`     | release history |

---

## 14. Changelog

See [`ROADMAP.md`](ROADMAP.md) for the full release history.  Recent
highlights:

### v1.0.0 — Production release (2026-04-21)

- All milestones M1–M6 and M5.3 closed.
- Fully stable against (X)NET V1.39 (Q=2 RTT=2 in peer L-table).
- Operational on PC/Flexnet 3.3g with fine-tuning pending (see §7).
- Docs reorganised, compatibility matrix added.
- Recommended multi-port `flexnetd.conf` shipped as the default.

### v0.7.9 — Proactive keepalive cadence fix (2026-04-21)

Proactive KA threshold bumped 20 s → 300 s.  (X)NET's 189 s native
KAs now always preempt our proactive timer, giving an event-driven
echo pattern that matches linbpq-flexnet.  Result: (X)NET's
smoothed RTT for our callsign converges to 2 ticks (200 ms)
instead of 200 ticks (20 s).

### v0.7.8 — Per-port `advert_mode` (2026-04-21)

New per-port `advert_mode` knob (`full` or `record`).  (X)NET needs
`full` (the `'3+' + record + '3-'` token exchange closes its sync
cycle).  PC/Flexnet needs `record` (pcf DMs the L2 link on an
unsolicited `'3+'` when its token state ≠ 0).

### v0.7.7 — Disable proactive type-4 TX (2026-04-21)

(X)NET V1.39 does not handle CE type-4 "routing-seq" frames and
withdraws all routes ~20 s after seeing one.  The type-4 TX added
in v0.7.2 is now disabled; the type-4 RX parser is retained for
forward compatibility.

### v0.7.6 — Config: `lt_reply=0` on (X)NET ports (2026-04-21)

Config-only change.  `lt_reply=0` (reply on every peer event)
produces `Q=2 RTT=2` in (X)NET's L-table, matching the
linbpq-flexnet reference.

### v0.7.5 — `dtable_merge` preserves real RTTs (2026-04-21)

(X)NET advertises its full dtable in two rounds after init: first
with real RTTs, second with RTT=0 as a refresh marker.  Our merge
was overwriting real RTTs with 0.  Fix: skip merges where
`incoming->rtt == 0`.

### v0.7.4 — Destinations-file truncation fix (2026-04-21)

Latent bug since v0.3.0.  The 60-second rate limit on destination
file writes blocked all records after the first batch of ~20 entries
during (X)NET's burst-dump at session setup.  Fix: flush on every
compact-record batch.

### v0.7.3 — Per-port `lt_reply` (2026-04-21)

New per-port `lt_reply` knob letting (X)NET and PC/Flexnet ports use
different link-time cadences within one daemon instance.

### Earlier versions

See [`ROADMAP.md`](ROADMAP.md) for v0.3.0 through v0.7.2.

---

## License

GPL v3.  See [`COPYING`](COPYING) if present, or
<https://www.gnu.org/licenses/gpl-3.0.html>.

## Credits

Developed by IW2OHX with capture-driven iteration against live
(X)NET and PC/Flexnet peers.  Sync-cycle semantics and link-time
behaviour were validated against the linbpq-flexnet reference
running on IW2OHX-13.

Published at <https://github.com/onionuser79/flexnetd>.
