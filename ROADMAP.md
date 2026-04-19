# flexnetd — Development Roadmap

**Current version:** v0.4.1 (stable, in production)
**Target version:** v1.0.0
**Author:** IW2OHX
**Started:** April 2026

---

## What's done

### v0.3.0 — Baseline (2026-04-11)

First working release. Native FlexNet CE/CF peering with xnet.

- Server mode: binds IW2OHX-3, accepts neighbor IW2OHX-14 and user sessions
- Bidirectional route exchange: ~187 destinations, Q/T=1
- CE protocol: init handshake, keepalive, link time, compact routing, token
- CF protocol: L3RTT probe/reply for RTT measurement
- URONode integration: destinations file, gateways file, immediate MOTD
- Kernel T2=1ms auto-tuning eliminates L2 REJ frames
- Fork model: CE session in child, user sessions exec URONode

### v0.4.0 — M1: Protocol completeness (2026-04-13)

Correctness fixes from full protocol reverse engineering.

- **M1.1** SSID encoding 0-15: single-char `0x30+N` (`:;<=?>` for 10-15)
- **M1.2** Init frame byte 0 = 0x30 always; no double-init in server mode
- **M1.3** L3RTT c3/c4 = 0 when link down, non-zero when active
- **M1.4** Deferred (CE type-6/7 path query — low priority, safe to skip)
- **Debug logging:** `-l <logfile>` for dual console+file output

### v0.4.1 — M3 + M4: Operational visibility (2026-04-14)

Link health display, destination query, VIA field.

- **M3** Link health file (`/usr/local/var/lib/ax25/flex/linkstats`)
  - Updated every 30s, xnet L-table format
  - Q/T=1 (direct link), RTT from link-time round-trip measurement
  - Tracks tx/rx bytes, frames, connect time, dst count
  - `LinkStatsFile` config option
- **M4.1** VIA callsign in destinations file (replaces `00000`)
  - `dtable_merge()` preserves `via_callsign` on route updates
- **M4.2/M4.3** `flexdest` standalone tool for D command
  - Exact match: `flexdest IR5S`
  - Prefix wildcard: `flexdest IW*`
  - SSID-specific: `flexdest IR5S-7`
  - No libax25 dependency

### v0.5.0 — M2: Digipeater path preservation (2026-04-14)

Outbound FlexNet connects include our node in the AX.25 digipeater via-list.

- **M2.2 Outbound via-list:** URONode gateways file includes IW2OHX-3 as
  digipeater. `flexnetd/output.c` writes `<neighbor> <dev> <our_call>`.
  URONode `do_connect()` builds SABM: `fm USER to DEST via IW2OHX-3 IW2OHX-14`
- **M2 H-bit fix (URONode patch):** `setsockopt(fd, SOL_AX25, AX25_IAMDIGI, &1)`
  before `connect()`. Without this, the kernel clears all H bits on outbound
  connections. With `AX25_IAMDIGI`, the kernel honors the H bit we set on
  `fsa_digipeater[0]`, so IW2OHX-3 goes out as `IW2OHX-3*` (already-repeated).
  This lets xnet (IW2OHX-14) see the frame as addressed to it.

  **Confirmed live** (2026-04-14): IW7CFD connects from URONode to IR5S,
  `U` command on IR5S shows: `IR5S>IW7CFD-15 v IQ5KG-7 IW2OHX-3`

  URONode changes in `gateway.c`:
  - `#define AX25_IAMDIGI 12` fallback
  - `setsockopt(fd, SOL_AX25, AX25_IAMDIGI, ...)` for FlexNet connects
  - H bit `|= 0x80` on first digipeater (our callsign)
  - Fixed UB in argv building (clean for-loop replacing k++ side-effect)

---

### v0.6.0 — M5: Route path display (2026-04-19)

Route trace output per-destination, produced by the peer-to-peer
path query protocol (CE type-6 REQUEST / type-7 REPLY).

- **M5.3 Full path via CE type-6/7** — implemented.
  - `ce_build_path_request` / `ce_build_path_reply`
  - `ce_parse_path_frame`
  - pending query table with QSO correlation and 30-second timeout
  - periodic probing with configurable `PathProbeInterval`
  - path cache file (`/usr/local/var/lib/ax25/flex/paths`)
- **flexdest `-r`** flag reads the cache and prints a line after each
  matched destination:
  ```
  IR5S      0-15     4 IR3UHU-2
  *** route: IW2OHX-14 IR3UHU-2 IQ5KG-7 IR5S   [route 3m42s ago]
  ```
- Type-6/7 wire format (fully decoded and documented in
  `PROTOCOL_SPEC.md`):
  `TYPE + HopCount_byte(0x20+N) + QSO_field(5 bytes, "%5u") + callsigns`
