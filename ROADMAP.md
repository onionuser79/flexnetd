# flexnetd — Development Roadmap

**Current version:** v0.7.4 (stable)
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

Correctness fixes aligning the daemon with the protocol specification.

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

**xnet does NOT implement CE type-7 replies.** Live testing confirmed
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

### v0.7.0 — M6: Multi-neighbor support + PCFlexnet interop (2026-04-19, stable)

**Shipped.**  Peers with multiple FlexNet neighbors simultaneously
and exchanges routes with PCFlexnet over AXUDP.

**Key behavioural constraints characterised and handled:**
- PCFlexnet's RTT clamp at 4095 saturates when our reply arrives
  before the peer's internal expected-reply timestamp (set to
  `now + 320 s` after each RTT update).  Fixed by
  `LinkTimeReplyInterval` (default 320 s).
- PCFlexnet's `'+'` handler at `0x1000641B` accepts `'3+'` only when
  `[ebx+0xF]` (token state) is 0.  Every other state triggers the
  reject path at `0x100065A0` → L2 DM.  Fixed by never echoing `'3+'`
  and by sending compact-record-only refreshes.
- Token state can only be 0 immediately after handshake or after
  specific transitions we cannot observe remotely — so we never
  proactively initiate token exchanges with pcf; we only reply.
- xnet's ARM implementation is more permissive and tolerates our
  older behaviour; the fixes above don't regress xnet's peering.

**v0.7.1 addressed all three open items above — see its section below.**

**Original scope (all delivered):**

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

### v0.7.1 — M6 fine-tuning, dual-peer production-ready (2026-04-20, stable)

**Shipped.**  Addresses the three open items flagged at end of v0.7.0
plus a timing refinement found during the v0.7.1 validation session.

**Fixes delivered:**

- **Priority 1 — via-field tag on compact records** (`e4f42d8`)
  `ce_parse_compact_records()` gains a `port_idx` parameter; each
  parsed entry is tagged with the port it arrived on.  `output.c`
  resolves `Via` via `g_cfg.ports[port].neighbor` instead of the
  legacy `g_cfg.neighbor`.

- **Priority 2 — per-port `RouteAdvertInterval`** (`efeaef5`)
  `PortCfg` gains its own `route_advert_interval` field.  Config
  parser accepts an optional 4th field on `Port` lines
  (`route_advert=<seconds>`).  Global default changed to **0**
  (disabled) — required for PCFlexnet compatibility.  (X)Net users
  set `route_advert=60` on their port.  The pcf DM chain was traced
  to PCFlexnet's compact-record handler tearing down L2 when its
  token state is 0.

- **Priority 3 — solved indirectly by Priority 2.**  The "pcf stops
  advertising after reconnect" symptom was a consequence of the DM
  cycle, not a separate bug.  With DMs eliminated, pcf's one-shot
  `'3+'` + route-send at session init is sufficient for the lifetime
  of the (now indefinite) session.

- **Priority 4 / M6.5 — per-port destinations + flock merge** (`2371865`)
  Same pattern M6.6 applied to linkstats.  Each CE child writes
  `destinations.<port>` with its own dtable; `destinations_merge()`
  runs under flock, picks best-RTT-per-key across peers, writes
  unified `destinations` with a mix of `Via IW2OHX-12` and
  `Via IW2OHX-14` as appropriate.

- **Timing — non-drifting link-time scheduler** (`2f44e8c`)
  Switched the `LinkTimeReplyInterval` gate from "elapsed-since-last"
  to an absolute wall-clock schedule (`next_lt_tx = last + interval`).
  Drift from the 20 s proactive timer no longer accumulates; sends
  now land at exactly N × 320 s, giving clean 1-tick samples in
  pcf's RTT history instead of mixed 100/200 ticks.

- **Gateways file — multi-port** (`7e5a57b`)
  `output_write_gateways()` was still using the legacy single-port
  fields and wrote only `ports[0]`.  Now iterates all configured
  ports and emits one gateway line per port.  URONode can now
  resolve routes advertised as `Via IW2OHX-12` (pcf) to the correct
  outbound kernel interface.

**Configuration impact (back-compat):**

- Old configs without any `Port` lines continue to work unchanged
  (legacy flat keywords synthesise `ports[0]`).
- Old configs WITH `Port` lines get the new default `route_advert=0`
  globally — xnet users should add `route_advert=60` to their xnet
  `Port` line to keep pre-v0.7.1 behaviour.
- New files appear in `/usr/local/var/lib/ax25/flex/`:
  `destinations.<port>`, `destinations.lock` (managed by flexnetd;
  no manual setup).

