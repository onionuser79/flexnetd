# FlexNet / (X)Net Protocol Specification

**Applies to:** (X)NET V1.39 and PC/Flexnet 3.3g (tested peers; earlier
and later releases of both families appear compatible on the subset
documented here).
**Sources:** Live protocol captures from IW2OHX-14 ↔ IR3UHU-2,
IW2OHX-14 ↔ IW2OHX-13, IW2OHX-14 ↔ IW2OHX-12; DCC 1995 paper
(DK7WJ / N2IRZ); xnet138 sysop manual; RMNC_FlexNet_and_PC_FlexNet
English excerpt.
**Revision:** 2026-05-15 (added findings from the linbpq-flexnet v2.0
GA investigation cycle: INIT `max_ssid` clamping, SSID-range
advertisement mechanics, PID=CF dispatcher pattern, AX.25 V2
digi-chain reciprocity).

---

## 1. Framing

FlexNet runs over AX.25 connected-mode sessions between neighbor nodes.
Each AX.25 I-frame carries a PID byte identifying the payload type:

| PID  | Value | Layer                                  |
|------|-------|----------------------------------------|
| CE   | 0xCE  | Native FlexNet (routing, keepalive, path query) |
| CF   | 0xCF  | NET/ROM-compatible layer (L3RTT, D-table, CREQ/CACK) |
| F0   | 0xF0  | Plain AX.25 UI (beacons, user text)    |

The two protocol layers operate in parallel on the same AX.25 session.

---

## 2. PID = CE — Native FlexNet Protocol

### 2.1 Frame dispatch

The first byte of every CE payload is an ASCII type character in the
range `'0'` … `'7'`.  The receiver dispatches to one of eight handlers
indexed by `(byte[0] − 0x30)`.  Type bytes outside this range are
logged and dropped.

| Type | Char | Purpose                                     |
|------|------|---------------------------------------------|
| 0    | `0`  | Link setup / init handshake                  |
| 1    | `1`  | Link-time measurement (RTT probe reply)      |
| 2    | `2`  | Keepalive / RTT probe (null frame)           |
| 3    | `3`  | Routing tokens (`3+`, `3-`) and compact route records |
| 4    | `4`  | Routing-table sequence number (gossip)       |
| 5    | `5`  | Reserved (unused; receivers drop)            |
| 6    | `6`  | Path / Traceroute REQUEST                    |
| 7    | `7`  | Path / Traceroute REPLY                      |

### 2.2 SSID encoding

All CE frames encode a callsign SSID (0 … 15) as a single ASCII
character `0x30 + N`:

| N  | Char | Hex  |
|----|------|------|
| 0–9  | `'0'`–`'9'` | 0x30–0x39 |
| 10 | `:`  | 0x3A |
| 11 | `;`  | 0x3B |
| 12 | `<`  | 0x3C |
| 13 | `=`  | 0x3D |
| 14 | `>`  | 0x3E |
| 15 | `?`  | 0x3F |

Parser: `ssid = ord(c) - 0x30` for any `c` in `0x30 … 0x3F`.

Disambiguation — `?` (0x3F) has two meanings in compact records:
- Immediately after a callsign: SSID = 15.
- Immediately before RTT digits: indirect-measurement prefix.

Context determines which — the SSID character always follows the
callsign, the indirect prefix always precedes RTT digits.

### 2.3 Type 0 — Link setup / init handshake

Fixed-length 5-byte frame:

```
0x30   (0x30 + max_ssid)   0x25   0x21   0x0D
```

Byte 0 is always `0x30` (the literal digit `'0'`), never
`0x30 + min_ssid` — otherwise the receiver would misclassify the
frame as a type-3.  Byte 1 advertises the highest SSID the sender
accepts.  Bytes 2–3 are capability flags (`%!`).

Exchanged immediately after the AX.25 session comes up.