- M5.1/M5.2 (local partial route) remain as fallback when the cache
  doesn't yet have an entry for the requested destination.

#### Peer compatibility (live testing, 2026-04-19)

Live testing against xnet (our current FlexNet neighbor) confirmed:

| Peer           | Sends type-7? | Full path display? |
|----------------|--------------|--------------------|
| xnet (ARM)     | **No**       | **No**             |
| PC FlexNet     | Yes          | Yes (when peered)  |

**xnet does NOT implement CE type-7 replies.** Our earlier RE of
`xnet_arm7` showed no type-7 emit code, and live testing confirmed
it: every type-6 probe we send **times out with no reply at all**.

#### Common misinterpretation — READ THIS FIRST

When probing was enabled, the log showed type-3 compact records
arriving shortly after our type-6 probes, and it was tempting to
read those as "replies" to our probes.  **They are not.**  A
control run with `PathProbeInterval 0` (no probes sent at all)
showed the peer still emits exactly the same periodic type-3
routing updates on its normal ~4-minute broadcast cycle.  The
type-3 arrivals just happened to coincide with our probe timing.

In short: with an xnet peer, a type-6 probe goes out and is
discarded by the peer.  Any type-3 that arrives afterwards is
unrelated routing traffic, not a reply.

The `path_pending` type-3 correlation shortcut in `poll_cycle.c`
(clearing pending slots when the target callsign appears in a
type-3) is kept because it is still technically correct — the
destination is confirmed reachable — but it must not be read as
"xnet responded to our query".

**Consequences:**

1. Against an xnet-only neighbor, `flexdest -r` will always fall
   back to the M5.1-style partial route (`... <via> <dest>`).
2. Full `*** route:` path display requires peering with a
   PC-FlexNet-based node that implements type-7. This lands
   automatically once **M6 (multi-neighbor)** is done and we peer
   with both xnet and a PC-FlexNet node.
3. Production default for `PathProbeInterval` is now **0**
   (disabled) to avoid pointless traffic against xnet neighbors.
   Set to 60 (production) or 10 (debug) when a type-7-capable
   peer exists.
4. flexnetd now correlates incoming type-3 records against pending
   type-6 queries by base callsign and clears the pending slot
   when matched, so no spurious "UNSOLICITED" warnings are logged.
   The destination is confirmed reachable (but full hop list is
   not available from this peer).

### v0.7.0 — M6: Multi-neighbor support (prerequisite for M2.1)

**Goal:** Peer with more than one FlexNet neighbor simultaneously.
Currently flexnetd has a single `Neighbor` in config — all CE/CF state,
dtable merging, and user dispatch assume one peer.

**Motivation:** Today IW2OHX-3 only peers with IW2OHX-14 (xnet). If we
add a second port to peer with IW2OHX-12 (PCFlexnet), we gain:
- Redundancy if one path fails
- Direct access to PCFlexnet destinations
- **Unlocks M2.1** — cross-port transit becomes meaningful when there
  are two ports to forward between

**Kernel binding constraint — resolved (2026-04-19):**

The question "can we bind the same listen callsign (e.g. IW2OHX-3)
to multiple AX.25 ports at once?" was settled empirically with
`tools/test_multi_bind`:

```
[1] bind IW2OHX-3 on ax1 (SO_BINDTODEVICE=ax1)  → PASS fd=3
[2] bind IW2OHX-3 on ax2 (SO_BINDTODEVICE=ax2)  → PASS fd=4
[3] listen() on both                            → PASS
VERDICT: multi-port same-callsign binding WORKS with SO_BINDTODEVICE.
```

Linux AX.25 `ax25_bind()` uniqueness check is (callsign + device +
socket type), so different devices coexist fine as long as each
socket is pinned with `SO_BINDTODEVICE`. **→ architecture A** is
the way forward.

**Architecture A (confirmed):**

```
flexnetd single process, no fork at accept level
  ├─ listen_fd[0]  bind(CALL) + SO_BINDTODEVICE(port_0)
  ├─ listen_fd[1]  bind(CALL) + SO_BINDTODEVICE(port_1)
  └─ select() across all listen_fd[]; on ready → accept()
          ├─ peer == configured_neighbor[N]  → fork CE/CF handler
          └─ peer == anything else           → fork+exec uronode
```

**What's needed:**

