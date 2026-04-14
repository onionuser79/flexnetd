# flexnetd — v1.0 Development Roadmap

**Current version:** v0.3.0  
**Target version:** v1.0.0  
**Author:** IW2OHX  
**Based on:** Full FlexNet CE/CF protocol reverse engineering (April 2026)

---

## Background

flexnetd v0.3.0 achieves basic FlexNet peering with xnet nodes: native CE/CF
session handling, route exchange, link quality convergence (Q/T=1), and
URONode destination file output. The v1.0 release focuses on protocol
correctness from the full RE findings, user session identity preservation,
operational visibility, and a usable destination query interface.

---

## Milestone M1 — Protocol completeness  `v0.4.0`

Correctness fixes derived from the full protocol reverse engineering.
These are foundations that everything else builds on.

### M1.1 — CE SSID encoding for SSID ≥ 10  `ce_proto.c` `dtable.c`

Current compact record builder and parser handle only decimal digits (SSID
0-9). The protocol encodes SSID N as ASCII character `0x30 + N`, giving
`:`, `;`, `<`, `=`, `>`, `?` for SSID 10-15. 86 frames with SSID ≥ 10
were confirmed in live captures.

**Fix:** Replace decimal-only SSID handling with `ssid = ord(c) - 0x30`
(C: `ssid = c - 0x30`) for any character in range `'0'..'?'` (0x30..0x3F).
Apply to both `ce_build_record()` and `ce_parse_compact_records()`.

Disambiguation: `'?'` (SSID 15) and `'?'` as indirect RTT prefix are
distinct — SSID follows the callsign, prefix follows the SSID character.

### M1.2 — CE type-0 init frame byte 0  `poll_cycle.c`

Current code sends `0x3E` (CE_LINK_SETUP_BYTE) as byte 0 of the init
handshake. The confirmed protocol requires byte 0 to always be `0x30`.
Byte 1 carries the SSID range: `0x30 + max_ssid`.

Sending `0x3E` as byte 0 causes strict peers to misclassify the frame
as a CE type-6 destination query, breaking the handshake.

**Fix:** Change `CE_LINK_SETUP_BYTE` from `0x3E` to `0x30` in the init
frame builder. Correct format: `0x30, 0x30+max_ssid, 0x25, 0x21, 0x0D`.

### M1.3 — L3RTT val1/val2 fields  `cf_proto.c`

Confirmed semantics from Phase 3 disruption capture:
- `val1 = 0`, `val2 = 0` when `$M = 60000` (no L3RTT reply, link down)
- Both non-zero during normal operation

These fields are used by the peer for link quality assessment. Incorrect
values (currently always zero or unset) may impair Q/T convergence.

**Fix:** Set `val1` and `val2` to 0 when `max_dest` is 0 or link is
down; to a non-zero value (e.g. active neighbor count or fixed `3`)
when the link is active. Update `cf_build_l3rtt()`.

### M1.4 — CE type-6/7 path query — respond to incoming D queries  `poll_cycle.c`

When a peer issues a `D CALLSIGN` for a multi-hop destination via our
node, it sends a CE type-6 query. Currently these frames are silently
dropped (logged as unrecognised). We must reply with a CE type-7 frame
appending our callsign to the path.

**Fix:** Add a `buf[0] == '6'` handler in `run_native_ce_session()`:
parse the type-6 frame, look up the destination in `g_table`, and if
found, build a type-7 reply: `7$ <seq> <originator> <our_call> [<hops>] <dest>`.

---

## Milestone M2 — Identity preservation (AX.25 digipeating)  `v0.5.0`

FlexNet preserves the originating user's callsign end-to-end via AX.25
digipeater semantics — the user's callsign is the L2 source throughout,
with intermediate nodes appearing in the `via` field marked `*`.

This is architecturally distinct from NET/ROM, which rewrites the L2
source at each hop.

### CONFIRMED: FlexNet L3 connections = AX.25 digipeater chains

**Live captures (2026-04-14)** of multi-hop connections from IW7BIA at
IW2OHX-12 (PCFlexnet) to IR5S and IR3UGM (behind IR3UHU-2) confirmed
that xnet implements FlexNet L3 routing as AX.25 digipeating:

```
→ fm IW7BIA to IR5S via IW2OHX-12* IW2OHX-14* IR3UHU-2 ctl SABM+
← fm IR5S to IW7BIA via IR3UHU-2* IW2OHX-14 IW2OHX-12 ctl UA-
← fm IR5S to IW7BIA via IR3UHU-2* IW2OHX-14 IW2OHX-12 ctl I00^ pid F0 [256]
  ... data exchange (PID=F0, standard text) ...
← fm IR5S to IW7BIA via IR3UHU-2* IW2OHX-14 IW2OHX-12 ctl DISC+
→ fm IW7BIA to IR5S via IW2OHX-12* IW2OHX-14* IR3UHU-2 ctl UA-
```

**No CREQ/CACK/DREQ frames were observed.** The original ROADMAP M2.1
(CREQ framing) and M2.3 (session state machine) are NOT needed.

Evidence captured on both sides:
- Port 1 (IW2OHX-14 ↔ IW2OHX-12): `monitor_port1_raw.txt`
- Port 11 (IW2OHX-14 ↔ IR3UHU-2): `monitor_port11_raw.txt`

### Revised M2.1 — AX.25 digipeater support  `flexnetd.c` `ax25sock.c`

When flexnetd's callsign (IW2OHX-3) appears in a via list, it must
act as an AX.25 digipeater:

1. Receive SABM with our callsign in the via list (unmarked)
2. Mark our callsign with `*` (digipeated flag)
3. Forward to the next callsign in the via list