**Observed clamping behaviour:** (X)Net uses byte 1 of the init
frame to bound the SSID range it will accept in subsequent compact
records from this peer.  If the sender declares `max_ssid = 5` in
init but later advertises a record with `SSID_HI = 8`, the receiver
clamps the stored entry's `SSID_HI` to 5 (and discards the
upper portion of the range).

A node that wants to advertise a contiguous SSID range MUST set
byte 1 of its init handshake to cover the upper edge of that range,
**not** the node's own SSID.  See §2.6.1.

### 2.4 Type 1 — Link-time measurement

```
'1' <decimal_value> '\r'
```

Wire value is in **seconds**.  The sender computes the value by
dividing its internal smoothed round-trip estimate (held in 10-ms
ticks) by 100.  First frame of a new link carries the sentinel
`1600\r` (600 s = 60-second placeholder / infinity) and converges
down as real round-trip measurements accumulate.

A type-1 frame is emitted in response to every incoming keepalive.
Short frames such as `"10\r"`, `"11\r"`, `"12\r"` are legal values
(0 / 1 / 2 seconds) and are standalone type-1 frames.  They are not
fragments of any other frame kind.

RX processing:
1. Measure elapsed time since the previous peer frame — record as the
   current local RTT delta.
2. Parse the decimal value × 100 → the peer's reported RTT estimate.
3. Update the local smoothed RTT with an IIR filter:
   `smoothed = (15 × smoothed + new_delta + 8) / 16` (alpha = 1/16).

### 2.5 Type 2 — Keepalive

```
'2' + N × ' '     (no trailer)
```

- **xnet** emits 241-byte frames: `'2'` + 240 spaces.
- **PCFlexnet** emits 201-byte frames: `'2'` + 200 spaces.

Both forms are pure `'2'` followed by whitespace with no trailing
bytes.  A receiver should accept any length ≥ 2 whose body after
the leading `'2'` consists entirely of 0x20 bytes.

**Period:** emitted every 180 seconds per link while the session is
up.  Receipt of a keepalive triggers a type-1 reply carrying the
current smoothed link-time value.

### 2.6 Type 3 — Routing tokens and compact records

Three forms share the `'3'` prefix:

**Token — request routes**
```
'3' '+' '\r'
```
Sent to ask the peer to advertise its routes.

**Token — end of batch**
```
'3' '-' '\r'
```
Sent by the transmitter when it has finished emitting its route set.

**Compact record frame**
```
'3' + [ CALLSIGN(6) + SSID_LO + SSID_HI + RTT_decimal + ' ' ]+ + '\r'
```

Each record packs:
- 6 characters of callsign (space-padded right)
- SSID_LO: single char `0x30 + lower_ssid`
- SSID_HI: single char `0x30 + upper_ssid`
- RTT: 1–5 decimal digits in units of 100 ms
- Single space separator

An optional `'?'` appearing immediately before the RTT digits marks
the record as an indirect (second-hand) measurement.

A trailing `'-'` just before the final `'\r'` marks the whole frame
as a route withdrawal — every record in it is advertised with
RTT = 60000 (infinity).

**Session state per peer**

| State | Meaning                                                    |
|-------|------------------------------------------------------------|
| 1     | Idle / ready                                               |
| 2     | Received a `3+` from peer; advertising our routes back     |
| 3     | Received a `3-` from peer; peer finished sending to us     |
| 4     | Sent a `3+` of our own; awaiting peer's records + `3-`     |

Transitions:
- RX `3+` → state 2, emit our routes + `3-`, return to state 1.
- RX `3-` → state 3, then return to idle.
- TX `3+` → state 4, timestamp the send.
- Timeout: if state 4 persists more than **360 seconds** without a
  matching `3-`, reset to idle.

Route exchanges happen at cycle boundaries (~240 s between full
cycles), not on every individual RTT change.

### 2.6.1 SSID-range advertisement

