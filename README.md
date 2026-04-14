# flexnetd - FlexNet Routing Daemon for Linux AX.25

**Version 0.4.1** | Author: IW2OHX | License: GPL v3 | April 2026

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

# Neighbor
Neighbor        IW2OHX-14       # FlexNet peer callsign
PortName        xnet            # axports port name

# Server binding
FlexListenCall  IW2OHX-3        # Callsign to bind (must match axports)
Role            server          # 'server' (native CE/CF) or 'client' (D-cmd)

# Timers
PollInterval    240             # Client mode: seconds between D-cmd polls
KeepaliveInterval  90           # Keepalive cycle (seconds)
BeaconInterval  120             # Beacon interval (seconds)

# Output files (URONode reads these)
GatewaysFile    /usr/local/var/lib/ax25/flex/gateways
DestFile        /usr/local/var/lib/ax25/flex/destinations

# Protocol
Infinity        60000           # RTT value = unreachable
TriggerThreshold  50            # Route change threshold

# Logging
LogLevel        3               # 1=error, 2=warn, 3=info, 4=debug
Syslog          yes             # Log to syslog (daemon mode)
```

### Command-line Options

```
flexnetd [-c config] [-d] [-f] [-v[vv]] [-V]
  -c file   Config file (default: /usr/local/etc/ax25/flexnetd.conf)
  -d        Daemon mode (fork, log to syslog)
  -f        Foreground mode (log to stderr)
  -v        Increase verbosity (repeat for more: -vvv = DEBUG)
  -V        Print version and exit
```

---

## 6. FlexNet Protocol Reference

The FlexNet protocol uses two AX.25 PID values over connected-mode
(SEQPACKET) I-frames. The protocol is text-based (ASCII) with
fixed-width fields.

Protocol behavior documented from live packet captures between
flexnetd and (X)NET nodes.

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
| `'1'` (0x31) | Link Time | Link delay measurement (100ms ticks) |
| `'2'` (0x32) | Keepalive | Poll frame (241 bytes) |
| `'3'` (0x33) | Routing Data | Token signals and compact routing records |
| `'4'` (0x34) | Token | Token/sequence exchange |
| `'6'` (0x36) | Dest Broadcast | Individual destination update |

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

#### Destination Broadcast (type '6')

Individual destination update for triggered/incremental routing.

```
'6' ' ' <5-digit RTT> <callsign_info> ' ' <via_callsign> '\r'
```

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

### 6.5 Link Establishment Sequence

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

flexnetd writes two files that URONode reads for FlexNet routing:

### Gateways File

Default: `/usr/local/var/lib/ax25/flex/gateways`

```
addr  callsign  dev  digipeaters
00000 IW2OHX-14  ax1
```

Format matches the original flexd output. The `dev` field is the **kernel
interface name** (e.g., `ax1`), not the axports port name. URONode uses
this field to create AX.25 sockets for outbound FlexNet connections.

### Destinations File

Default: `/usr/local/var/lib/ax25/flex/destinations`

```
callsign  ssid     rtt  gateway
DB0AAT    0-9        7    00000
DK0WUE    0-13       2    00000
IW2OHX    3-3        1    00000
...
```

Updated atomically (write to `.tmp`, then `rename()`) every 60 seconds
during active CE sessions. Entries with RTT >= Infinity (60000) are
filtered out.

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
