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

### M1.1 — CE SSID encoding for SSID >= 10  `ce_proto.c` `dtable.c`

Current compact record builder and parser handle only decimal digits (SSID
0-9). The protocol encodes SSID N as ASCII character `0x30 + N`, giving
`:`, `;`, `<`, `=`, `>`, `?` for SSID 10-15. 86 frames with SSID >= 10
were confirmed in live captures.

**Fix:** Replace decimal-only SSID handling with `ssid = c - 0x30`
for any character in range `'0'..'?'` (0x30..0x3F).
Apply to both `ce_build_record()` and `ce_parse_compact_records()`.

Disambiguation: `'?'` (SSID 15) and `'?'` as indirect RTT prefix are
distinct -- SSID follows the callsign, prefix follows the SSID character.

### M1.2 — CE type-0 init frame byte 0  `poll_cycle.c`

Current code sends `0x3E` (CE_LINK_SETUP_BYTE) as byte 0 of the init
handshake. The confirmed protocol requires byte 0 to always be `0x30`.
Byte 1 carries the SSID range: `0x30 + max_ssid`.

Sending `0x3E` as byte 0 causes strict peers to misclassify the frame
as a CE type-6 destination query, breaking the handshake.

**Fix:** Change CE_LINK_SETUP_BYTE from `0x3E` to `0x30` in the init
frame builder. Correct format: `0x30, 0x30+max_ssid, 0x25, 0x21, 0x0D`.

### M1.3 — L3RTT val1/val2 fields  `cf_proto.c`

Confirmed semantics from Phase 3 disruption capture:
- `val1 = 0`, `val2 = 0` when `$M = 60000` (no L3RTT reply, link down)
- Both non-zero during normal operation

These fields are used by the peer for link quality assessment. Incorrect
values may impair Q/T convergence.

**Fix:** Set val1 and val2 to 0 when link is down; to a non-zero value
(e.g. active neighbor count or fixed `3`) when active. Update `cf_build_l3rtt()`.

### M1.4 — CE type-6/7 path query — respond to incoming D queries  `poll_cycle.c`

When a peer issues `D CALLSIGN` for a multi-hop destination via our node,
it sends a CE type-6 query. Currently silently dropped. We must reply with
a CE type-7 frame appending our callsign to the path.

**Fix:** Add a `buf[0] == '6'` handler in `run_native_ce_session()`:
parse the type-6 frame, look up the destination in `g_table`, and if
found, build a type-7 reply: `7$ <seq> <originator> <our_call> [<hops>] <dest>`.

---

## Milestone M2 — Identity preservation  `v0.5.0`

FlexNet preserves the originating user's callsign end-to-end via AX.25
digipeater semantics -- the user's callsign is the L2 source throughout,
with intermediate nodes appearing in the `via` field marked `*`.

This is architecturally distinct from NET/ROM, which rewrites the L2
source at each hop. Implementing it correctly is the core user-facing
feature of a FlexNet node.

### M2.1 — CREQ forwarding with originator  `flexnetd.c`

When a user connects to a FlexNet destination via flexnetd, build and
send a CREQ frame:

```
L3 fm <mycall> to <dest_node> LT <lt> CREQ IN=<in> ID=<id> Window=4 <user_callsign> <mycall>
```

Fields confirmed from live capture: Window=4 always, ID constant for
the session lifetime, originator is the user callsign, forwarding is
the local node callsign.

### M2.2 — AX.25 digipeater via-field construction  `ax25sock.c`

The SABM frame for a user session must carry the full via list. For a
2-hop path `user -> flexnetd -> IR3UHU-2 -> IR5S`, the SABM is:

```
fm user to IR5S via flexnetd* IR3UHU-2 ctl SABM+
```

The `via_callsign` field already exists in `DestEntry` (populated by
CE type-7 replies). Use it to construct the via list.

### M2.3 — Session state machine  `poll_cycle.c`

```
IDLE -> CONNECTING (CREQ sent) -> CONNECTED (CACK received) -> DISCONNECTING (DREQ) -> IDLE
```