| Task | Description | Complexity |
|------|-------------|------------|
| **M6.1 Config: repeatable Port block** | New `Port <name> { Neighbor <call>; ListenCall <call>; ... }` syntax, array of `PortCfg` in `FlexConfig` | Low |
| **M6.2 Multi-listener loop** | `ax25_listen()` once per configured port, `select()` across the array in `run_server()` | Low |
| **M6.3 Accept dispatch** | Match accepted peer against the neighbor list for the port the accept came from; fork CE session or uronode accordingly | Low |
| **M6.4 Per-neighbor CE state** | Each forked CE child already has its own session state; the parent doesn't need multi-state, so this is mostly arg-passing hygiene | Low |
| **M6.5 Shared dtable via file-merge** | Children each write their own `destinations.<port>` and `gateways.<port>` snapshot, parent merges into the single production output file with `flock` to avoid torn reads | Medium |
| **M6.6 Linkstats per neighbor** | `linkstats` file gains one row per active CE session (peer column becomes the key) | Low |

Each child fork already has a private dtable today, which actually
*helps* multi-neighbor: no shared-state locking needed inside the
CE handler.  The only cross-child concern is the destinations file,
solved by per-child snapshot + parent-side merge (M6.5).

### v0.8.0 — M2.1: Inbound transit digipeating (blocked on M6)

**Status:** Blocked — makes sense only with 2+ FlexNet ports.

**Goal:** When a remote user connects THROUGH our node to a destination
beyond us, forward the connection (act as AX.25 digipeater between
ports).

**Key discovery (2026-04-14):** FlexNet L3 connections use **AX.25
digipeater chains**, NOT CREQ/CACK framing.

Captured evidence (IW7BIA from IW2OHX-12 → IR5S via IR3UHU-2):
```
→ fm IW7BIA to IR5S via IW2OHX-12* IW2OHX-14* IR3UHU-2 ctl SABM+
← fm IR5S to IW7BIA via IR3UHU-2* IW2OHX-14 IW2OHX-12 ctl UA-
```

Note: in our current single-port setup, the chain does NOT include
IW2OHX-3 — the FlexNet network routes directly to IW2OHX-14 without
using us as an intermediate. This is why M2.1 is blocked on M6
(multi-port): only then does IW2OHX-3 appear as a cross-port bridge.

**What's needed (when M6 is done):**

| Task | Description | Complexity |
|------|-------------|------------|
| **M2.1 Digipeater support** | When our callsign appears in an inbound via list, mark ourselves as digipeated and forward to next hop on the other port. Option A: kernel AX.25 digipeating (`/proc/sys/net/ax25/<iface>/digi`). Option B: application-level digi in flexnetd (for cross-port cases the kernel may not handle). | Medium |

**First test (when reached):**
```bash
echo 1 > /proc/sys/net/ax25/ax1/digi
echo 1 > /proc/sys/net/ax25/ax2/digi
# Try transit from one port through us to the other
```

### v1.0.0 — Production release

- Full route path display (M5.3 if needed)
- M2.1 transit fully tested across ports
- Production hardening, documentation

---

## What was discarded

The original ROADMAP planned CREQ/CACK/DREQ session framing for M2
(connection setup, session state machine, frame builders/parsers).
Live captures on 2026-04-14 proved this is NOT how xnet handles
FlexNet L3 connections. The entire CREQ/CACK approach was replaced
with AX.25 digipeater support, which is simpler and matches the
confirmed wire protocol.

| Original plan | Status | Reason |
|---------------|--------|--------|
| M2.1 CREQ frame builder | Discarded | Not used — xnet uses AX.25 digipeating |
| M2.3 Session state machine (IDLE→CONNECTING→CONNECTED) | Discarded | No CREQ/CACK handshake exists |
| CREQ/CACK/DREQ parsers | Discarded | These frames were never observed |

---

## Version summary

| Version | What | Status |
|---------|------|--------|
| v0.3.0 | Basic CE/CF peering, route exchange, Q/T=1 | **Released** |
| v0.4.0 | Protocol correctness (SSID, init, L3RTT) + debug logging | **Released** |
| v0.4.1 | Link health, VIA field, flexdest D-command tool | **Released** |
| v0.5.0 | Outbound digipeater path preservation (H-bit + AX25_IAMDIGI) | **Released** |
| v0.6.0 | M5 — Route path display (type-6/7 query + flexdest -r) | **Stable** |
| v0.7.0 | M6 — Multi-neighbor support (two FlexNet ports) | Planned |
| v0.8.0 | M2.1 — Inbound transit digipeating (requires M6) | Blocked on M6 |
| v1.0.0 | Production hardening + full docs | Planned |

---

## Protocol reference

- **CE/CF routing:** `PROTOCOL_SPEC.md` — full wire format from RE
- **FlexNet L3 connections:** AX.25 digipeater chains (confirmed 2026-04-14)
- **Capture evidence:** `monitor_port1_raw.txt`, `monitor_port11_raw.txt`
