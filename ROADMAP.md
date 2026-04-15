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

## What's remaining

### v0.6.0 — M2.1: Inbound transit (digipeater support)

**Goal:** When a remote user connects THROUGH our node to a destination
beyond us, forward the connection (act as AX.25 digipeater).

**Key discovery (2026-04-14):** FlexNet L3 connections use **AX.25
digipeater chains**, NOT CREQ/CACK framing.

Captured evidence (IW7BIA from IW2OHX-12 → IR5S via IR3UHU-2):
```
→ fm IW7BIA to IR5S via IW2OHX-12* IW2OHX-14* IR3UHU-2 ctl SABM+
← fm IR5S to IW7BIA via IR3UHU-2* IW2OHX-14 IW2OHX-12 ctl UA-
```

**What's needed:**

| Task | Description | Complexity |
|------|-------------|------------|
| **M2.1 Digipeater support** | When IW2OHX-3 appears in an inbound via list, mark ourselves as digipeated and forward to next hop. Option A: kernel AX.25 digipeating (`/proc/sys/net/ax25/ax1/digi`). Option B: handle in flexnetd. | Medium |

**First step:** Test kernel digipeating:
```bash
echo 1 > /proc/sys/net/ax25/ax1/digi
# Then test inbound transit from xnet through us
```

### v0.7.0 — M5: Route path display

**Goal:** Show the hop-by-hop route path for FlexNet destinations, like
xnet's route trace output:
```
d iw2ohx-3     (on IR3UHF)
*** route: IR3UHF IZ3LSV-14 IR3UHU-2 IW2OHX-14 IW2OHX-3
```

**What we have today:** Each `DestEntry` carries `via_callsign` — the
next hop from xnet's perspective. This gives us a partial path:
`IW2OHX-3 → IW2OHX-14 → [via] → destination`.

**What's needed:**

| Task | Description | Complexity |
|------|-------------|------------|
| **M5.1 Partial route** | Show known path in `flexdest` and URONode D output: `route: IW2OHX-3 IW2OHX-14 [via_callsign] DEST` | Low |
| **M5.2 Path chaining** | Recursively resolve via_callsign through the destinations table to extend the path where possible | Low |
| **M5.3 CE type-6/7 path query** | Parse xnet's type-6/7 CE frames for full path info (if xnet sends path vectors in routing updates) | Medium |

### v1.0.0 — Production release

- Multi-neighbor support (xnet port + pcf port)
- Full route path display (M5.3 if needed)
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
| v0.6.0 | Inbound transit digipeating | Planned |
| v0.7.0 | Route path display (like xnet's `*** route:` output) | Planned |
| v1.0.0 | Multi-neighbor, production hardening | Planned |

---

## Protocol reference

- **CE/CF routing:** `PROTOCOL_SPEC.md` — full wire format from RE
- **FlexNet L3 connections:** AX.25 digipeater chains (confirmed 2026-04-14)
- **Capture evidence:** `monitor_port1_raw.txt`, `monitor_port11_raw.txt`
