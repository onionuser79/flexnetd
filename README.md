# flexnetd v1.0.0

A native FlexNet (CE/CF) routing daemon for Linux AX.25. flexnetd peers
directly with (X)Net and PC/Flexnet nodes over AX.25 (RF, AXIP or AXUDP),
learns their destination tables, advertises the local node, and gives
**URONode** the files it needs to route users into the FlexNet network.
It replaces the legacy text-polling `flexd`.

Licence: GPL v3.

- **What's new:** [RELEASE_NOTES.md](RELEASE_NOTES.md)
- **What's next:** [ROADMAP.md](ROADMAP.md)
- **The protocol, for implementers:** [PROTOCOL_SPEC.md](PROTOCOL_SPEC.md)

```
  (X)Net / PC/Flexnet neighbour  ══ FlexNet CE/CF ══  flexnetd
                                                         │
                                  destinations, gateways ├──► URONode (user connects out)
                                  user connects in       └──► exec uronode
```

---

## Contents

1. [Features](#features)
2. [Compatibility](#compatibility)
3. [Installation](#installation)
4. [Configuration](#configuration)
5. [Parameter reference](#parameter-reference)
6. [Running](#running)
7. [Output files and `flexdest`](#output-files-and-flexdest)
8. [Troubleshooting](#troubleshooting)
9. [Known limitations](#known-limitations)
10. [Source layout](#source-layout)

---

## Features

- FlexNet link protocol with each configured neighbour: init handshake,
  keepalive, link-time measurement, compact route exchange, L3RTT
  probes.
- Advertises the node's callsign, optionally with an SSID range.
- Up to **4 FlexNet neighbours**, one per AX.25 port, all sharing one
  listen callsign. Destination tables from all neighbours are merged,
  keeping the cheapest route per destination.
- Per-neighbour settings for the behaviours in which (X)Net and
  PC/Flexnet differ.
- **Server mode**: flexnetd accepts connections on the listen callsign
  itself — FlexNet neighbours get a FlexNet session, every other caller
  is handed to URONode immediately.
- Writes URONode-compatible `gateways` and `destinations` files, a
  per-link statistics file, and a path cache from CE path replies.
  Files are written atomically.
- `flexdest` query tool (no libax25 needed).
- Tunes the kernel AX.25 interface at start-up so FlexNet peers do not
  see delayed acknowledgements.
- **Client mode** (legacy): no peering, polls a FlexNet node's `D`
  command instead.

flexnetd is a FlexNet participant for a URONode system. It advertises
only its own callsign and does not carry other stations' traffic; it is
not a replacement for the dedicated FlexNet routers — (X)Net,
PC/Flexnet, RMNC/Flexnet.

## Compatibility

| Peer | Status |
|---|---|
| (X)Net 1.39 | Tested. Link converges to its best reported cost. |
| PC/Flexnet 3.3g | Tested. Link and route exchange stable; PC/Flexnet's displayed cost for the link stays higher than ideal (see [Known limitations](#known-limitations)). |
| Other (X)Net and PC/Flexnet releases, RMNC/Flexnet | Not tested |

Host: Linux with kernel AX.25 support. Tested on Raspberry Pi OS /
Debian (aarch64). Does not build on macOS (no libax25).

---

## Installation

### 1. Prerequisites

- Kernel AX.25 support (`modprobe ax25`) and an AX.25 port in
  `axports` (RF, AXIP or AXUDP) for each FlexNet neighbour.
- `libax25-dev`: `sudo apt install libax25-dev`
- URONode at `/usr/local/sbin/uronode`, for user sessions.

`make check-deps` checks for libax25 and its headers.

### 2. Build and install

```bash
git clone https://github.com/onionuser79/flexnetd.git
cd flexnetd
make
sudo make install
```

`make install` installs `/usr/local/sbin/flexnetd` and
`/usr/local/sbin/flexdest`, and `/usr/local/etc/ax25/flexnetd.conf`
**only if it does not already exist** — an upgrade keeps your
configuration. After an upgrade, compare it with the shipped
`flexnetd.conf` for new options.

Install the systemd unit yourself:

```bash
sudo cp flexnetd.service /etc/systemd/system/
sudo systemctl daemon-reload
```

### 3. Patch URONode (for outbound connections)

URONode needs a small patch so its outbound FlexNet connections carry the
node's callsign as an already-repeated digipeater. Without it, the
neighbour receives a digipeater entry it does not recognise and drops the
connection.

```bash
cd /path/to/uronode-source
git apply /path/to/flexnetd/patches/uronode-m2-digipeater-path.patch
make clean && make && sudo make install
```

Details: [patches/README.md](patches/README.md).

### 4. Free the listen callsign in `ax25d.conf`

flexnetd binds the listen callsign itself on every FlexNet port. **The
listen callsign must not appear in `ax25d.conf`** for those ports; if
ax25d also binds it, one of the two silently loses. Remove or comment
the `[<listen_call> VIA <port>]` block and reload ax25d. Other ax25d
entries are unaffected:

```
# [NODEA-3 VIA xnet]    <- must be absent: flexnetd owns NODEA-3 on port xnet
[NODEA-7 VIA xnet]      # other callsigns stay with ax25d
NOCALL   * * * * * *  L
default  * * * * * *  -  root  /usr/local/sbin/uronode  uronode
```

### 5. Stop the legacy `flexd`

```bash
sudo systemctl disable --now flexd
```

### 6. Configure, start, verify

Edit `/usr/local/etc/ax25/flexnetd.conf` (next section), then:

```bash
sudo systemctl enable --now flexnetd
sudo journalctl -u flexnetd -f
```

Verify from the neighbour: its link table (`L` on (X)Net) should list
your listen callsign with a low cost within a few minutes. Locally,
`flexdest` should list destinations with real costs.

---

## Configuration

Configuration is `/usr/local/etc/ax25/flexnetd.conf`: one `Keyword value`
per line, `#` starts a comment. It is read at start-up; **restart the
daemon to apply changes** (`SIGHUP` does not reload).

### Minimal configuration

```conf
MyCall     NODEA-3
Alias      NODEA
MinSSID    3
MaxSSID    3
Role       server

Port xnet  NODEB-14  NODEA-3  route_advert=0  lt_reply=0    advert_mode=full
```

### Two neighbours, one of each family

```conf
Port xnet  NODEB-14  NODEA-3  route_advert=0  lt_reply=0    advert_mode=full
Port pcf   NODEC-12  NODEA-3  route_advert=0  lt_reply=320  advert_mode=record
```

These per-port values are the recommended ones: they are what each peer
family needs to keep the link stable and measure it correctly (see
[`Port`](#port-name-neighbour-listen_call-options)).

### Multiple ports, one listen callsign

All `Port` lines may use the same listen callsign. flexnetd binds it on
each port separately, using that port's own `axports` callsign to select
the interface, so incoming connections are delivered to the right
session. Each port therefore needs its own entry in `axports`.

---

## Parameter reference

### Identity and mode

| Keyword | Default | Meaning |
|---|---|---|
| `MyCall` | `NOCALL-7` | Node callsign advertised to FlexNet neighbours |
| `Alias` | `NONE` | Node alias (up to 6 characters), carried in L3RTT probes |
| `MinSSID` / `MaxSSID` | `0` / `15` | SSID range of `MyCall`'s base callsign to advertise, as one destination. Set both to the node's SSID to advertise only that. |
| `Role` | `server` | `server`: native FlexNet peering (requires `Port` lines). `client`: legacy mode, no peering — polls a neighbour's `D` command. |

**Set `MinSSID`/`MaxSSID` explicitly.** The default advertises all 16
SSIDs of your base callsign, claiming callsigns that may belong to other
stations.

### `Port <name> <neighbour> <listen_call> [options]`

One line per FlexNet neighbour, up to 4.

| Field | Meaning |
|---|---|
| `name` | `axports` port name (up to 7 characters) |
| `neighbour` | Callsign of the FlexNet neighbour on that port |
| `listen_call` | Callsign flexnetd listens on and uses as the node's digipeater identity |

Options (any order; omitted = the global default):

| Option | Values | Towards (X)Net | Towards PC/Flexnet |
|---|---|---|---|
| `route_advert=<s>` | `0` = advertise own routes at session start only; `N` = also every N s | `0` | `0` (**required**: PC/Flexnet drops the link on mid-session records after a table exchange) |
| `lt_reply=<s>` | minimum seconds between link-time frames to the neighbour; `0` = reply to every keepalive / link-time frame | `0` (longer intervals leave (X)Net's measurement stuck) | `320` (**required**: faster replies make PC/Flexnet record a saturated cost of 4095) |
| `advert_mode=` | `full` = `3+`, own records, `3-`; `record` = own records only | `full` (default) | `record` (**required**: PC/Flexnet drops the link on an unexpected `3+`) |

A bare integer as the fourth field is accepted as `route_advert=` for
compatibility with older configurations.

### Timers

| Keyword | Default | Meaning |
|---|---|---|
| `RouteAdvertInterval` | `0` | Global default for `route_advert` |
| `LinkTimeReplyInterval` | `90` | Global default for `lt_reply`. The shipped configuration sets `320` (safe for PC/Flexnet) and overrides it per port. |
| `PathProbeInterval` | `0` | Seconds between background CE path queries; `0` disables. Each reply fills the path cache used by `flexdest -r`. |
| `PollInterval` | `240` | Client mode only: seconds between `D` polls |

Keepalives are not configurable: flexnetd answers the neighbour's
keepalives and sends one of its own only when no keepalive has gone out
for 300 s.

### Output files

| Keyword | Default |
|---|---|
| `GatewaysFile` | `/usr/local/var/lib/ax25/flex/gateways` |
| `DestFile` | `/usr/local/var/lib/ax25/flex/destinations` |
| `LinkStatsFile` | `/usr/local/var/lib/ax25/flex/linkstats` |
| `PathsFile` | `/usr/local/var/lib/ax25/flex/paths` |

URONode reads `gateways` and `destinations` from the default location;
change these only if your URONode is configured to match.

### Protocol and logging

| Keyword | Default | Meaning |
|---|---|---|
| `Infinity` | `60000` | Cost treated as unreachable. Do not change. |
| `LogLevel` | `3` | `1` error, `2` warning, `3` info, `4` debug |
| `Syslog` | `no` | `yes` logs to syslog (tag `flexnetd`) |

Accepted for compatibility, currently without effect: `KeepaliveInterval`,
`BeaconInterval`, `TriggerThreshold`, `ProbeCount`. Legacy single-port
keywords `Neighbor`, `PortName` (alias `Interface`) and `FlexListenCall`
are still read when there is no `Port` line; new configurations should
use `Port`.

### Command line

```
flexnetd [-c config] [-d] [-f] [-v[vv]] [-l logfile] [-V]

  -c file   configuration file (default /usr/local/etc/ax25/flexnetd.conf)
  -d        run as a daemon
  -f        run in the foreground, log to stderr
  -v        more verbose; -vvv = debug
  -l file   also log to this file
  -V        print the version and exit
```

---

## Running

- **systemd**: `flexnetd.service` starts the daemon after `ax25.service`
  and restarts it on failure. If your AX.25 stack is not started by
  systemd, start flexnetd after bringing the stack up.
- **Logs**: `journalctl -u flexnetd`, syslog if `Syslog yes`, or the `-l`
  file.
- **Kernel tuning**: at start-up flexnetd sets, for each FlexNet port's
  interface, `t2_timeout` to 1 ms (the default 3 s ack delay makes FlexNet
  peers retransmit and the kernel answer with REJ) and
  `standard_window_size` to 7.
- **Process model**: each FlexNet session and each user session runs in
  its own child process, so a long-lived session never blocks new
  connections.

### Debugging

```bash
make debug                      # builds ./flexnetd_debug (symbols, -DDEBUG)
sudo ./flexnetd_debug -f -vvv -l /tmp/flexnetd_debug.log \
     -c flexnetd.conf.debug
```

`flexnetd.conf.debug` is a template with debug logging and a 10 s path
query interval; set its callsigns and `Port` lines to match your
configuration. Every CE/CF frame is hex-dumped at debug level. For an
on-wire view alongside it: `sudo listen -a -p <port>`.

---

## Output files and `flexdest`

| File | Contents | Written |
|---|---|---|
| `gateways` | One line per neighbour: address, neighbour callsign, **kernel interface** (e.g. `ax1`, not the `axports` name), and the listen callsign as digipeater — which URONode puts in outbound connections | at start-up |
| `destinations` | `Dest SSID RTT Via` — every reachable destination with its cost (100 ms units) and next hop. Unreachable entries are omitted. | on every received route batch |
| `linkstats` | One row per FlexNet link, (X)Net `L`-table style | every 30 s |
| `paths` | Hop chains from CE path replies | on every reply |

With several ports, each session writes `<file>.<port>` and a locked
merge produces the combined file, keeping the cheapest route per
destination.

```bash
flexdest                 # all destinations
flexdest NODEB           # exact callsign
flexdest NODEB-7         # entries whose SSID range contains 7
flexdest NODE*           # prefix
flexdest -r NODEB        # with the recorded hop chain, if known
flexdest -f file -p file # other destinations / paths files
```

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| The neighbour never lists our callsign | The listen callsign is also bound by ax25d | Remove the `[<listen_call> VIA <port>]` block from `ax25d.conf` |
| flexnetd logs "cannot resolve port" | Port name missing from `axports` | Check the `Port` line's name against `axports` |
| PC/Flexnet disconnects shortly after the link comes up | `advert_mode=full` or `route_advert` ≠ 0 on its port | `advert_mode=record route_advert=0` |
| PC/Flexnet shows the link at cost 4095 | `lt_reply` below 320 on its port | `lt_reply=320` |
| (X)Net shows a high cost for the link | `lt_reply` > 0 on the (X)Net port | `lt_reply=0` |
| Kernel sends REJ frames on the FlexNet port | T2 tuning did not apply | Check `/proc/sys/net/ax25/<iface>/t2_timeout` |
| Outbound connects fail; the destination does not see our callsign in its user list | URONode not patched | Apply `patches/uronode-m2-digipeater-path.patch` |
| Our advertised SSID range is wrong on peers | `MinSSID`/`MaxSSID` not set | Set both explicitly |

---

## Known limitations

- **Participant only.** flexnetd advertises its own callsign and does not
  re-advertise routes or carry transit sessions, and it does not relay
  other nodes' path queries.
- **PC/Flexnet's displayed cost** for a flexnetd link settles higher than
  the 1–2 it shows for (X)Net neighbours. Routing and link stability are
  unaffected. A fix is known and planned — see [ROADMAP.md](ROADMAP.md).
- PC/Flexnet recycles AXIP links on a fixed lifetime of about 90 minutes
  (5445 s); the link re-establishes by itself.
- At most 4 FlexNet ports.

---

## Source layout

| File | Contents |
|---|---|
| `flexnetd.c` | `main()`, daemon, signal handling, server accept loop and dispatch |
| `config.c` | Configuration parser |
| `ax25sock.c` | AX.25 sockets: listen, accept, connect, kernel tuning |
| `ce_proto.c` | PID `0xCE` frame builders and parsers |
| `cf_proto.c` | PID `0xCF` frame builders and parsers (L3RTT) |
| `poll_cycle.c` | FlexNet session state machine |
| `dtable.c` | Destination table |
| `output.c` | Atomic writers for the output files |
| `util.c`, `log.c` | Callsign helpers, logging |
| `flexdest.c` | `flexdest` query tool |
| `flexnetd.h` | Shared types and constants |
| `patches/` | URONode patch |
| `tools/test_multi_bind.c` | Manual test of same-callsign binding on several ports |

| Document | For |
|---|---|
| [README.md](README.md) | Installing and configuring |
| [RELEASE_NOTES.md](RELEASE_NOTES.md) | What changed in each release |
| [ROADMAP.md](ROADMAP.md) | Planned work |
| [PROTOCOL_SPEC.md](PROTOCOL_SPEC.md) | The FlexNet protocol, for implementers (shared with linbpq-flexnet) |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Development build, conventions |

Related: [linbpq-flexnet](https://github.com/onionuser79/linbpq-flexnet),
FlexNet for LinBPQ, by the same author.