**Verified against production peers:**
- **Pcf (IW2OHX-12)** — session stable indefinitely, route exchange
  completes, ~100-120 routes via pcf in the unified output.
- **Xnet (IW2OHX-14)** — route exchange unchanged, `dst=1` stays
  stable via the periodic re-advertisement.

**Known limitation — pcf-side displayed Q/T.**  PCFlexnet uses
**multi-state `ts_ahead`** values (320 s in state 5, 180 s in
states 2/3) so no single fixed interval on our side can produce
clean 1-tick samples across all pcf states.  The
current behaviour is ~75% state-5 samples (clean 1s and 100s) and
~25% state-2/3 samples (4095 saturation due to delta overflow).
**Impact is purely cosmetic on pcf's display** — all functional
aspects (route exchange, stability, outbound routing) work correctly.
An adaptive-interval scheduler is planned for v0.7.2.

### v0.7.1.1 — link-time value 2→0 experiment (REVERTED in v0.7.1.2)

Patch attempt based on the (X)Net ↔ pcf L2 capture (30 min monitor
traffic on pcf's radio port).  The capture showed (X)Net sending its
link-time with value "0" (`31 30 0D`) whereas we were sending "2"
(`31 32 0D`).  Rationale: PCFlexnet's smoothed-RTT formula is
`(peer_val + avg + 1) / 2`, so `peer_val=0` should converge to 1 and
`peer_val=2` should stall at 2.

**Deployed 2026-04-20, immediately observed regression:**

- pcf's displayed Q/T went UP (to ~966), not down
- pcf's history buffer refilled with 4095, 600, 200, 100 samples
- Xnet's view confirmed the byte change hit the wire (`rtt 600/0`)
- **Links went down multiple times** — actual instability, not just
  cosmetic drift

Hypothesis for why value=0 failed at our cadence: the xnet capture
showed xnet sending value=0 **ONCE per session** (at t=5.32s after
init), then silent.  We sent value=0 every 320s via the proactive
timer.  Pcf's state machine may take a "bad peer" branch when
peer_val=0 arrives repeatedly, triggering L2 DM.

Reverted in v0.7.1.2.

### v0.7.1.2 — revert v0.7.1.1 (2026-04-20, stable)

Reverts the two poll_cycle.c call sites back to
`ce_build_link_time(..., 2)`.  Restores v0.7.1 behaviour.  Full
analysis preserved in the v0.7.1.1 section above and v0.7.2 TODO.

### v0.7.2 — Protocol alignment pass (2026-04-21, stable)

Wire-format corrections so the daemon emits frames that exactly match
the specification in `PROTOCOL_SPEC.md`.  All changes are in
`ce_proto.c` and the CE session loop in `poll_cycle.c`.

**CE type-2 keepalive — corrected**

*Before:* 241 bytes = `'2'` + 237 × `' '` + `'10\r'` (from an early
capture misread where two back-to-back frames were concatenated in
the monitor hex dump).

*After:* 241 bytes = `'2'` + 240 × `' '` — pure `'2'` + spaces, no
trailer.  RX now accepts any `'2'` + all-space payload regardless of
length, so partial/fragmented deliveries and PCFlexnet's 201-byte
variant (`'2'` + 200 spaces) both work.

**CE type-4 routing sequence number — correct format and wiring**

*Before:* `ce_build_token()` emitted `'4%d%c\r'` (decimal + flag char);
`ce_parse_frame()` echoed received type-4 frames back to the peer.

*After:* Wire format is `'4%u\r'` — no flag byte.  RX handler stores
the received value and does not reply.

Added TX: emit a type-4 frame whenever the reachable destination
count in our dtable changes between iterations — the "routing table
changed" hint.  Cheaper than a full `'3+'` cycle.

**CE type-1 link-time classification unified**

*Before:* `'10\r'` / `'11\r'` / `'12\r'` were classified as a
distinct `CE_FRAME_STATUS_10` and treated as no-ops.

*After:* They are ordinary type-1 link-time frames with decimal
values 0 / 1 / 2 respectively (wire format is `"1%ld\r"` for any
decimal).  RX handler accepts any `'1'` + decimal ≥ 3 bytes.  The
`CE_FRAME_STATUS_10` constant is retained for source-compat but no
frame is ever classified as it.

**Keepalive period**

`DEFAULT_KEEPALIVE_S` raised from 90 to 180 s to match the protocol
specification.  The proactive 20-s send in the CE session handler
(used to keep silent pcf peers alive) is unaffected.

**Deferred to v0.7.4+**

Three refinement items remain (link-time cadence is resolved in
v0.7.3 below):

1. **Proactive `'3+'` tokens with REJ tolerance** — re-enable
   periodic route re-advertisement to pcf with explicit
   non-disconnect REJ handling.
2. **Routing batches** — pack the dtable into 240-byte multi-record
   type-3 frames matching the reference cadence, so peer dtables
   refresh periodically without our issuing `'3+'`.
3. **State-6 investigation** — understand what triggers pcf's
   state-6 (`ts_ahead = 0`) so our repeated link-times stop
   saturating to 4095 in the states where pcf expects 180 s.

Tracking input: `flexnet_capture_port1.json`, `PROTOCOL_SPEC.md`,
and the reference implementation `../linbpq-flexnet/FlexNetCode.c`.

### v0.7.4 — destinations-file truncation fix (2026-04-21, stable)

Latent bug since v0.3.0.  `run_native_ce_session()` rate-limited the
`output_write_destinations()` call to once every 60 s, so when xnet
dumps its full ~120-entry dtable in a single 20–30 s burst at
session setup, only the first batch of 20 records reached disk.
The rest were merged into `g_table` in memory but the 60 s gate
blocked the flush, and since xnet goes silent after the initial
burst (routes are re-advertised only on change), the gate never
reopened.  Users saw 20 entries in `fld` for the entire session
lifetime.

Fix: remove the 60 s gate, flush after every compact-record batch.
One-line change, no config knobs.

### v0.7.3 — Per-port link-time cadence (2026-04-21, stable)

Fixes xnet's smoothed-RTT convergence.  Root cause analysis from the
2026-04-21 30-minute xnet-port capture:

- Xnet sends us a type-1 link-time frame every ~20 s (in response to
  our proactive keepalive).  Each arrival updates xnet's smoothed-RTT
  measurement loop.
- Our v0.7.0–v0.7.2 code replied to xnet's keepalive/link-time only
  once every 320 s, gated by the global `LinkTimeReplyInterval`.
- With 300+ s gaps between our type-1 frames, xnet's internal delta
  measurement exceeded the 60000-tick abort threshold, so its
  smoothed value never left the infinity sentinel (60000).
- On the wire this showed up as xnet sending us `"1600"` forever
  (= 60000 / 100 encoded), and its `L` display for our row showed
  `Q=301, RTT=600/2`.
- Reference implementation `linbpq-flexnet` (running on IW2OHX-13,
  converging cleanly to `Q=2, RTT=2/2`) replies on every received
  keepalive and every received link-time, plus once per 21 s timer —
  no rate limit.

The fix has two parts:

1. **Per-port `lt_reply_interval`** — `PortCfg` gains a new field; the
   config parser accepts `lt_reply=<sec>` on `Port` lines.  `-1`
   (default) means "inherit the global `LinkTimeReplyInterval`".
2. **Inline link-time reply restored** — the M6.9.2 patch (which
   removed the inline reply to stabilise pcf) is now gated by the
   per-port `lt_reply_interval`: xnet ports with `lt_reply=20` fire
   every cycle (linbpq-flexnet pattern); pcf ports with
   `lt_reply=320` still fire once per 320 s window.

Recommended `Port` configuration:

```
Port xnet IW2OHX-14 IW2OHX-3 route_advert=60 lt_reply=20
Port pcf  IW2OHX-12 IW2OHX-3 route_advert=0  lt_reply=320
```

Back-compat:
- Existing configs without `lt_reply=` inherit the global — identical
  to v0.7.2 behaviour.
- The `route_advert=` syntax is unchanged.

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
| v0.7.0 | M6 — Multi-neighbor + PCFlexnet interop | **Stable** |
| v0.7.1 | M6 fine-tuning — per-port route_advert, M6.5 destinations merge, non-drift timing | **Stable** |
| v0.7.1.1 | Link-time value 2→0 experiment | **Reverted** (caused link instability) |
| v0.7.1.2 | v0.7.1.1 reverted back to value=2 | **Stable** |
| v0.7.2 | Protocol alignment: keepalive format, type-4 seq, link-time unification | **Stable** |
| v0.7.3 | Per-port `lt_reply_interval` + inline reply restored (fixes xnet smoothed RTT) | **Stable** |
| v0.7.4 | Destinations-file truncation fix (drop 60 s flush rate limit, write every batch) | **Stable** |
| v0.8.0 | M2.1 — Inbound transit digipeating (requires M6) | Blocked on M6 |
| v1.0.0 | Production hardening + full docs | Planned |

---

## Protocol reference

- **CE/CF routing:** `PROTOCOL_SPEC.md` — full wire format reference
- **FlexNet L3 connections:** AX.25 digipeater chains (confirmed 2026-04-14)
- **Capture evidence:** `monitor_port1_raw.txt`, `monitor_port11_raw.txt`