A single compact record encodes a contiguous SSID range when its
`SSID_LO` and `SSID_HI` bytes differ.  (X)Net and PC/Flexnet both
store the range as a single destination-table entry and display
it in `D` output as `CALLSIGN SSID_LO-SSID_HI`.

Wire example — node `IR2UFV` advertising the range 0–8 with RTT = 1:
```
'3' 'I' 'R' '2' 'U' 'F' 'V' '0' '8' '1' ' ' '\r'
33 49 52 32 55 46 56 30 38 31 20 0D
```

Decode:
- frame-type marker `'3'`
- 6-byte callsign `IR2UFV` (already exactly 6 chars, no space pad)
- `SSID_LO` = `'0'` (0)
- `SSID_HI` = `'8'` (8)
- RTT digits = `"1"` (one 100-ms tick)
- single space terminator, then `'\r'`

Receivers parsing this record store one D-table entry covering all
SSIDs 0..8 on the callsign.  A subsequent `C IR2UFV-5` from any
peer will be routed toward the advertising node.

**Single-SSID is a degenerate range** (`SSID_LO == SSID_HI`).  The
wire encoding is identical for a range and a single-SSID record;
receivers don't need to switch parsing paths.

**Init-handshake dependency** (§2.3).  An originating node that
wants its `SSID_HI = 8` advert honoured by xnet must send
`max_ssid = 8` in its init handshake.  If it sends `max_ssid = 0`
(typical when blindly using the node's own SSID), xnet stores
only `SSID_LO..max_ssid_seen` and silently drops the rest of the
range.

**Inbound dispatch is application-layer.**  Advertising the range
only makes the range *reachable* on the FlexNet cloud; what happens
when a peer issues `C CALLSIGN-N` for `N` inside the range depends
on whether the receiving node has an application bound to that
SSID (BBS, chat, mail gateway, etc.) or treats it as the node
command parser.  See §3.3 / §5.

### 2.7 Type 4 — Routing-table sequence number

```
'4' <decimal_seq> '\r'
```

The sender maintains a 16-bit local sequence counter that increments
on every mutation of its routing table.  The per-port tick compares
the global sequence against the last value advertised to that peer
and, on mismatch, emits a type-4 frame.

Receivers parse the decimal value and store it as the peer's
current sequence number.  **No reply, no echo.**  When a peer later
observes that its stored sequence has advanced past the seq it last
requested routes for, it knows to issue a `3+` and pull an updated
record set.

Type-4 is a lightweight "routes changed" hint; it is cheaper than
a full `3+` cycle and is intended for frequent use.

### 2.8 Type 5 — Reserved

Type 5 is reserved in the `'0'..'7'` range but has never been
observed on the wire and no known implementation emits or handles
it.  Receivers log and drop.  Do not emit.

### 2.9 Type 6 — Path / Traceroute REQUEST

```
'6' FLAG_BYTE  %4d_SEQ  DEST_CS  ' '  NEXT_HOP_CS  '\r'
```

- **FLAG_BYTE** is either `0x20` (space) for a fresh request from
  the originating node, or `0x60` (backtick) for a request that has
  already been forwarded by at least one intermediate node.  This
  is also the hop counter: each forwarder increments the byte
  before re-emitting, and frames with the byte past `0x50` (80 hops)
  are dropped as loop-prevention.
- **SEQ** is a 4-digit decimal correlator (right-aligned in 4
  bytes, space-padded for small values).
- **DEST_CS** and **NEXT_HOP_CS** are the callsigns being queried
  and the selected next-hop toward the destination.

Type-6 is only emitted for multi-hop paths — direct neighbors are
resolved by a local destination-table lookup.

### 2.10 Type 7 — Path / Traceroute REPLY

```
'7' HOP_BYTE  %4d_SEQ  CS_1  ' '  CS_2  ' '  ...  CS_N  '\r'
```

- **HOP_BYTE** is the hop counter; each forwarder increments before
  re-emitting.  Capped at 80.
- **SEQ** echoes the request's sequence number.
- The callsign list is the accumulated path.  The first callsign is
  the addressee.  Each intermediate node, as it forwards the reply,
  prepends its own callsign.

Receiver behaviour:
- If the first callsign matches our own, consume the reply
  (deliver it to whichever subsystem issued the request).
- Otherwise, look up the first callsign in the destination table
  and forward the reply toward it.

Example reply for a route IW2OHX-3 → IR3UHU-2 → IQ5KG-7 → IR5S:
```
7$    1IW2OHX-14 IR3UHU-2 IQ5KG-7 IR5S
```

Length grows with each hop added; a typical short reply is about
38 bytes plus 8 bytes per accumulated callsign.

---

## 3. PID = CF — NET/ROM-Compatible Layer

### 3.0 Dispatcher pattern

PID = CF on a FlexNet-flagged AX.25 link carries multiple payload
families: L3RTT probes (FlexNet's own), NetROM L3 NODES broadcasts,
and the full NetROM L4 message set (CREQ, CACK, INFO, IACK, DREQ,
DACK).  A receiver MUST dispatch by **payload content**, not by
PID alone.

Recommended pattern:

1. Locate the literal byte string `"L3RTT:"` anywhere in the
   payload.  (X)Net wraps L3RTT probes inside a NetROM L3 envelope,
   so the marker sits at offset ~15, not at offset 0.  A binary
   substring search over the whole payload is safe — the downstream
   parser already uses search-style scanning.
2. If `"L3RTT:"` is present → dispatch to the L3RTT handler (§3.1).
3. Otherwise → hand the buffer to the host NetROM L3/L4 dispatcher,
   which will deliver CACK/INFO/IACK to the originating user
   session, NODES broadcasts to the routes table, etc.

A naïve dispatcher that swallows every PID=CF frame as L3RTT will
silently drop user CACK/INFO and the originating user session
will time out even though the L3 handshake completed.  This is the
single most common integration bug for hosts adding FlexNet on top
of an existing NetROM stack.

### 3.1 L3RTT — Round-trip-time probe

Payload:
```
L3RTT: <counter> <val1> <val2> <node_serial>  <NODE_ALIAS>  LEVEL3_V2.1  (X)NET<ver>  $M<rtt>  [$N]
```

Format string: `L3RTT:%11lu%11lu%11lu%11lu %-6.6s LEVEL3_V2.1 (X)NET%d $M%u $N\r`

| Field        | Meaning                                                     |
|--------------|-------------------------------------------------------------|
| counter      | 10-digit monotonic millisecond tick counter; used as RTT echo token. |
| val1         | 0 when no reply (`$M=60000`); non-zero when link active. Typical values 0, 3, 4, 5. |
| val2         | Same semantics as val1. **val1 = 0 AND val2 = 0 = reliable link-down indicator.** Never simultaneously zero during normal operation. |
| node_serial  | Fixed per-node identifier. E.g. NETUHU (IR3UHU-2) = 3076478712; BOLNET (IW2OHX-14) = 67717840. |
| NODE_ALIAS   | 6-character alias of the node sending the probe.            |
| $M\<rtt\>    | RTT in 100-ms units. `$M=60000` = no reply.                 |
| $N           | End-of-record sentinel.                                     |

**LT field:** `LT=2` = initiator; `LT=1` = responder.  Approximately
4% of frames show exceptions at link-recovery boundaries.

### 3.2 D_TABLE — Destination-table exchange

Monitor display format:
```
CALLSIGN-SSID   RTT/SSID_MAX  [PORT[TYPE] 'ALIAS']
```

`D` command display:
```
CALLSIGN  SSID_LO-SSID_HI  RTT   (4 entries per line)
```

- **RTT = 60000** signals poison reverse (unreachable).
- **PORT**: 0 = RF, 1 = HAMNET, 16–23 = AXIP / AXUDP.
- **TYPE**: 8 = (X)Net / DAMA, 7 = FlexNet, 6 = BPQ, 5 = JNOS,
  4 = legacy.

On link drop, the node immediately advertises an updated D_TABLE
with RTT = 60000 for every affected destination (t = 0.0 s).
D_TABLE exchange over NET/ROM continues even when the CE native
session is down.

### 3.3 CREQ / CACK — Connection request and acknowledgement

```
CREQ: L3 fm <node> to <dest>  LT <lt>  CREQ  IN=<in>  ID=<id>  Window=4  <originator>  <forwarding_node>
CACK: L3 fm <node> to <node>  LT <lt>  CACK  IN=<in>  ID=<id>  Window=4
```

- **originator**: the ORIGINATING user's callsign.  Identity is
  preserved at L3 — the intermediate forwarding node does not rewrite
  the origin.
- **forwarding_node**: the local node forwarding the CREQ.
- **Window**: always 4.
- **ID**: session correlator; constant for the lifetime of the session.

Multi-hop semantics: a CREQ propagates forward through every hop.
The CACK comes from the final destination node directly, not from
an intermediate neighbor.

LT values observed in CREQ/CACK: 11, 14, 17.  These are packed /
compound values; the exact bit layout is TBD but does not affect
basic operation.

### 3.4 DREQ / DACK — Disconnect request and acknowledgement

Same structure as CREQ/CACK, no payload callsign required.

### 3.5 I / IACK — User data frames in transit

```
I:    L3 fm <src> to <dst>  LT <lt>  I     IN=<in>  ID=<id>  S(<seq>) R(<ack>) [<len>]
IACK: L3 fm <src> to <dst>  LT <lt>  IACK  IN=<in>  ID=<id>  S(<seq>) R(<ack>)
```

---

## 4. PID = F0 — Plain AX.25 UI Beacon

Destination callsign `QST`, approximately every 120 seconds.
Example payload:
```
"IW2OHX-14 - Digi (X)Net DLC7 di Bollate (MI)"
```

---

## 5. Identity Preservation

FlexNet preserves the originating user's callsign using **AX.25
digipeater semantics**, not L3 field rewriting as in traditional
NET/ROM.

- The user's callsign is the AX.25 L2 source address throughout.
- Intermediate nodes appear in the `via` field, marked `*` when the
  frame has already been heard through them.

Example — user IW7BIA-15 connecting to IR5S via IW2OHX-14 and IR3UHU-2:
```
IW7BIA-15 to IR5S via IW2OHX-14* IR3UHU-2  ctl SABM+
IR5S to IW7BIA-15 via IR3UHU-2* IW2OHX-14  ctl UA-
```

Example — user IW2OHX arriving through RF digipeater DB0FHN and
then onward via IW2OHX-14 → IR3UHU-2 → IR5S:
```
IW2OHX to IR5S via DB0FHN* IW2OHX-14* IR3UHU-2  ctl SABM+
IR5S to IW2OHX via IR3UHU-2* IW2OHX-14 DB0FHN   ctl UA-
```

The CREQ L3 payload carries the originator explicitly:
```
CREQ payload: <originating_callsign> <forwarding_node>
```

### 5.1 AX.25 V2 reciprocity — digi-chain transit caveat

AX.25 V2 requires the UA response's digi list to be the **reverse**
of the SABM's digi list — same nodes in opposite order, only the
`*` "already repeated" flags update along the way.  Any node that
**adds** its own callsign to the digi chain on the forward path
breaks this invariant: the UA returns through the extended chain
and arrives at the originator with **one more digi** than was sent,
and AX.25 V2 rejects the UA.

This rules out a transit forwarding pattern where an intermediate
FlexNet node receives a SABM addressed to a destination it can
reach through a different neighbour and tries to relay by
prepending its own neighbour-callsign as an extra L2 digi.  The
forward path appears to succeed (the far end returns a UA + data),
but the originating node cannot bind the UA to its pending session.

Acceptable approaches for transit forwarding:

1. **Pure L2 digipeat.**  Update `*` flags as the frame passes
   through, never extend the chain.  Requires the originator to
   have included the transit node in its SABM digi list from the
   start.
2. **NetROM L3 forwarding.**  Rewrite at L3 with a fresh L2
   SABM/UA on each hop.  The L3 envelope preserves the originator
   identity (§5); each hop's L2 session is independent.

linbpq-flexnet's v1.9.4 attempt at distance-vector
re-advertisement plus L2 digi-chain extension hit this exact wall
and was reverted in v1.9.7.

---

## 6. Timing Constants

| Constant                       | Value                   |
|--------------------------------|-------------------------|
| RTT unit (wire)                | 100 ms                  |
| RTT infinity                   | 60000                   |
| L3RTT cycle                    | ~240 s                  |
| CE keepalive period            | 180 s (exact)           |
| CE link-time initial value     | `1600\r` (600 s)        |
| CE `3+` reply timeout          | 360 s                   |
| CE type-6/7 TTL cap            | 80 hops                 |
| CE init-handshake frame size   | 5 bytes                 |
| CE keepalive size — xnet       | 241 bytes               |
| CE keepalive size — PCFlexnet  | 201 bytes               |
| CREQ window                    | 4                       |
| T3 (session keepalive base)    | 180000 ms               |

---

## 7. Routing Algorithm

- **Metric:** Round-trip time in 100-ms units.
- **Infinity:** 60000 (sentinel for unreachable).
- **Poison reverse:** on link drop, advertise RTT = 60000 immediately
  (t = 0.0 s) for every destination reachable only through the lost
  neighbor.
- **Priority order** for destination selection:
  destination table > link table > heard list > SSID routing.
- **Distance vector** — no topology database.

---

## 8. Link Disruption Sequence

Observed behaviour when a remote FlexNet link goes down (example:
IR3UHU-2 dropped from IW2OHX-14):

```
t = 0.0 s   DISC/UA, CE '3-', D_TABLE with RTT = 60000 (all immediate)
t = 7.4 s   IR3UHU-2 attempts L2 reconnect, gets CE error
t = 49 s    D_TABLE continues over NET/ROM
```

Recovery:
```
t = 31 s    L3RTT resumes
t = 65 s    CE reinit (type-0 handshake)
t = 68 s    First keepalive + type-1 link-time (`1600\r`)
t = 410 s   Full CE route exchange resumes
```

---

## 9. Polling-Cycle Structure (~240 s)

One complete cycle between neighbors consists of:

1. `[AX.25 SABM/UA]` — not visible in monitor `+11`.
2. About 19 × L3RTT probe (LT=2) + reply (LT=1) + RR acks.
3. Full D-table exchange in both directions (PID = CF).
4. CE token exchange: `3+` + compact records + `3-` per direction.
5. `[AX.25 DISC/UA]`.

Between cycles, a CE keepalive every 180 s keeps the L2 session open.

---

## 10. Scheduling Model

Each node runs a main tick loop at 1-second granularity, iterating
through up to 20 configured AX.25 ports.  Per-port work each tick:

1. If the local routing-table sequence has advanced since the last
   type-4 sent to this peer, emit a type-4 frame.
2. If `now − last_keepalive_tx > 180 s` and the CE session is fully
   up, emit a keepalive.  The peer's type-1 reply updates our
   smoothed RTT estimate.
3. Evaluate the `3+` advertisement gate:
   - Session state must be idle (state 1).
   - Remote link state must be fully established.
   - An internal "advertisement-debt" metric (derived from
     accumulated RTT drift vs the peer's smoothed value) must
     exceed a fixed threshold of 20.
   - The candidate destination's callsign must not equal our own.

   If all conditions hold, emit `3+`, move to state 4, and record
   the timestamp.  If 360 seconds pass with no matching `3-` reply,
   reset to idle.

---

## 11. Implementation Checklist

1. Open an AX.25 connected session to each FlexNet neighbor (L2).
2. On connect, emit CE type-0 init: `0x30 (0x30 + max_ssid) 0x25 0x21 0x0D`.
3. Every ~240 s, run ~19 L3RTT probe/reply pairs over PID = CF then
   a full D-table exchange.
4. After the first keepalive exchange, emit CE type-1 link time
   starting from `1600\r`; converges down as RTT is measured.
5. Every 180 s while the link is up, emit a CE keepalive —
   `'2'` + 240 spaces as a single AX.25 I-frame (xnet sizing;
   PCFlexnet sizes to 201 bytes).
6. CE route exchange per cycle: `3+\r` request, compact records,
   `3-\r` end of batch.
7. On link drop, immediately advertise RTT = 60000 for every
   affected destination.
8. In L3RTT, keep val1 and val2 non-zero while the link is active;
   both zero iff no reply.
9. Distance-vector routing with RTT metric and infinity = 60000;
   poison-reverse on link drop.
10. Include TYPE = 8 and an alias in every L3RTT; set LT = 2 when
    originating the probe.
11. D-command for multi-hop: emit CE type-6 toward the next hop
    and await type-7.
12. D-command for direct neighbors: local D-table lookup only;
    no CE traffic.
13. CREQ: set originator = user callsign, forwarding = local node
    callsign, Window = 4.
14. User-session identity: preserve the L2 source as the user
    callsign end-to-end; intermediate nodes appear only as AX.25
    digipeaters in the via field, never as the L2 source.
15. CE SSID parser: `ssid = ord(c) - 0x30` for `c ∈ '0' … '?'`.
16. When decoding monitor traffic, note that the pretty-printer can
    concatenate adjacent frames into one display entry (the dump's
    offset field resets to `0000` at the boundary).  Do not assume
    a single displayed block is a single on-wire frame.
17. When advertising a contiguous SSID range (§2.6.1), set the
    type-0 init handshake byte 1 (`max_ssid`) to the upper edge of
    the range.  Using the node's own SSID here causes (X)Net to
    silently truncate the advert (§2.3).
18. When integrating FlexNet onto a host with an existing NetROM
    stack (LinBPQ, JNOS, etc.), dispatch PID=CF by **payload
    content** (§3.0): scan for `"L3RTT:"` and only consume the
    frame if found; otherwise return control to the host's NetROM
    L3/L4 dispatcher.  Unconditional PID=CF capture drops user
    CACK/INFO and prevents session establishment.
19. Do not extend the AX.25 L2 digi chain in transit (§5.1).
    Either pure L2 digipeat (no chain extension) or full NetROM
    L3 forwarding (fresh L2 session per hop).  Adding a digi
    mid-route breaks AX.25 V2 reciprocity and the originator
    rejects the UA.

---

## 12. Source Documents

- `DCC1995-FlexNet-DK7WJ-N2IRZ.pdf` — conference paper by the
  FlexNet author (DK7WJ / N2IRZ).
- `xnet138.pdf` — (X)Net 1.38 sysop manual (German).
- `RMNC_FlexNet_and_PC_FlexNet.html` — English sysop-manual excerpt.
- `flexnet1.pdf` — Italian FlexNet overview (1995).
- `flexnet_capture_1h.json` — 1-hour live capture, IW2OHX-14,
  April 2026.
- `flexnet_capture_port1.json` — 30-minute (X)NET ↔ PC/Flexnet
  live capture, April 2026.
- `phase1_results.json` — clean ASCII capture (monitor `-x +11`),
  April 2026.
- `phase2_results.json` — CE frame classification analysis, April 2026.
- `phase3_results.json` — link-disruption capture (RO FL D/A), April 2026.
- `phase4_monitor.txt` — D-command and user-session capture, April 2026.