Handle inbound CACK (from the final destination node, not the intermediate
neighbor), and DREQ/DACK at session teardown.

### M2.4 — Multi-hop path resolution  `dtable.c`

For destinations more than 1 hop away: issue a CE type-6 query to
discover the full path, build the AX.25 via list from the type-7 reply.

---

## Milestone M3 — Link health display  `v0.6.0`

URONode should display FlexNet link health in the same format as the
xnet `L` command, giving operators a familiar view of link quality.

### Target output format (matches xnet L table exactly)

```
Link to       dst  Q/T    rtt    tx connect   tx   rx   txq/rxq  rr+%  bit/s
11:IR3UHU-2     1 F   1   0/0     0  2h 29m   37K  75K   99/100   1.7    101
```

| Column | Source |
|--------|--------|
| `port:callsign` | port number from config + neighbor callsign |
| `Q/T` | link quality/timer from CE type-1 link-time convergence |
| `mode` | always `F` (FlexNet) |
| `rtt` | last measured RTT in 100ms ticks |
| `tx/rx queue` | current AX.25 tx/rx queue depth |
| `connect time` | session uptime |
| `tx/rx bytes` | cumulative bytes sent/received |
| `txq/rxq %` | tx/rx frame success rate |
| `rr+%` | retransmit rate |
| `bit/s` | recent throughput |

### M3.1 — LinkStats struct  `flexnetd.h`

Add a `LinkStats` struct tracking all L-table fields per active session.
Updated on every frame sent/received in `run_native_ce_session()`.

### M3.2 — Link stats output file  `output.c` `flexnetd.conf`

New periodic output (every 30s):
- File: `/usr/local/var/lib/ax25/flex/linkstats` (configurable via `LinkStatsFile`)
- Written atomically (temp + rename)
- URONode reads it for the L command display

---

## Milestone M4 — Searchable destination command  `v0.7.0 -> v1.0.0`

### M4.1 — Replace gateway field with VIA callsign  `output.c`

Current destinations file uses `00000` as gateway. Replace with the
actual next-hop callsign from `DestEntry.via_callsign`.

**Target output:**
```
Dest     SSID    RTT Gateway
-------- ----- ----- -------
IR5S     0-15      4 IW2OHX-14
```

### M4.2 — D command with pattern matching  `flexnetd.c`

Handle `D <pattern>` queries:
- Exact callsign: `D IR5S`
- Prefix wildcard: `D IW*`
- SSID-specific: `D IR5S-7`

Scan `g_table` for matches, return formatted result in xnet D style.

### M4.3 — D command output format  `output.c`

```
FlexNet Destination IR5S:
Dest     SSID    RTT Gateway
-------- ----- ----- -------
IR5S     0-15      4 IW2OHX-14
```

Wildcard example (`D IW*`):
```
FlexNet Destinations matching IW*:
Dest     SSID    RTT Gateway
-------- ----- ----- -------
IW2OHX   3-3      1 (local)
IW7BIA   0-15     4 IW2OHX-14
IW8PGT   0-9      6 IW2OHX-14
```

---

## Version summary

| Version | Milestone | Key deliverable |
|---------|-----------|-----------------|
| v0.3.0 | current | Basic CE/CF peering, route exchange, Q/T=1 |
| v0.4.0 | M1 | Full protocol correctness (SSID, init, L3RTT, type-6/7) |
| v0.5.0 | M2 | User session forwarding with identity preservation |
| v0.6.0 | M3 | Link health display in xnet L-table format |
| v0.7.0 | M4 partial | VIA field in destinations, D command |
| v1.0.0 | M4 complete | Full D wildcard, production-ready release |

---

## Protocol reference

All milestones are grounded in `PROTOCOL_SPEC.md` which documents the
complete FlexNet CE/CF wire protocol as reverse-engineered from live
captures between IW2OHX-14 (xnet) and IW2OHX-3 (flexnetd), April 2026.
All items in `PROTOCOL_SPEC.md` are resolved -- the spec is implementation-ready.
