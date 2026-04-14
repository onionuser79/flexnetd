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

---

## What's remaining

### v0.5.0 — M2: Transit connections (identity preservation)

**Goal:** When a user connects THROUGH our node to a destination beyond
us, preserve their callsign end-to-end.

**Key discovery (2026-04-14):** Live multi-hop captures confirmed that
FlexNet L3 connections use **AX.25 digipeater chains**, NOT CREQ/CACK
framing. This dramatically simplifies M2.

Captured evidence (IW7BIA from IW2OHX-12 → IR5S via IR3UHU-2):
```
→ fm IW7BIA to IR5S via IW2OHX-12* IW2OHX-14* IR3UHU-2 ctl SABM+
← fm IR5S to IW7BIA via IR3UHU-2* IW2OHX-14 IW2OHX-12 ctl UA-
```

User identity (IW7BIA) preserved as L2 source. Intermediate nodes in
via list with `*` = digipeated. Standard SABM/UA/DISC. No special framing.

**What's needed:**

| Task | Description | Complexity |
|------|-------------|------------|
| **M2.1 Digipeater support** | When IW2OHX-3 appears in a via list, mark ourselves as digipeated and forward to next hop. Option A: kernel AX.25 digipeating. Option B: handle in flexnetd. | Medium |
| **M2.2 Outbound via-list** | When a local URONode user does `c ir5s`, build SABM with via list from routing table (`ax25_connect_via()`). | Medium |
| **M2.3 Path resolution** | Walk `DestEntry.via_callsign` to build full digipeater chain for any destination. | Low |

**First step:** Investigate Linux kernel digipeating support:
```bash
cat /proc/sys/net/ax25/ax1/digi
ax25_rt -l
```
If the kernel handles cross-port digipeating natively, M2.1 might be
zero code — just configuration.

### v1.0.0 — Production release

- Multi-neighbor support (xnet port + pcf port)
- M1.4 CE type-6/7 path query response (if needed)
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
| v0.5.0 | AX.25 digipeater support for transit connections | Planned |
| v1.0.0 | Multi-neighbor, production hardening | Planned |

---

## Protocol reference

- **CE/CF routing:** `PROTOCOL_SPEC.md` — full wire format from RE
- **FlexNet L3 connections:** AX.25 digipeater chains (confirmed 2026-04-14)
- **Capture evidence:** `monitor_port1_raw.txt`, `monitor_port11_raw.txt`