Example: when IW2OHX-3 is in the path between IW2OHX-12 and IW2OHX-14:
```
→ fm IW7BIA to IR5S via IW2OHX-12* IW2OHX-3 IW2OHX-14 ctl SABM+
  (we receive this, mark ourselves, forward:)
→ fm IW7BIA to IR5S via IW2OHX-12* IW2OHX-3* IW2OHX-14 ctl SABM+
```

**Option A:** Enable kernel AX.25 digipeating (`ax25_rt` or `digi`).
The Linux kernel may already support this natively — investigate
`/proc/sys/net/ax25/*/digi` and `ax25_rt -a` for cross-port digipeating.

**Option B:** Handle in flexnetd by intercepting frames with our
callsign in the via list, forwarding to the appropriate port.

### Revised M2.2 — Via-list construction for outbound connects  `ax25sock.c`

When a local user (connected to URONode) wants to connect to a FlexNet
destination, build the SABM with the full digipeater via list from the
routing table:

```c
/* Extend ax25_connect() to accept via list */
int ax25_connect_via(const char *mycall, const char *dest,
                     const char *port_name,
                     const char **via_list, int via_count);
```

The via list is built from `DestEntry.via_callsign` (next-hop) in the
routing table. For multi-hop paths, chain the next-hops.

### M2.3 — Path resolution from routing table  `dtable.c`

Build the full via list for a destination by walking the routing table:
1. Look up destination → get `via_callsign` (next-hop neighbor)
2. The path is: `our_callsign, neighbor_callsign` for 1-hop
3. For multi-hop: recursively resolve each next-hop

All routes currently arrive through IW2OHX-14, so the via list is
always `[IW2OHX-3, IW2OHX-14]` for xnet-side destinations. Multi-
neighbor support would add `[IW2OHX-3, IW2OHX-12]` for pcf-side.

---

## Milestone M3 — Link health display  `v0.4.1` ✅ IMPLEMENTED

URONode displays FlexNet link health in xnet `L` command format.
Implemented in v0.4.1: LinkStats struct, periodic file output,
Q/T=1 (direct link), RTT from link-time round-trip measurement.

### Target output format (matches xnet L table exactly)

```
Link to       dst  Q/T    rtt    tx connect   tx   rx   txq/rxq  rr+%  bit/s
11:IR3UHU-2     1 F   1   0/0     0  2h 29m   37K  75K   99/100   1.7    101
```

**Column definitions:**
| Column | Source |
|--------|--------|
| `port:callsign` | port number from config + neighbor callsign |
| `Q/T` | link quality/timer (from CE type-1 link-time convergence) |
| `mode` | always `F` (FlexNet) |
| `rtt` | last measured RTT in 100ms ticks |
| `tx/rx queue` | current AX.25 tx/rx queue depth |
| `connect time` | session uptime (from session start) |
| `tx/rx bytes` | cumulative bytes sent/received |
| `txq/rxq %` | tx/rx frame success rate |
| `rr+%` | retransmit rate |
| `bit/s` | recent throughput |

### M3.1 — LinkStats struct  `flexnetd.h`

Add a `LinkStats` struct tracking all L-table fields per active session.
Updated on every frame sent and received in `run_native_ce_session()`.

### M3.2 — Link stats output file  `output.c` `flexnetd.conf`

New periodic output (every 30s):
- File: `/usr/local/var/lib/ax25/flex/linkstats` (configurable via `LinkStatsFile`)
- Written atomically (temp + rename)
- URONode reads it for the `L` command display

---

## Milestone M4 — Searchable destination command  `v0.7.0 → v1.0.0`

### M4.1 — Replace gateway field with VIA callsign  `output.c`

Current destinations file uses `00000` (numeric zero) as the gateway
field. Replace with the actual next-hop callsign from `DestEntry.via_callsign`.

**Current output:**
```
callsign  ssid     rtt  gateway
IR5S      0-15       4    00000
```

**Target output:**
```
Dest     SSID    RTT Gateway
-------- ----- ----- -------
IR5S     0-15      4 IW2OHX-14
```

### M4.2/M4.3 — D command with pattern matching  `flexdest.c` ✅ IMPLEMENTED

Standalone `flexdest` tool reads the destinations file and supports:
- Exact callsign: `flexdest IR5S`
- Prefix wildcard: `flexdest IW*`
- SSID-specific: `flexdest IR5S-7`
- All destinations: `flexdest` (no args)

Output matches xnet D command style:

```
FlexNet Destinations matching IW*:
Dest     SSID    RTT Via
IW2OHX   3-3      1 (local)
IW7BIA   0-15     4 IW2OHX-14
IW8PGT   0-9      6 IW2OHX-14
```

---

## Version summary

| Version | Milestone | Status | Key deliverable |
|---------|-----------|--------|-----------------|
| v0.3.0  | baseline  | ✅ | Basic CE/CF peering, route exchange, Q/T=1 |
| v0.4.0  | M1        | ✅ | Protocol correctness (SSID, init, L3RTT) + debug logging |
| v0.4.1  | M3+M4.1   | ✅ | Link health display, VIA field, flexdest tool |
| v0.5.0  | M2        | **planned** | AX.25 digipeater support for transit connections |
| v1.0.0  | release   | **planned** | Production-ready with multi-neighbor support |

---

## Protocol reference

**CE/CF routing protocol:** documented in `PROTOCOL_SPEC.md`, reverse-
engineered from live captures between IW2OHX-14 (xnet) and IW2OHX-3
(flexnetd), April 2026.

**FlexNet L3 connection protocol:** confirmed via live multi-hop captures
(2026-04-14) as AX.25 digipeater chains. See `monitor_port1_raw.txt` and
`monitor_port11_raw.txt` in the investigation directory. No CREQ/CACK
framing — standard SABM/UA/DISC with via lists.
