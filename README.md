# flexnetd - FlexNet Routing Daemon for Linux AX.25

**Version 0.7.1.2 — stable** | Author: IW2OHX | License: GPL v3 | April 2026

[![stable](https://img.shields.io/badge/release-v0.7.7-brightgreen.svg)](https://github.com/onionuser79/flexnetd/releases/tag/v0.7.7)

A native FlexNet CE/CF protocol daemon for Linux, enabling direct peering
with FlexNet nodes (such as xnet) over AX.25 AXUDP links. Replaces the
legacy `flexd` polling daemon with a full bidirectional protocol
implementation that supports real-time route exchange, link quality
measurement, and user session routing.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Features](#2-features)
3. [Architecture](#3-architecture)
4. [Installation](#4-installation)
5. [Configuration](#5-configuration)
6. [FlexNet Protocol Reference](#6-flexnet-protocol-reference)
7. [Linux AX.25 Kernel Tuning](#7-linux-ax25-kernel-tuning)
8. [Output Files](#8-output-files)
9. [Development Methodology](#9-development-methodology)
10. [Changelog](#10-changelog)
11. [Source Files](#11-source-files)

---

## 1. Overview

FlexNet is a routing protocol for AX.25 packet radio networks, widely used
in European ham radio infrastructure. It provides automatic destination
discovery and quality-based routing between nodes.

The traditional Linux approach uses `flexd`, a simple polling daemon that
connects to a FlexNet neighbor, sends a text `D` command, and parses the
destination table from the response. This works but is limited: it provides
no bidirectional route exchange, no link quality measurement, and no real-time
route updates.

**flexnetd** implements the native FlexNet CE/CF protocol directly, enabling:

- Real FlexNet peering with full route exchange
- Link quality measurement via L3RTT and link time
- Proactive route advertisement (the node becomes visible in the network)
- User session routing through FlexNet destinations via URONode
- Automatic kernel AX.25 parameter tuning for optimal performance

### Example: Live peering

```
IW2OHX-14 (xnet) ←── FlexNet CE/CF ──→ IW2OHX-3 (flexnetd)
                                              │
                                              ├── ~200 FlexNet destinations
                                              ├── Route: IW2OHX 3-3 RTT=1
                                              └── User sessions → URONode
```

---

## 2. Features

### Protocol

- Native FlexNet CE protocol: init handshake, keepalive, compact routing
  records, token exchange, destination broadcast
- Native FlexNet CF protocol: L3RTT probe/reply for round-trip measurement
- Bidirectional route exchange with token-based flow control
- Proactive route advertisement on every keepalive cycle
- Link time measurement response for Q/T convergence

### Server Mode

- Binds directly to an AX.25 port callsign (no ax25d dependency for the
  FlexNet link)
- Dispatches connections by peer identity:
  - Configured neighbor → FlexNet CE/CF session handler
  - All other peers → fork + exec URONode (immediate, no delay)
- CE sessions run in forked child processes so the accept loop stays free
- AX25_PIDINCL for correct CE/CF PID framing on neighbor sessions only

### Client Mode

- Periodic D-command polling as fallback (connects to neighbor, sends `d`,
  parses text response)
- Compatible with any FlexNet node that supports the `D` command

### Output

- Writes URONode-compatible flex destination and gateway files
- Atomic file writes (temp + rename) to prevent partial reads
- Periodic destination file updates every 60 seconds during active sessions
- Gateway file uses kernel interface name for URONode AX.25 socket
  compatibility

### Kernel Tuning

- Automatically sets AX.25 T2 timeout to 1ms on the configured port at
  startup
- Adjusts window size to match axports configuration
- Eliminates L2 REJ frames caused by the default 3-second ack delay

---

## 3. Architecture

### Server Mode (default)

```
                        ┌─────────────────────┐
  AX.25 port (ax1)     │     flexnetd         │
  ─────────────────────►│                      │
                        │  ax25_listen()       │
                        │  ax25_accept()       │
                        │       │              │
                        │       ├─ neighbor?   │
                        │       │  yes → fork  │──► CE/CF session
                        │       │              │    (poll_cycle.c)
                        │       │  no → fork   │──► exec uronode
                        │       │              │
                        └─────────────────────┘
```

**Neighbor detection**: The accepted peer callsign is compared against
`Neighbor` in the config. If it matches, AX25_PIDINCL is enabled and
the CE/CF session handler runs. Otherwise, the connection is immediately
dispatched to URONode with no peek delay.

### CE/CF Session Lifecycle

```
1. SABM/UA          (L2 connection established by kernel)
2. Init exchange     (CE type '0': SSID range announcement)
3. Keepalive cycle   (CE type '2': 241-byte poll, every ~21s)
4. Link time reply   (CE type '1': delay measurement, every cycle)
5. Route exchange    (CE type '3': token + compact records)
6. L3RTT response    (CF: echo probe with timing counters)
7. Repeat 3-6        (continuous while link is alive)
```

### Session Fork Model

CE sessions are long-running (hours). To prevent blocking the accept loop,
each CE session is forked into a child process. User connections (URONode)
are also forked. The parent process only handles accept() and dispatch.

```
Parent:   listen → accept → classify → fork → close(conn_fd) → accept ...
Child CE: poll_cycle_run_mode(fd, native) → write destinations → exit
Child UX: dup2(fd, 0/1/2) → exec uronode
```

---

## 4. Installation

### Prerequisites

- Linux with AX.25 kernel support
- libax25 development headers (`apt install libax25-dev`)
- AX.25 port configured in axports (e.g., `xnet` port with AXUDP)
- URONode installed at `/usr/local/sbin/uronode`

### Build

```bash
cd flexnetd/
make clean && make
```

For debug builds: `make debug` (produces `flexnetd_debug` with symbols).

### Install

```bash
sudo make install
```

This installs:
- `/usr/local/sbin/flexnetd` (binary)
- `/usr/local/etc/ax25/flexnetd.conf` (config, if not already present)

### Patch URONode (required for outbound FlexNet connects)

flexnetd writes a gateways file that URONode reads for FlexNet routing.
For outbound connections to work with digipeater path preservation, URONode
needs a small patch to `gateway.c` that sets the `AX25_IAMDIGI` socket
option and the AX.25 H bit on the digipeater via-list.

Without this patch, outbound FlexNet connects from URONode will fail
(the SABM frame goes out without the H bit, and the FlexNet neighbor
drops it).

```bash
cd /path/to/uronode-source/
git apply /path/to/flexnetd/patches/uronode-m2-digipeater-path.patch
make clean && make
sudo make install
```

See [`patches/README.md`](patches/README.md) for full details on what
the patch does and why it's needed.

### Deploy

Add to your AX.25 startup script (`/etc/init.d/ax25`), **after** the
AXUDP interface (ax25ipd) is started:

```bash
/usr/local/sbin/flexnetd -d -c /usr/local/etc/ax25/flexnetd.conf
```

### Important: ax25d.conf

The FlexNet listen callsign (e.g., `IW2OHX-3`) must **NOT** appear in
`ax25d.conf`. flexnetd binds it directly via `ax25_listen()`. If ax25d
also binds it, there will be a conflict.

### Disabling flexd

If the legacy `flexd` daemon is running, stop and disable it:

```bash
sudo systemctl stop flexd
sudo systemctl disable flexd
```

Or remove the `flexd` line from your init script. flexnetd replaces it
entirely.

---

## 5. Configuration

All settings are in `/usr/local/etc/ax25/flexnetd.conf`.

```
# Identity
MyCall          IW2OHX-3        # Our FlexNet callsign
Alias           OHXGW           # 6-char node alias
MinSSID         3               # SSID range lower bound
MaxSSID         3               # SSID range upper bound

Role            server          # 'server' (native CE/CF) or 'client' (D-cmd)

# FlexNet ports — two equivalent syntaxes:
#
# 1) Legacy single-port (v0.3.0..v0.6.0):
Neighbor        IW2OHX-14       # FlexNet peer callsign
PortName        xnet            # axports port name
FlexListenCall  IW2OHX-3        # Callsign to bind on this port
#
# 2) Multi-port (v0.7.0+, M6) — up to 4 Port entries, same listen
#    callsign shared across ports is supported (the kernel allows it
#    because each socket uses SO_BINDTODEVICE):
#
# Port xnet  IW2OHX-14  IW2OHX-3
# Port pcf   IW2OHX-12  IW2OHX-3
#
# 3) Multi-port with per-port overrides (v0.7.1+):
#    Optional 4th field overrides the global RouteAdvertInterval for
#    THIS port only.  Use "route_advert=<seconds>" or bare integer.
#    Different FlexNet implementations have different tolerances for
#    unsolicited compact records — see v0.7.1 changelog.
#
# Port xnet  IW2OHX-14  IW2OHX-3  route_advert=60
# Port pcf   IW2OHX-12  IW2OHX-3  route_advert=0

# Timers
PollInterval    240             # Client mode: seconds between D-cmd polls
KeepaliveInterval  90           # Keepalive cycle (seconds)
BeaconInterval  120             # Beacon interval (seconds)

# M6.7: seconds between periodic re-advertisements of our own routes
# to each FlexNet peer (prevents our callsign aging out of their tables).
#   0  — DISABLED (default since v0.7.1; safe for PCFlexnet, required
#        for compatibility — PCFlexnet DMs the L2 link on unsolicited
#        compact records)
#   60 — re-advertise every 60 s (works fine with xnet; PCFlexnet will
#        DM the link if this is enabled on its port)
# Prefer per-port overrides via the "route_advert" field on Port lines.
RouteAdvertInterval  0

# M6.9: seconds between link-time frames we send to each peer.
# Matches PCFlexnet's internal ts-ahead window (about 320 s).
# Sending faster produces negative deltas that saturate pcf's 12-bit
# RTT field at 4095.  Set to 0 to disable rate-limiting.
LinkTimeReplyInterval  320

# Output files (URONode reads these)
GatewaysFile    /usr/local/var/lib/ax25/flex/gateways
DestFile        /usr/local/var/lib/ax25/flex/destinations
# v0.7.1: per-port files appear alongside the unified dest_file —
#   destinations.<port>  — one per CE session
#   destinations.lock    — flock sentinel for the merge
# These are managed by flexnetd; no manual setup needed.

# Protocol
Infinity        60000           # RTT value = unreachable
TriggerThreshold  50            # Route change threshold

# Logging
LogLevel        3               # 1=error, 2=warn, 3=info, 4=debug
Syslog          yes             # Log to syslog (daemon mode)
```

### Command-line Options

```
flexnetd [-c config] [-d] [-f] [-v[vv]] [-l logfile] [-V]
  -c file   Config file (default: /usr/local/etc/ax25/flexnetd.conf)
  -d        Daemon mode (fork, log to syslog)
  -f        Foreground mode (log to stderr)
  -v        Increase verbosity (repeat for more: -vvv = DEBUG)
  -l file   Also write log to file (dual console + file output)
  -V        Print version and exit
```

### Debug Build and M5.3 Testing

For protocol-level testing of the path query protocol (M5.3) there is a
debug binary with unoptimised symbols and a dedicated config template.

```bash
make debug                      # builds flexnetd_debug (no install)

# Run in foreground with DEBUG log level, dual console + file, fast probes
sudo /usr/local/sbin/flexnetd_debug -f -vvv \
    -l /tmp/flexnetd_m53.log \
    -c /usr/local/etc/ax25/flexnetd.conf.debug
```

`flexnetd.conf.debug` sets `PathProbeInterval 10` so probes go out every
10 seconds rather than the 60-second production default.  Full table of
200 destinations scans in ~33 minutes instead of ~3 hours.

At DEBUG log level, every type-6 (outbound probe) and every type-7
(inbound reply) frame is hex-dumped to the log.  The pending-query
table is dumped every 30 seconds so you can see which probes are still
in flight.

```bash
# Watch the live log
tail -F /tmp/flexnetd_m53.log

# Watch the cache fill up
watch -n 1 'cat /usr/local/var/lib/ax25/flex/paths'

# Query a specific destination
flexdest -r ir5s
```

Typical sequence to observe:
1. `TX type-6 probe`  hex-dump + `sent type-6 probe qso=N target=...`
2. `RX type-7 reply`  hex-dump + `matches pending slot=X`
3. `output_write_paths_cache_add: target=... kind=R hops=N`
4. `flexdest -r <target>` prints the `*** route:` line

---

## 6. FlexNet Protocol Reference

The FlexNet protocol uses two AX.25 PID values over connected-mode
(SEQPACKET) I-frames. The protocol is text-based (ASCII) with
fixed-width fields.

Protocol behavior documented from live packet captures between
flexnetd and its neighbors.

### 6.1 PID Values

| PID | Name | Purpose |
|-----|------|---------|
| 0xCE | FlexNet native | Link management, keepalive, routing |
| 0xCF | NET/ROM compatible | L3RTT probes and destination tables |
| 0xF0 | No L3 | User data / text sessions |

### 6.2 CE Protocol (PID=0xCE)

Each CE frame is classified by its first payload byte:

| Byte | Type | Description |
|------|------|-------------|
| `'0'` (0x30) | Init Handshake | Link setup with SSID range |
| `'1'` (0x31) | RTT-Pong / Link Time | Link delay measurement (100ms ticks) |
| `'2'` (0x32) | RTT-Ping / Keepalive | Poll frame (241 bytes) |
| `'3'` (0x33) | Routing Data | Token signals and compact routing records |
| `'4'` (0x34) | Destination filter | Send only / Resend all with RTT threshold |
| `'6'` (0x36) | Route/Traceroute Request | Path query (see §6.2.6) |
| `'7'` (0x37) | Route/Traceroute Reply | Path response with accumulated hops (see §6.2.6) |

#### Init Handshake (type '0')

Establishes a FlexNet link between two nodes.

```
Byte 0: 0x30                   (init handshake marker, always 0x30)
Byte 1: 0x30 + max_ssid        (upper SSID bound, 0-15)
Byte 2: 0x25                   (capability flags)
Byte 3: 0x21                   (capability flags)
Byte 4: 0x0D                   (CR terminator)
```

Total: 5 bytes. Example: max SSID 14 = `30 3E 25 21 0D`.

**Important**: Byte 0 MUST always be `0x30`. Using `0x30 + min_ssid`
when min_ssid > 0 causes misclassification (e.g., `0x33` = routing data).

#### Link Time (type '1')

Reports link delay measurement in 100ms ticks.

```
'1' <decimal_integer> '\r'
```

Example: `1600\r` = delay of 600 ticks = 60,000ms.
Example: `12\r` = delay of 2 ticks = 200ms.

The neighbor uses this value in exponential smoothing to compute the
link's Q/T (quality/timer) metric. Repeated measurements are required
for convergence from the initial value of 600 (RTT_INFINITY).

#### Keepalive / Poll (type '2')

Null frame for link liveness detection.

```
'2' + 240 space characters (0x20) = 241 bytes total
```

The keepalive is exactly 241 bytes. There is no trailing `'10\r'` (that
is a separate CE status frame sometimes seen after a keepalive).

Keepalive cycles occur every ~21 seconds. Each cycle:
1. Neighbor sends keepalive
2. flexnetd echoes keepalive back
3. flexnetd sends link time measurement
4. Neighbor sends link time measurement

#### Routing Data (type '3')

##### Token Signals (3 bytes each)

| Frame | Meaning |
|-------|---------|
| `3+\r` | Request token / ready to exchange routes |
| `3-\r` | Release token / end of routing batch |

##### Compact Routing Records

Multi-entry frames carrying the routing table:

```
'3' <record1> <record2> ... <recordN> ['-'] '\r'
```

Each record: `CALLSIGN(6) SSID_LO(1) SSID_HI(1) RTT(digits) ' '`

- **CALLSIGN**: 6 characters, right-padded with spaces
- **SSID_LO/HI**: Single character = `0x30 + ssid_value`
  (`'0'`=0, `'1'`=1, ... `'9'`=9, `':'`=10, `';'`=11, `'<'`=12,
   `'='`=13, `'>'`=14, `'?'`=15)
- **RTT**: Variable-length decimal (1-5 digits), unit = 100ms ticks
- **Withdrawal**: Trailing `'-'` before `'\r'` = all entries withdrawn

Example: `3DB0GW 09607 IW2OHX331 \r`
- DB0GW SSID 0-9 RTT=607
- IW2OHX SSID 3-3 RTT=1

#### Token Exchange (type '4')

Token-based flow control for routing updates.

```
'4' <decimal_value> <flag_char> '\r'
```

#### Route / Traceroute Request and Reply (type '6' / '7')  — §6.2.6

These two frames implement the `D <callsign>` (find path to destination)
and `TRACE <callsign>` (hop-by-hop traceroute) commands across a FlexNet
network.  A requesting node emits a **type-6 request**; the target node
returns a **type-7 reply** that accumulates the hop-by-hop path as each
forwarding node appends its callsign.

Route and Traceroute share the same wire format — they are distinguished
by a single bit in the QSO field (see below).

##### Type-6 Request — wire format

```
'6'  HOP_BYTE  QSO_FIELD(5 bytes)  ORIGIN_CALL  ' '  TARGET_CALL
```

| Field | Size | Encoding |
|-------|------|----------|
| `'6'` | 1 byte | Frame type marker |
| HOP_BYTE | 1 byte | `0x20 + hop_count` — initial sender emits `0x20` (count=0); each forwarder increments before re-emitting |
| QSO_FIELD | 5 bytes | Query correlator, written as `sprintf("%5u", qso_id)` — space-padded right-aligned decimal |
| ORIGIN_CALL | variable | Originator's callsign-SSID text (ASCII) |
| `' '` | 1 byte | Separator |
| TARGET_CALL | variable | Destination callsign-SSID text (ASCII) |

**Example** (Route request, QSO=1, from IW2OHX-3 to IR5S, freshly sent):
```
"6     1IW2OHX-3 IR5S"
hex: 36 20 20 20 20 20 31 49 57 32 4F 48 58 2D 33 20 49 52 35 53
```

##### Type-7 Reply — wire format

```
'7'  HOP_BYTE  QSO_FIELD(5 bytes)  ' '  HOP_1  ' '  HOP_2  ' '  ...  HOP_N
```

Each forwarding node on the return path appends `' ' + own_callsign`
before relaying the frame, and increments HOP_BYTE.

**Example** (captured reply, 4 hops accumulated, QSO=1):
```
"7$    1IW2OHX-14 IR3UHU-2 IQ5KG-7 IR5S"
├─ '7'      = reply frame type
├─ '$'      = HopCount byte; 0x24 - 0x20 = 4 hops
├─ '    1'  = QSO number = 1 (matches the request)
└─ 4 callsigns separated by ' '
```

##### Route vs Traceroute flag

The **first byte** of the QSO_FIELD encodes the Route vs Traceroute
selector in its bit `0x40`:

| bit 0x40 | Meaning |
|----------|---------|
| 0 (clear) | **Route** — caller wants only the final path |
| 1 (set) | **Traceroute** — each hop also reports its identity to the requester |

Since `sprintf("%5u", n)` naturally produces either `' '` (0x20) or a
decimal digit (`0x30`..`0x39`) in the first byte — all of which have
bit `0x40` clear — an initial Route request carries bit `0x40 = 0`.
To signal Traceroute, the sender replaces the first byte of QSO_FIELD
with one that has bit `0x40 = 1` (for example `0x60`).

##### QSO correlation

The 5-byte QSO_FIELD is the **only correlator** between a request and
its reply.  The originator MUST pick a value unique among its in-flight
queries, e.g. a monotonically increasing counter modulo 100000.

### 6.3 CF Protocol (PID=0xCF)

#### L3RTT Probe/Reply

Round-trip time measurement between nodes.

```
L3RTT:<c1><c2><c3><c4> <alias> LEVEL3_V2.1 <version> $M<max_dest> $N\r
```

Fields:
- `c1-c4`: Four unsigned long counters, 11 chars wide each
- `alias`: 6-char node alias
- `LEVEL3_V2.1`: Fixed protocol version identifier
- `version`: Software version string
- `$M`: Max destination count
- `$N`: End marker

The probe originator fills c1 (send time). The responder fills c3
(receive time) and c4 (send time), enabling RTT calculation that
excludes processing time at the remote node.

#### D-Table Records

CF frames also carry text-format destination records (used in client
mode D-command polling):

```
CALLSIGN  RTT/SSID_MAX  PORT[TYPE] 'ALIAS'
```

### 6.4 Protocol Constants

| Constant | Value | Description |
|----------|-------|-------------|
| PID_CE | 0xCE | FlexNet native protocol |
| PID_CF | 0xCF | NET/ROM compatible / L3RTT |
| PID_F0 | 0xF0 | AX.25 no-L3 (text/user data) |
| RTT_INFINITY | 60000 | Unreachable destination marker |
| CE_KEEPALIVE_LEN | 241 | Keepalive frame size (bytes) |
| SSID_ENCODE_BASE | 0x30 | SSID character encoding offset |
| MAX_SSID | 15 | Maximum AX.25 SSID value |

### 6.5 FlexNet L3 Connections (AX.25 Digipeater Chains)

**Key discovery (2026-04-14):** FlexNet L3 connections do NOT use
CREQ/CACK session framing. Instead, they are standard AX.25 connections
with digipeater chains in the via-list. The user's callsign is preserved
end-to-end as the L2 source address.

Live capture evidence (IW7BIA connecting from IW2OHX-12 to IR5S):
```
→ fm IW7BIA to IR5S via IW2OHX-12* IW2OHX-14* IR3UHU-2 ctl SABM+
← fm IR5S to IW7BIA via IR3UHU-2* IW2OHX-14 IW2OHX-12 ctl UA-
→ data frames (normal AX.25 I-frames with PID=F0)
→ fm IW7BIA to IR5S via ... ctl DISC+
← fm IR5S to IW7BIA via ... ctl UA-
```

The `*` marks indicate the AX.25 H bit (has-been-repeated, bit 7 of
`ax25_call[6]`). Each intermediate node marks its entry and forwards
the frame to the next unrepeated digi in the list.

#### Digipeater Path Preservation from URONode (outbound)

When a URONode user connects to a FlexNet destination, the gateways
file includes our callsign as the first digipeater. URONode builds:
```
fm IW7CFD-15 to IR5S via IW2OHX-3 IW2OHX-14 ctl SABM+
```

**Critical: AX25_IAMDIGI is required.** The Linux kernel's
`ax25_connect()` only honors H bits from userspace if the socket has
`AX25_IAMDIGI` set. Without it, the kernel clears all `repeated[]`
flags regardless of what the application puts in `fsa_digipeater`:

```c
/* net/ax25/af_ax25.c — ax25_connect() */
if ((fsa->fsa_digipeater[ct].ax25_call[6] & AX25_HBIT) && ax25->iamdigi)
    digi->repeated[ct] = 1;    /* H bit honored */
else
    digi->repeated[ct] = 0;    /* H bit CLEARED */
```

The URONode patch sets both flags before `connect()`:
```c
setsockopt(fd, SOL_AX25, AX25_IAMDIGI, &1, sizeof(1));
sa.ax.fsa_digipeater[0].ax25_call[6] |= 0x80;
```

This produces the correct wire format:
```
fm IW7CFD-15 to IR5S via IW2OHX-3* IW2OHX-14 ctl SABM+
```

The destination's `U` command confirms our node in the digipeater path:
```
IR5S>IW7CFD-15 v IQ5KG-7 IW2OHX-3
```

### 6.6 Link Establishment Sequence

```
         flexnetd (IW2OHX-3)                xnet (IW2OHX-14)
                 |                                  |
                 |  <-- SABM (L2 connect request)   |
                 |  --> UA   (L2 accept)            |
                 |                                  |
  [PIDINCL on]   |                                  |
                 |  --> CE init (max SSID 3)        |
                 |  --> CE keepalive (241 bytes)     |
                 |                                  |
                 |  <-- CE link time (delay: 600)   |
                 |  --> CE link time (delay: 2)     |
                 |  <-- CE init (max SSID 14)       |
                 |                                  |
                 |         [CONNECTED]               |
                 |                                  |
  [~21s keepalive cycle, repeats]                   |
                 |  <-- CE keepalive                |
                 |  --> CE keepalive (echo)          |
                 |  --> CE link time (delay: 2)     |
                 |  <-- CE link time (delay: N)     |  N converges down
                 |  --> CE link time (delay: 2)     |
                 |                                  |
  [route exchange after first keepalive]            |
                 |  --> 3+\r  (request token)       |
                 |  --> 3IW2OHX331 \r (our route)   |
                 |  --> 3-\r  (release token)       |
                 |  <-- 3<records>\r  (batch 1..N)  |
                 |  <-- 3-\r  (release token)       |
                 |                                  |
  [cycle repeats every ~21s]                        |
```

---

## 7. Linux AX.25 Kernel Tuning

### The Problem

The Linux kernel AX.25 stack has a default T2 (acknowledgement delay) of
3000ms. This means the kernel waits up to 3 seconds before sending an RR
acknowledgement for received I-frames. FlexNet nodes like xnet use
aggressive retransmit timers (~70ms for AXUDP links), so they retransmit
before the RR arrives, causing the kernel to emit REJ frames for duplicate
sequence numbers.

### The Solution

flexnetd automatically tunes the kernel AX.25 parameters on startup by
writing to `/proc/sys/net/ax25/<interface>/`:

| Parameter | Default | flexnetd sets | Reason |
|-----------|---------|---------------|--------|
| `t2_timeout` | 3000ms | 1ms | Immediate ack, no piggybacking delay |
| `standard_window_size` | 2 | 7 | Match axports config, allow more in-flight |

This is done in `ax25_tune_interface()` which resolves the axports port
name to the kernel interface name and writes the values at daemon startup.

### AX25_PIDINCL

Linux AX.25 SEQPACKET sockets default to PID=F0 for all I-frames. The
`setsockopt(SOL_AX25, AX25_PID, ...)` call silently fails on many kernels.

flexnetd uses `setsockopt(SOL_AX25, AX25_PIDINCL, 1)` which reliably
enables per-frame PID control:

- **send()**: First byte of buffer = PID byte (extracted by kernel)
- **recv()**: PID byte prepended to delivered data

PIDINCL is enabled **only** on FlexNet neighbor sessions. User sessions
that exec URONode must NOT have PIDINCL enabled (it corrupts text I/O).

---

## 8. Output Files

flexnetd writes four files that URONode / flexdest / operators read for
FlexNet routing state:

### Gateways File

Default: `/usr/local/var/lib/ax25/flex/gateways`

```
addr  callsign  dev  digipeaters
00000 IW2OHX-14 ax1 IW2OHX-3
```

Format matches the original flexd output. The `dev` field is the **kernel
interface name** (e.g., `ax1`), not the axports port name. The optional
`digipeaters` field lists our callsign so outbound FlexNet connects include
it in the AX.25 via-list for digipeater path preservation (see §6.5). URONode uses
the `dev` field to create AX.25 sockets for outbound connections.

### Destinations File

Default: `/usr/local/var/lib/ax25/flex/destinations`

```
Dest     SSID    RTT Via
DB0AAT    0-9        7 IW2OHX-12
DK0WUE    0-13       2 IW2OHX-12
IR3UHU    1-1       88 IW2OHX-14
IW2OHX    3-3        1 IW2OHX-14
...
```

Written atomically (write to `.tmp`, then `rename()`) on every incoming
compact-routing frame. Entries with RTT >= `Infinity` (60000) are filtered
out. The VIA field shows the next-hop callsign from the CE routing data
(falls back to the per-port neighbor if not specified).

**v0.7.1 multi-port merge (M6.5):** When multiple CE children are active
(one per configured Port), each writes its own
`destinations.<port_name>` file (e.g. `destinations.xnet`,
`destinations.pcf`) with its view of the table.  A `flock`-serialised
merge produces the unified `destinations` file by picking, for each
(callsign, SSID range) pair, the entry with the **lowest RTT** across
all peers.  This means users see the best available path in the
single unified file:

- Routes with shorter RTT via pcf → `Via IW2OHX-12`
- Routes with shorter RTT via xnet → `Via IW2OHX-14`
- Routes known only to one peer → that peer's `Via`

A `destinations.lock` file is created alongside for the flock sentinel.

### Linkstats File

Default: `/usr/local/var/lib/ax25/flex/linkstats`

One row per active CE peering, L-table format (matches xnet's `l` output).
Multi-port deployments use per-port files + flock merge, same pattern as
destinations (M6.6).

### Paths File (M5.3)

Default: `/usr/local/var/lib/ax25/flex/paths`

In-memory cache of CE type-7 (Route/Traceroute REPLY) responses, flushed
to disk on each new reply. Consumed by `flexdest -r <callsign>` to show
the recorded hop chain to a destination.

---

## 9. Development Methodology

flexnetd was developed using a capture-driven approach:

1. **Traffic monitoring**: A Python agent (`xnet_agent.py`) connects to
   the FlexNet node via telnet, enables sysop monitor mode, and captures
   all AX.25 frames to structured JSON files.

2. **Protocol analysis**: Captured frames were analyzed to document the
   FlexNet CE/CF protocol behavior, timing patterns, and frame formats.

3. **Iterative implementation**: Each protocol feature was implemented,
   deployed, and validated against fresh captures. Link table metrics
   (Q/T, RTT, rr+%, dst count) provided quantitative feedback on each
   change.

4. **Kernel tuning**: Capture analysis revealed that L2 REJ frames were
   caused by the Linux kernel's 3-second T2 ack delay. Reducing T2 to
   1ms eliminated all REJs.

### Key metrics tracked during development

| Metric | Start | Final |
|--------|-------|-------|
| Route visible (dst) | 0 | 1 |
| Q/T (link quality) | 600 (infinity) | 1 |
| RTT (round-trip) | 600/600 | 0/2 |
| rr+% (retransmit rate) | 100% | 0.0% |
| Destinations | 0 | ~200 |

---

## 10. Changelog

### v0.7.7 (2026-04-21)

**Critical fix: disable proactive type-4 TX — production xnet withdraws routes on receiving it.**

**Symptom**

After every session with xnet (IW2OHX-14), the remote destinations
file grows to ~60–65 entries, then stays at that count.  A few
seconds later xnet's `L` row for IW2OHX-3 shows Q climbing and RTT
climbing.  Routes that xnet initially advertised are marked "link
down" in the next routing frame and never come back.

**Root cause — from live monitor capture**

Using `sudo listen -a -p xnet` on the IW2OHX host, the timeline at
session setup shows:

```
15:47:21.115  IW2OHX-14 -> IW2OHX-3 len=244 FlexNet: Routing
              22 records, real RTTs: SV1DZI 12-12 delay:56, VA3BAL 1-1 delay:62, ...
15:47:21.118  IW2OHX-14 -> IW2OHX-3 len=238 FlexNet: Routing    (21 more records)
15:47:21.119  IW2OHX-14 -> IW2OHX-3 len=244 FlexNet: Routing    (22 more records)
              — xnet advertised 65 routes, real RTTs —

15:47:21.123  IW2OHX-3 -> IW2OHX-14  ctl I15 len=3
              FlexNet: unknown packet type
              0000  42.              ← OUR type-4 frame: '4' '2' '\r'

              — 21-second silence —

15:47:42.125  IW2OHX-14 -> IW2OHX-3 len=242 FlexNet: Routing
              24 records all "link down"
15:47:42.129  IW2OHX-14 -> IW2OHX-3 len=242 FlexNet: Routing
              24 records all "link down"
15:47:42.132  IW2OHX-14 -> IW2OHX-3 len=172 FlexNet: Routing
              17 records all "link down"
              — xnet withdrew all 65 routes —
```

The production xnet node identifies itself as **`(X)NET/DLC7 V1.39`**.
That version does NOT have a type-4 dispatcher handler — my v0.7.2 RE
was done on a newer `linuxnet` V2.1 binary where slot 4 of the CE
jump table (at rodata 0x0808fca4) is bound to a routing-seq gossip
handler.  V1.39's parser falls through to the "unknown packet type"
path, and 20 seconds later xnet aggressively withdraws routes.

linbpq-flexnet (running on IW2OHX-13, keeping 60+ routes stable for
8+ days) **never** emits type-4.  Our flexnetd added proactive
type-4 TX in v0.7.2 based on the V2.1 RE.  That addition was
premature on the V1.39 production peer.

**Fix**

Disable the proactive type-4 TX block in `run_native_ce_session()`.
The RX parsing path stays — harmless, forward-compatible with V2.1+
peers that might send type-4.  No configuration change.

**Expected effect**

- Destinations file grows to the full ~65 entries and stays there.
- Xnet does NOT withdraw routes 20 s after advertising them.
- Xnet's `L` row for IW2OHX-3 stabilises instead of climbing.

### v0.7.6 (2026-04-21)

**Config recommendation change — xnet port should use `lt_reply=0`, not `lt_reply=20`.**

Docs-only release, no code change.  v0.7.3 introduced per-port
`lt_reply_interval` and recommended `lt_reply=20` on xnet ports.  Live
testing showed that 20 s does not converge xnet's smoothed RTT to the
same level as the linbpq-flexnet reference implementation (which
replies on every peer frame with no rate limit):

```
Port   lt_reply  xnet L row for us
-----  --------  ---------------------------------------
xnet   320 (v0.7.2) → Q=301 RTT=600/2 (infinity sentinel — never converges)
xnet    20 (v0.7.3) → Q=122 RTT=242/2 (converges to our send cadence ~24 s)
xnet     0 (v0.7.6) → Q=2   RTT=2/2   (matches linbpq-flexnet on IW2OHX-13)
```

With `lt_reply=0`, every peer keepalive and every peer link-time
immediately triggers a reply from us.  Xnet's inter-frame gap
measurement collapses from ~20 s to ~200 ms (the L2 round-trip) and
its smoothed-RTT loop converges to wire value 2.

Pcf behaviour is unchanged — keep `lt_reply=320` on the pcf port.
The 320 s ts_ahead window means replying faster saturates pcf's RTT
at 4095.

`flexnetd.conf`, `flexnetd.conf.debug`, and the `LinkTimeReplyInterval`
comment block are updated with the new recommendation.

**Upgrade:** Pull + rebuild is **optional** (no code change), but edit
your config:

```diff
-Port xnet IW2OHX-14 IW2OHX-3 route_advert=60 lt_reply=20
+Port xnet IW2OHX-14 IW2OHX-3 route_advert=60 lt_reply=0
 Port pcf  IW2OHX-12 IW2OHX-3 route_advert=0  lt_reply=320
```

Restart flexnetd.  Within 2–3 minutes xnet's `=>l` row for IW2OHX-3
should move from `Q=122 RTT=242/2` to `Q=2 RTT=2/2`.

### v0.7.5 (2026-04-21)

**Fix: destinations file showing RTT=0 for every entry.**

After the v0.7.4 fix removed the 60-second write gate, users saw 60
destinations in `fld` — but every one with `RTT=0`.  Debug log showed
`dtable_merge: IMPR ...  208(20.8s) -> 0(0.0s)` repeating for every
record.

**Root cause**

Xnet advertises its FlexNet dtable in two rounds after session init:

1. First round (3 compact frames, ~60 unique records): real measured
   RTTs — `HB9ON 8-8 242`, `I0OJJ 3-3 208`, etc.
2. Second round (3 more frames, same ~60 records), ~20 s later:
   every record carries `RTT=0` on the wire.

The second round is almost certainly a protocol-level refresh /
keepalive marker (or xnet echoing back the routes it knows via us).
It is NOT a real "zero round-trip" measurement.  But `dtable_merge`
treated it as an improvement because `0 < 242`, overwriting every
real RTT with 0.

Raw bytes from the second-round frames confirm `RTT=0` is literally
what xnet is sending:

```
0000  CE 33 48 42 39 4F 4E 20 38 38 30 20 48 42 39 4F  |.3HB9ON 880 HB9O|
                           ^^ ^^ ^^
                       SSID 8 8  RTT 0
```

**Fix**

`dtable_merge` now skips the merge entirely when `incoming->rtt == 0`:

- If an entry already exists, touch `last_updated` only — preserve
  the existing `rtt`, `port`, etc.
- If no entry exists, skip the insert — a row with `RTT=0` in the
  displayed `destinations` file is useless, and the destination
  will re-advertise with a real RTT in the next cycle.

This does not affect withdrawal handling: withdrawn routes arrive
with `RTT=60000` (or a trailing `-`), never `RTT=0`, so the infinity
path is unchanged.

**Expected effect**

After the next session setup, the `destinations` file should show
~60 destinations with their correct RTT values (208, 242, etc.),
matching what xnet actually measured.

### v0.7.4 (2026-04-21)

**Critical fix — destinations file truncated to first 20 entries.**

A latent bug present since v0.3.0 was silently dropping 80–95% of the
routes xnet advertises to us.  The fix is a one-line change in
`poll_cycle.c`.

**Symptom**

After session establishment, `fld` and the `destinations` file consistently
showed only the first 20 routes (alphabetically, ending at `IQ5KG-7`).
Logs showed the full route set *was* being received and parsed — the
following appears in the debug log at session init:

```
compact frame — 20 entries parsed, 20 total merged
destinations_merge: wrote unified file with 20 rows  ← write fired
compact frame — 20 entries parsed, 40 total merged   ← no write
compact frame — 20 entries parsed, 60 total merged   ← no write
compact frame — 24 entries parsed, 84 total merged   ← no write
compact frame — 24 entries parsed, 108 total merged  ← no write
compact frame — 12 entries parsed, 120 total merged  ← no write
```

All 120 records arrived within ~22 seconds, all merged into the
in-memory dtable, but only the first batch made it to the on-disk
`destinations` file.

**Root cause**

`run_native_ce_session()` rate-limited the call to
`output_write_destinations()` to once every 60 seconds:

```c
if (merged_total > 0 && now - last_dest_write >= 60) {
    output_write_destinations();
    last_dest_write = now;
}
```

That was benign while xnet advertised routes in a slow trickle, but
since xnet actually dumps its entire FlexNet dtable in a 20–30 s
burst at session setup and then goes silent (routes are re-advertised
only when they change), the gate blocked batches 2–6 and never
reopened.  Users saw 20 entries for the whole session lifetime, even
though the in-memory state had all 120.

**Fix**

Remove the rate limit; write the destinations file after every
compact-record batch.  The cost is negligible (~20 KB of disk I/O per
batch, at most 6–8 batches per session setup) and after the initial
burst xnet stops sending compact records, so the writes naturally
stop too.

The `last_dest_write` local variable is still tracked for diagnostics
but no longer gates the write.

**Expected effect**

`fld` and the `destinations` file should now contain the full ~120-entry
FlexNet destination table within ~30 seconds of session establishment,
matching what the in-memory dtable shows.

### v0.7.3 (2026-04-21)

Fixes xnet's smoothed-RTT convergence by making the link-time reply
interval **per-port** and restoring the inline reply path that M6.9.2
had removed.

**Problem**

After v0.7.0 / M6.9.2, flexnetd sent link-time frames to the peer
only via the proactive 20 s timer, gated by the global
`LinkTimeReplyInterval=320`.  On the xnet port this meant one type-1
frame every 5+ minutes — far too slow for xnet's smoothed-RTT loop,
which expects continuous ~20 s cadence.  Xnet's smoothed value stayed
at its 60000-tick infinity sentinel → every type-1 reply it sent to
us carried the string `"1600"` → its `L` display showed our row as
`Q=301, RTT=600/2`.

Reference implementation `linbpq-flexnet` (running on IW2OHX-13,
converging cleanly with xnet to `Q=2, RTT=2/2`) replies with a
link-time frame:
- on **every received peer keepalive**
- on **every received peer link-time**
- **every 21 s** from its periodic timer

No rate limit.  Our previous "one per 320 s" pacing was the problem.

**Fix**

1. **Per-port `lt_reply_interval`** — `PortCfg` gains a new field; the
   config parser accepts `lt_reply=<sec>` as an optional token on
   `Port` lines (same pattern as `route_advert=`).  `-1` means
   "inherit the global `LinkTimeReplyInterval`".
2. **Inline reply restored** — when the peer sends us a link-time,
   we now reply inline with our own (gated by the effective per-port
   interval).  On xnet ports (`lt_reply=20`) this fires every cycle
   and xnet's smoothed converges to ≈ 2 s wire value.  On pcf ports
   (`lt_reply=320`) the gate still fires only once per 320 s window,
   so the existing pcf behaviour is preserved.

**Configuration**

Recommended `Port` lines:

```
Port xnet  IW2OHX-14  IW2OHX-3  route_advert=60  lt_reply=20
Port pcf   IW2OHX-12  IW2OHX-3  route_advert=0   lt_reply=320
```

`lt_reply=0` is also valid and means "reply on every peer frame" —
equivalent to linbpq-flexnet's behaviour.

**Back-compat**

- Existing configs without `lt_reply=` inherit the global
  `LinkTimeReplyInterval` (default 320 s) — identical to v0.7.2
  behaviour for those ports.
- The `route_advert=` syntax is unchanged.
- Legacy flat-keyword configs (no `Port` blocks) synthesise a
  `ports[0]` with both `route_advert_interval = -1` and
  `lt_reply_interval = -1` (inherit globals).

**Expected effect**

- Xnet port: smoothed converges within 2–3 cycles; xnet's `L` row
  for IW2OHX-3 should move from `Q=301 RTT=600/2` to something near
  `Q=2 RTT=2/2` (matching the IW2OHX-13 row which runs the reference
  implementation).
- Pcf port: unchanged 320 s cadence; pcf's existing behaviour
  preserved.

### v0.7.2 (2026-04-21)

Protocol alignment pass: wire-format corrections so the daemon emits
frames that exactly match the specification in `PROTOCOL_SPEC.md`.

**CE keepalive format**

The keepalive frame is now `'2'` + 240 spaces (241 bytes, all spaces
after the leading `'2'`) with no trailer.  Earlier versions emitted
`'2'` + 237 spaces + `'10\r'` — that trailer was never part of the
keepalive; the `'1x\r'` bytes seen in traffic captures alongside
keepalives are a separate type-1 link-time frame that follows
immediately after.

RX now accepts any `'2'` + all-spaces payload regardless of length,
covering both the (X)Net variant (241 bytes) and PCFlexnet (201 bytes).
`ce_build_keepalive()` updated; `CE_KEEPALIVE_LEN = 241` remains the
TX size.

**CE type-1 link-time classification unified**

`'10\r'` / `'11\r'` / `'12\r'` are ordinary type-1 link-time frames
with decimal values 0 / 1 / 2 — not a distinct frame kind.  The
obsolete `CE_FRAME_STATUS_10` classification has been removed from
the RX path; `ce_parse_frame()` now accepts any `'1'` + decimal
frame ≥ 3 bytes as `CE_FRAME_LINK_TIME`.

**CE type-4 routing-sequence gossip**

Correct wire format is `'4%u\r'` — no flag byte.  RX stores the
peer's sequence number and does not reply.  Previous versions
emitted `'4%d%c\r'` and echoed received type-4 frames back to the
peer; both behaviours are removed in v0.7.2.

TX is now wired to destination-table changes: whenever our
reachable destination count changes between iterations, emit a
`'4<seq>\r'` frame (with a 5 s cooldown to avoid bursts during
initial route ingestion).

**Keepalive period**

`DEFAULT_KEEPALIVE_S` raised from 90 to 180 s to match the
specification.  This is the config default only — the
proactive 20 s send used when the peer is silent (to keep PCFlexnet
peers' link health tracking happy) is unaffected.

**Still open / not yet done**

Four refinement items remain for v0.7.3+:
link-time cadence alignment (init-only vs periodic), proactive
`'3+'` with REJ tolerance, routing batches (240-byte multi-record
frames), and state-6 handling.  See `ROADMAP.md`.

### v0.7.1.2 (2026-04-20)

Reverts v0.7.1.1.  The link-time value 2→0 experiment regressed
production behaviour: pcf's Q/T went up to ~966 and links dropped
multiple times within minutes of deployment.

Production reverts to the v0.7.1 baseline (value "2", 320 s cadence).
All v0.7.1 functional goals remain met.

See v0.7.1.1 entry below for the analysis and v0.7.2 TODO in
ROADMAP.md for the revised plan (cadence mirroring rather than
value change).

### v0.7.1.1 (2026-04-20 — REVERTED in v0.7.1.2)

Experimental: link-time value 2 → 0 based on xnet ↔ pcf capture
showing xnet sends "10\r" rather than "12\r".

Regressed in production (link instability).  See ROADMAP.md for the
revised hypothesis: the CADENCE (xnet sends ONCE per session) may
matter more than the value itself.

### v0.7.1 (2026-04-20)

**M6 fine-tuning and production-readiness for dual-peer deployments.**

- **Priority 1 — via-field fix:** compact routing records received from
  each peer are now tagged with their arrival port index so the
  `destinations` file shows the correct `Via <neighbor>` per entry.
  `ce_parse_compact_records()` gains a `port_idx` parameter; `output.c`
  resolves `Via` via `g_cfg.ports[port].neighbor` instead of always
  falling back to the legacy `g_cfg.neighbor`.
- **Priority 2 — per-port `RouteAdvertInterval`:** after tracing L2
  DM events to the M6.7 periodic re-advertisement, the `PortCfg`
  struct gains its own `route_advert_interval` field.  The config
  parser accepts an optional 4th field on `Port` lines
  (`route_advert=<seconds>`).  Global default changed to **0**
  (disabled) — required for PCFlexnet compatibility, since PCFlexnet
  tears down the L2 link within 10–15 ms of receiving any unsolicited
  compact record.  (X)Net is tolerant — set `route_advert=60` on its
  port to prevent the peer aging our route out at ~120 s.
- **Priority 3 — solved indirectly by Priority 2:** the "pcf stops
  advertising after reconnect" symptom was a consequence of the DM
  cycle, not a separate bug.  With DMs eliminated, pcf's one-shot
  `'3+'` + route-send at session init now remains sufficient for the
  lifetime of the (now-indefinite) session.
- **M6.5 / Priority 4 — per-port destinations + flock merge:**
  `destinations.<port>` files written by each CE child, merged into
  the unified `destinations` via `flock` with best-RTT-per-key
  semantics.  Users see `Via IW2OHX-12` for routes where pcf is
  faster, `Via IW2OHX-14` for routes where xnet is faster, in a
  single coherent output.  Same pattern as M6.6 linkstats.
- **Timing — non-drifting link-time scheduler:** the rate-limit gate
  switched from "elapsed-since-last" to absolute-schedule
  (`next_lt_tx = last + interval`).  Drift from the 20 s proactive
  timer no longer accumulates; link-time sends now land at exactly
  N × `LinkTimeReplyInterval` past session start, giving clean
  1-tick samples in pcf's RTT history instead of the mixed 100/200
  ticks previously observed.
- **Gateways file — multi-port fix:** `output_write_gateways()` was
  still using the legacy single-port fields, so the gateways file
  listed only `ports[0]` regardless of how many `Port` blocks were
  configured.  URONode then had no way to resolve routes advertised
  as `Via IW2OHX-12` (pcf) and fell back to `IW2OHX-14` (xnet) or
  failed outright.  Now iterates all configured ports:
  ```
  addr  callsign  dev  digipeaters
  00000 IW2OHX-14 ax1 IW2OHX-3
  00001 IW2OHX-12 ax2 IW2OHX-3
  ```

**Upgrade notes:**
- `RouteAdvertInterval` default is now **0**.  If you were running
  v0.7.0 with the previous default of 60 and want to keep that
  behavior for xnet, add `route_advert=60` to the xnet `Port` line.
- New files appear in `/usr/local/var/lib/ax25/flex/`:
  `destinations.<port>` and `destinations.lock` alongside the
  unified `destinations`.  These are managed by flexnetd; no manual
  action needed.
- Configuration syntax is backward-compatible; existing `Port`
  lines without the 4th field inherit the global
  `RouteAdvertInterval` (now 0 by default).

**Known cosmetic limitation — pcf-side Q/T display:**

PCFlexnet's `l *` display for our row may show a periodically-elevated
Q/T value (e.g. `(1143/2)` at ~1 h uptime) even when the link is
fully functional.  PCFlexnet uses **different `ts_ahead` windows in
different states of its internal state machine** (320 s in state 5,
180 s in states 2/3, 0 s in state 6).  A fixed-interval scheduler on
our side — like our current 320 s `LinkTimeReplyInterval` — is
optimal for state-5 windows (producing clean 1-tick samples) but
produces delta overflow (→ 4095 saturation) when our send happens to
land in a state-2/3 window (where pcf expects 180 s intervals).

Impact: **purely cosmetic on pcf's display**.  Route exchange,
session stability, destination merging and outbound connections via
pcf all work correctly.  (X)Net does not exhibit this pattern and
converges cleanly to `Q/T=1`.

Planned for v0.7.2: adaptive `LinkTimeReplyInterval` that reads pcf's
own reported smoothed RTT from its link-time frames and shrinks/grows
our send interval accordingly, aiming for delta=0 across all pcf
states.

### v0.7.0 (2026-04-19)

**M6 — Multi-port, multi-neighbor support + PCFlexnet interop:**

- **M6.1** Config: repeatable `Port <name> <neighbor> <listen_call>` blocks
  (up to 4 ports).  Legacy `Neighbor`/`PortName`/`FlexListenCall` keywords
  still work — synthesised into `ports[0]` for back-compat.
- **M6.2** Multi-listener `select()` loop.  Each configured port gets its
  own `ax25_listen()` socket; the parent dispatches incoming connects
  to per-port CE session children.
- **M6.2b** Same-callsign bind across ports via `ax25d`'s
  `fsa_digipeater[0]` device-selector trick.  `IW2OHX-3` can be bound
  simultaneously on `ax1` (xnet) and `ax2` (pcf) with the kernel
  correctly routing inbound SABM to the right port.
- **M6.3** Proactive CE keepalive timer (every 20 s).  Prevents silent
  link degrade over AXUDP tunnels when the peer doesn't autonomously
  send keepalives.
- **M6.6** Per-port `linkstats.<port>` files + `flock`-serialised merge
  into unified `linkstats`.  URONode's `fl` command now shows one row
  per active CE peering instead of racing on a single file.
- **M6.7** Periodic route re-advertisement (default 60 s) keeps our
  callsign fresh in the peer's destination table.  Configurable via
  `RouteAdvertInterval`.
- **M6.8** CE keepalive trailer adjustment (later superseded in
  v0.7.2 — the correct wire format is `'2'` + 240 spaces, no
  trailer).
- **M6.9** `LinkTimeReplyInterval` (default 320 s) rate-limits our
  link-time frames.  PCFlexnet's internal expected-reply timestamp
  is set to `now + 3200` ticks (= 320 s) after each RTT update.
  Frames arriving before this timestamp produce a negative delta
  that wraps and clamps to `4095`, producing the "saturated RTT"
  symptom observed in pre-v0.7.0 runs.
- **M6.9.1** Seed `last_lt_tx = now − interval` on CE session start to
  prevent handshake-burst frames from all passing the rate gate.
- **M6.9.2** Remove reply-to-peer-link-time path; proactive timer
  handles all link-time sends cleanly.
- **M6.9.3** Route-exchange fix — don't echo `'3+'` when replying to
  peer's `'3+'`.  PCFlexnet's `'+'` handler rejects `'3+'` when its
  token state is non-zero, which it always is after receiving its
  own `'3+'`.  The echo triggered DISC of the L2 link within 2 ms —
  killing every prior route-exchange attempt with pcf.  With the
  fix, pcf's full d-table (192 entries) now transfers successfully.
- **M6.9.4** Periodic route re-advertisement sends compact record ONLY
  (no `'3+'`, no `'3-'`).  The PCFlexnet compact-record handler
  processes records unconditionally without touching the token
  state machine — safe for repeated refreshes.

**PCFlexnet timing model** — characterised from behavioural
observation:
  - 100 ms timer tick drives RTT updates
  - RTT update: clamp at 4095, minimum 1, 16-slot circular history,
    IIR-smoothed as `(peer_val + avg + 1) / 2`
  - Expected-reply window: `now + 3200` ticks (= 320 s) when
    smoothed ≥ 96, or `(smoothed + 4) × 32` otherwise
  - Token-state machine with a reject path when state is non-zero
  - Command-dispatch supports `/c` `/m` `/q` `/s` `/t` `/w` slash-commands

### v0.6.0 (2026-04-19)

**M5 — Route path display:**
- CE type-6 / type-7 path query protocol implemented
  (Route REQUEST / REPLY — wire format decoded from PC/FlexNet binaries)
- `ce_build_path_request` / `ce_build_path_reply` builders
- `ce_parse_path_frame` parser for both directions
- QSO correlation table tracks in-flight queries (timeout 30s)
- Periodic eager probing: one destination per ~60s, round-robin
- New cache file `/usr/local/var/lib/ax25/flex/paths` with format:
  `<target> <kind> <n_hops> <unix_ts> <hop1> <hop2> ...`
- New config option `PathsFile`
- `flexdest -r` shows `*** route:` line from cache, or partial
  fallback if no cache entry yet

### v0.5.0 (2026-04-14)

**M2 — Digipeater path preservation (outbound FlexNet connects):**
- Gateways file now includes our callsign as digipeater:
  `00000 IW2OHX-14 ax1 IW2OHX-3`
- URONode builds outbound SABM with our callsign in the AX.25 via-list
- **AX25_IAMDIGI kernel fix**: `setsockopt(fd, SOL_AX25, AX25_IAMDIGI, &1)`
  required before `connect()` — without this, the kernel clears all H bits
  on outbound connections regardless of what userspace sets
- H bit (`|= 0x80`) set on our digi entry so it goes out as `IW2OHX-3*`
  (already-repeated), enabling the neighbor to process the next digi
- Fixed undefined behavior in URONode `gateway.c` argv building
  (`k++` side-effect in array indexing replaced with clean for-loop)
- **Confirmed live**: IW7CFD connects from URONode to IR5S, `U` command
  on IR5S shows: `IR5S>IW7CFD-15 v IQ5KG-7 IW2OHX-3`

### v0.4.1 (2026-04-14)

**M3 — Link health display:**
- New `LinkStats` struct tracks Q/T, RTT, tx/rx bytes, connect time
- `/usr/local/var/lib/ax25/flex/linkstats` updated every 30s in xnet L-table format
- Q/T = 1 for direct link, RTT measured from link-time round-trip
- New `LinkStatsFile` config option

**M4.1 — VIA field in destinations:**
- Destinations file shows actual VIA callsign instead of `00000`
- `dtable_merge()` preserves `via_callsign` on route updates

**M4.2/M4.3 — flexdest destination query tool:**
- New standalone `flexdest` binary for D command with pattern matching
- Supports exact match, prefix wildcard (`IW*`), SSID-specific (`IR5S-7`)
- No libax25 dependency, reads destinations file directly

**Protocol discovery — FlexNet L3 connections:**
- Live multi-hop captures confirmed FlexNet L3 routing uses AX.25
  digipeater chains, NOT CREQ/CACK framing
- User identity preserved as L2 source through via-list digipeating
- Standard SABM/UA/DISC with intermediate nodes in via field

### v0.4.0 (2026-04-13)

**M1 — Protocol completeness:**
- CE SSID encoding for SSID >= 10: single-char `0x30+N` (`':'`=10 through `'?'`=15)
- Init frame byte 0 fix: always `0x30`, no double-init in server mode
- L3RTT c3/c4 set to 0 when link is down (no routes), non-zero when active

**Debug logging:**
- New `-l <logfile>` CLI option for dual console + file output
- Usage: `flexnetd_debug -f -vvv -l /tmp/flexnetd.log`

### v0.3.0 (2026-04-11)

**Protocol fixes:**
- Send link time on every keepalive cycle (was: only once at connection
  start). This enables Q/T convergence from 600 to single digits within
  5 minutes.
- Fixed `got_setup` flag not being set in socket mode. The
  `CE_LINK_SETUP_BYTE` (0x3E) handler only fires in pipe mode; added
  `got_setup = 1` to the init handshake handler for socket mode.
- Proactive route advertisement: `send_own_routes()` sends `3+\r` +
  compact record + `3-\r` after first keepalive exchange, since the
  neighbor does not send `3+` (request token) unprompted.

**User session fix:**
- Removed 10-second peek timeout for non-neighbor connections. URONode
  is now exec'd immediately on connect, so the MOTD appears without
  the user having to send CR first.

**Gateway file fix:**
- Gateways file now writes the kernel interface name (`ax1`) instead
  of the axports port name (`xnet`). Fixes URONode crash when users
  try to connect to FlexNet destinations from telnet.

**Kernel tuning:**
- `ax25_tune_interface()` sets T2=1ms and window=7 on the AX.25
  interface at daemon startup. Eliminates L2 REJ frames caused by the
  default 3-second ack delay.

**Destination file:**
- Periodic write every 60 seconds during active CE sessions (was: only
  written on session disconnect).

### v0.2.0 (2026-04-10)

**Initial working release:**
- Server mode with FlexNet CE/CF protocol handler
- CE session forked into child process (accept loop stays free)
- AX25_PIDINCL on neighbor sessions only
- Init handshake, keepalive exchange, compact routing record parsing
- Token exchange echo
- L3RTT probe response
- CE destination broadcast parsing
- D-table merge and output file generation
- Client mode (D-command polling, legacy compatibility)
- URONode-compatible gateways and destinations file output
- Atomic file writes (temp + rename)

### v0.1.0 (2026-04-09)

**Proof of concept:**
- Basic AX.25 socket operations (listen, accept, connect, send, recv)
- Configuration file parser
- CE frame classifier
- Logging framework (stderr + syslog)

---

## 11. Source Files

| File | Purpose |
|------|---------|
| `flexnetd.c` | Main: server loop, accept, neighbor/user dispatch |
| `flexnetd.h` | Types, constants, CE_FRAME_* codes, function declarations |
| `ce_proto.c` | CE protocol: init, keepalive, compact records, type-0/1/4/6 |
| `cf_proto.c` | CF protocol: L3RTT build/parse, D-table line parse |
| `poll_cycle.c` | Session handler: native CE/CF mode + D-command client mode |
| `ax25sock.c` | AX.25 socket ops: listen, accept, connect, send/recv, PIDINCL, kernel tuning |
| `dtable.c` | Destination routing table: merge, find, sort, dump |
| `output.c` | Write URONode flex files (gateways + destinations) |
| `config.c` | Config file parser |
| `util.c` | Callsign utils, RTT display, hex dump |
| `log.c` | Logging (stderr / syslog) |
| `Makefile` | Build targets: all, debug, asan, syntax, install |
