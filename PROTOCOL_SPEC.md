# FlexNet Node-to-Node Protocol (CE/CF) — Interoperability Specification

| | |
|---|---|
| **Status** | Informational. Describes the protocol as observed on live networks and as implemented by linbpq-flexnet v2.5.0 and flexnetd v1.0.0. |
| **Version** | 1.2 (2026-10-04) |
| **Applies to** | (X)Net 1.39, PC/Flexnet (V3.3g / V4 family), linbpq-flexnet v2.3.0 and flexnetd v1.0.0. RMNC/Flexnet is expected to follow the same wire format but has not been tested. |
| **Licence** | Same as the repository. Free to use for any independent implementation. |

---

## Abstract

FlexNet is a distance-vector routing protocol for AX.25 packet-radio
networks. Neighbouring nodes run it over a connected-mode AX.25 link,
exchange destination tables whose metric is a measured round-trip time,
discover paths hop by hop, and carry user sessions to multi-hop
destinations by rewriting the AX.25 digipeater chain at each node.

There is no complete public specification of the protocol. This document
is written from packet captures of production (X)Net and PC/Flexnet
nodes, from the published (X)Net sysop manual and FlexNet conference
material, and from the experience of building an interoperable
implementation. It is intended to let others write their own
implementation without repeating that work.

## Status of this document

This is **not** an authoritative specification from the protocol's
authors. Everything here is either (a) observed on the wire between real
implementations, or (b) a requirement that an implementation found
necessary to interoperate. Where the meaning of a field is not settled,
the document says so (see §14, *Open questions*). A live capture of the
peer you need to talk to outranks this text.

## Conventions

The key words **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT** and
**MAY** are to be interpreted as in RFC 2119. They are used for
behaviour that is required to interoperate with the implementations
listed above, not as a claim about what the protocol's authors intended.

Byte values are written in hexadecimal (`0x33`) or as quoted ASCII
(`'3'`). `CR` is `0x0D`. Callsigns in examples are fictional
(`NODEA`, `NODEB-2`, `USER-1`, …).

---

## Contents

1. Terminology
2. Architecture
3. Transport requirements
4. Protocol identifiers (PID)
5. PID `0xCE` — native FlexNet frames
6. Link session lifecycle
7. Routing
8. Route exchange rules
9. Path discovery (types 6 and 7)
10. Carrying user sessions — L2 forwarding
11. PID `0xCF` — the NET/ROM-compatible layer
12. Timing and limits
13. Interoperability notes by implementation
14. Open questions
15. Security considerations
16. Implementation checklist
- Appendix A — Frame examples
- Appendix B — References

---

## 1. Terminology

| Term | Meaning |
|---|---|
| **Node** | A station running a FlexNet router or participant. |
| **Neighbour / peer** | A node with which this node holds a FlexNet link. |
| **Link** | A connected-mode AX.25 session between two neighbours that carries PID `0xCE` frames. |
| **Destination** | A callsign with a contiguous SSID range (`CALL lo-hi`), reachable through the network. |
| **Cost / RTT** | The routing metric: a round-trip time in units of 100 ms (§7.1). |
| **Infinity** | Cost `60000`: unreachable. |
| **Record** | One destination entry inside a type-3 frame. |
| **Transaction** | A route exchange opened by `3+` and closed by `3-` (§8.2). |
| **Chain** | The AX.25 digipeater list of a frame, or the callsign list of a type-6/7 frame. |
| **H-bit** | The AX.25 "has been repeated" bit on a digipeater address. Shown as `*`. |
| **Direct neighbour** | A destination that is itself a peer of this node. |

## 2. Architecture

- FlexNet is **hop-by-hop**. Each node talks only to its neighbours. There
  is no flooding and no topology database; each node holds, per
  destination, the best cost learned from each neighbour.
- The **routing plane** is PID `0xCE` on the neighbour link: link
  measurement, keepalive, route records, path discovery.
- The **forwarding plane is AX.25 itself.** A user session to a remote
  destination is an ordinary AX.25 connection whose digipeater chain the
  nodes along the path rewrite (§10). User sessions are **not**
  encapsulated and are **not** carried in NET/ROM L3/L4 between FlexNet
  routers.
- PID `0xCF` on the same link carries a NET/ROM-compatible layer used for
  link-quality probes (L3RTT) and, between some implementations, NET/ROM
  L3/L4 traffic (§11).
- Identity is preserved by addressing alone: the user's callsign is the
  AX.25 source address end to end; nodes appear only as digipeaters.

```
  USER-1 ── NODEA ════ NODEB ════ NODEC ── DEST
                 FlexNet     FlexNet
                 link        link
  routing plane : PID 0xCE between neighbours only
  user session  : one AX.25 connection USER-1 -> DEST,
                  digi chain rewritten at NODEB (§10)
```

## 3. Transport requirements

3.1. A FlexNet link is an AX.25 **connected-mode** session between two
neighbours. It MAY run over RF, AXIP or AXUDP. Tested deployments used
AXUDP, and a KISS link between two linbpq-flexnet nodes.

3.2. Implementations MUST use **AX.25 version 2.0** framing on a FlexNet
link. Captures show (X)Net answering an XID (AX.25 2.2 negotiation) with
FRMR followed by DISC. An implementation that supports 2.2 SHOULD disable
XID/SABME towards FlexNet peers.

3.3. **Retry budget.** Captures show (X)Net over a low-latency link
adapting T1 down to about 62 ms and giving up after about 9 polls —
roughly **0.6 s** — after which it deletes the session locally without
sending DISC. An implementation MUST NOT stall its AX.25 processing for
anything approaching that interval (garbage collection, blocking I/O,
lock contention). A single stall longer than the peer's budget ends the
link.

3.4. An implementation SHOULD be patient in the other direction. A peer
may stall for tens of seconds under load; tearing the link down after a
few retries causes needless route churn. A retry budget of the order of
a minute (for example 25 retries × 3 s) has been found adequate.

3.5. The session is normally opened by either side with SABM. A node
MUST accept an inbound SABM from a configured FlexNet neighbour and MUST
NOT send it any non-FlexNet traffic on that link — in particular no
connect banner or text on PID `0xF0`. PC/Flexnet has been observed to
disconnect a link that receives such traffic.

3.6. A node SHOULD treat a FlexNet link as either FlexNet or NET/ROM,
not both: NET/ROM and FlexNet both use PID `0xCF` (§11).

3.7. **Someone has to open the link.** Every AXUDP link in the captures
was opened by the (X)Net or PC/Flexnet side. Two neighbours that both
wait for the other's SABM never form a link — the case for two nodes of
an implementation that only accepts. An implementation SHOULD be able
to open the session to a configured neighbour itself and reopen it
after it drops. On RF it SHOULD back off while the neighbour does not
answer (linbpq-flexnet: 60 s, doubling to 15 min) so that an absent
station does not occupy the channel, and both sides opening at once
MUST be harmless: a SABM received while one's own SABM is outstanding
is answered with UA and the link is up.

## 4. Protocol identifiers (PID)

| PID | Carries | Section |
|---|---|---|
| `0xCE` | Native FlexNet: init, link time, keepalive, routes, path discovery | §5 |
| `0xCF` | NET/ROM-compatible layer: L3RTT probes, NET/ROM L3/L4 | §11 |
| `0xF0` | Plain AX.25 text / UI beacons | — |

**Receivers SHOULD classify by content as well as by PID.** PC/Flexnet
has been observed to send frames whose body is a valid CE frame (for
example a keepalive or a link-time frame) with PID `0xF0`. A receiver
that drops or mis-routes them loses link-measurement samples. A body that
parses as one of the CE shapes in §5 on an established FlexNet link
SHOULD be processed as CE.

## 5. PID `0xCE` — native FlexNet frames

### 5.1 Dispatch

The first byte of every CE body is an ASCII digit that selects the frame
type:

| First byte | Type | Purpose |
|---|---|---|
| `'0'` | 0 | Init handshake |
| `'1'` | 1 | Link time |
| `'2'` | 2 | Keepalive |
| `'3'` | 3 | Route tokens (`3+`, `3-`) and route records |
| `'4'` | 4 | Sequence / token frame |
| `'5'` | 5 | Reserved — never observed |
| `'6'` | 6 | Path request (traversal) |
| `'7'` | 7 | Path reply |

A receiver MUST ignore (not answer) a CE frame it cannot classify. A
wrong answer is worse than none: peers do not report protocol errors,
they drop the link or the frame silently.

### 5.2 SSID encoding

Wherever a CE frame carries an SSID as a single byte, the value is
`0x30 + ssid`, giving `'0'`–`'9'` for 0–9 and `':'` `';'` `'<'` `'='`
`'>'` `'?'` for 10–15. Decode with `ssid = byte − 0x30` for bytes
`0x30`–`0x3F`.

### 5.3 Type 0 — Init handshake

```
'0'  MAXSSID  <flags...>  CR
```

| Field | Size | Value |
|---|---|---|
| type | 1 | `'0'` (`0x30`) |
| MAXSSID | 1 | `0x30 +` the highest SSID the sender will advertise for its own callsign |
| flags | 2–3 | Observed: (X)Net and linbpq-flexnet send `'%' '!'` (5-byte frame); PC/Flexnet sends `' ' ' ' '!'` (6-byte frame). Meaning not established. |
| end | 1 | `CR` |

5.3.1. Each side sends one init immediately after the AX.25 session comes
up. It is **not** repeated during the life of the session.

5.3.2. **MAXSSID bounds what the peer will accept from you.** (X)Net has
been observed to clamp the upper SSID of every subsequent record from a
peer to that peer's declared MAXSSID. A node that advertises an SSID
range for its own callsign (§7.6) MUST declare the upper edge of that
range here, not its own SSID.

5.3.3. A receiver MUST accept both the 5-byte and 6-byte forms.

5.3.4. A node SHOULD NOT send a fresh init on a link that is still
established. PC/Flexnet treats a received init as the start of a new
session and resets its link measurement for that neighbour (§6.3).

5.3.5. Because the peer's init arrives once, an implementation that
recreates its per-link state mid-session (for example after an internal
table rebuild) MUST be able to consider the link established from
ongoing valid CE traffic, not only from having seen the init.

### 5.4 Type 1 — Link time

```
'1'  DECIMAL  CR
```

`DECIMAL` is one or more ASCII digits: the sender's current smoothed
link-time figure.

5.4.1. Frames as short as three bytes (`"10" CR`, `"12" CR`) are valid
type-1 frames carrying a single-digit value. (X)Net has been observed to
reply to a link-time frame with `"10" CR` within 1 ms on a fast link.
A receiver MUST NOT mistake a three-byte type-1 frame for anything else.

5.4.2. **Units.** The value is treated by peers as a link-cost figure
on the same 100 ms scale as route costs. A freshly established
PC/Flexnet session starts high (values of `600` and above are routinely
seen at session start) and converges down. An implementation MAY
advertise a small constant (linbpq-flexnet sends `5`); it is the
*timing* of type-1 frames, not their value, that peers use to measure
the link (§6.3).

5.4.3. A type-1 frame is the normal response to a peer's keepalive or
link-time frame, subject to the rate limit in §6.3.

### 5.5 Type 2 — Keepalive

```
'2'  SPACE × N  [CR]
```

| Sender | Observed shape |
|---|---|
| (X)Net | `'2'` + 240 spaces, no `CR` (241 bytes) |
| PC/Flexnet | `'2'` + 199 spaces + `CR` (201 bytes) |

5.5.1. A receiver MUST accept any frame of two or more bytes whose first
byte is `'2'` and whose second byte is a space.

5.5.2. The trailing byte distinguishes the two families in practice:
**a keepalive ending in `CR` identifies a PC/Flexnet peer**, one ending
in a space an (X)Net-like peer. linbpq-flexnet uses this to select
per-peer timing (§6.3, §8.4).

5.5.3. Both families accept the 241-byte (X)Net form.

### 5.6 Type 3 — Route tokens and route records

Three frames share the `'3'` prefix. The two tokens are exactly three
bytes long:

| Frame | Bytes | Meaning |
|---|---|---|
| `3+` | `'3' '+' CR` | **Request**: "send me your whole destination table." Opens a transaction (§8.2). |
| `3-` | `'3' '-' CR` | **End of batch**: "I have finished sending." Closes a transaction. |
| record frame | see below | One or more route records. |

`3+` is a request, not an announcement. A node MUST NOT prefix its own
record frames with `3+`; PC/Flexnet has been observed to disconnect a
link that does so repeatedly.

#### 5.6.1 Record frame

```
record-frame = '3' 1*record [ '-' ] CR
record       = CALL SSID-LO SSID-HI COST ' '
```

| Field | Size | Value |
|---|---|---|
| `'3'` | 1 | **One per frame**, not one per record |
| CALL | 6 | Base callsign, upper case, space-padded on the right |
| SSID-LO | 1 | `0x30 +` lowest SSID of the range |
| SSID-HI | 1 | `0x30 +` highest SSID of the range |
| COST | 1–5 | ASCII decimal cost in 100 ms units; `60000` = infinity |
| `' '` | 1 | Record separator |
| `'-'` | 0–1 | If present immediately before `CR`: **every record in the frame is a withdrawal** (cost infinity), whatever its COST digits say |

5.6.2. Senders SHOULD pack as many records as fit into one I-frame.
Captured (X)Net and PC/Flexnet record frames fill 200–250 bytes of a
256-byte PACLEN. Sending one record per I-frame is legal but wastes most
of every frame and makes a full-table exchange tens of times slower.

5.6.3. A parser MUST expect **one** leading `'3'` followed by N records.
A parser that assumes one `'3'` per record reads single-record frames
correctly and silently loses every multi-record frame a real peer sends.

5.6.4. A single SSID is a degenerate range (`SSID-LO == SSID-HI`); the
encoding is the same.

5.6.5. A record with cost `0` has been observed: (X)Net re-sends its
table shortly after session start with every cost `0`, as a refresh
marker. Its exact meaning is not established (§14). A receiver MUST NOT
let such a record overwrite a real cost, and SHOULD NOT install or
re-advertise it.

5.6.6. A `'?'` byte immediately before the COST digits has been reported
in older material as an "indirect measurement" marker. It is
distinguishable from SSID 15 by position. Receivers SHOULD tolerate it.

### 5.7 Type 4 — Sequence / token frame

```
'4'  DECIMAL  [CHAR]  CR
```

Observed from PC/Flexnet; (X)Net's form carries no trailing character
(`'4' DECIMAL CR`). It is described in older material as a routing-table
sequence number that tells the neighbour its table has changed. The
exact semantics are not established (§14).

A node SHOULD NOT send type-4 frames on its own initiative: (X)Net 1.39
has been observed to withdraw every route learned from a neighbour about
20 s after receiving an unsolicited type-4 from it. Echoing a type-4
received from a PC/Flexnet neighbour back to it (linbpq-flexnet's
behaviour) has caused no observed problem.

### 5.8 Type 5

Reserved. Never observed. Do not send; ignore on receipt.

### 5.9 Types 6 and 7 — Path request and reply

```
'6' | '7'   HOP   QSO(5)   CALL *( ' ' CALL )   [CR]
```

| Field | Size | Value |
|---|---|---|
| type | 1 | `'6'` request, `'7'` reply |
| HOP | 1 | `0x20 +` a small counter. A fresh request carries `0x21`. Each node that forwards a request increments it by one (§9). |
| QSO | 5 | Right-aligned ASCII decimal correlator chosen by the originator (`"    7"`). Bit `0x40` of the **first** QSO byte is a trace flag and MUST be masked before parsing the number. |
| chain | var | Space-separated callsigns with `-SSID` suffix where non-zero. |

The chain is always **anchored on the node that asked**:
`[asker, hop1, …, target]`. The procedure is in §9.

---

## 6. Link session lifecycle

### 6.1 Establishment

1. AX.25 SABM/UA between the two neighbours.
2. Each side sends a type-0 init (§5.3).
3. Each side sends a keepalive and a link-time frame.
4. Each side advertises its own destinations (§8.1). A node SHOULD begin
   advertising on receipt of the peer's init; it MUST NOT wait for the
   peer's first keepalive, which PC/Flexnet may not send promptly.
5. From then on: keepalives, link-time frames and route records flow as
   described below.

### 6.2 Keepalive

A node SHOULD send a keepalive on each link at least every few minutes.
Observed and working cadences:

| Towards | Cadence |
|---|---|
| (X)Net | about 180–300 s |
| PC/Flexnet | about every 29–30 s |

PC/Flexnet has been observed to cycle AXIP links whose peer is too quiet;
the shorter cadence avoids it.

6.2.1. **Echoing keepalives.** flexnetd and linbpq-flexnet answer a
received keepalive with one of their own; this is the behaviour
PC/Flexnet's link measurement has been observed to work with. Neither
(X)Net nor PC/Flexnet was observed to answer such an answer. Two nodes
that both answer every keepalive, however, answer each other without
end: the first link between two linbpq-flexnet nodes exchanged 13 093
keepalives in about three minutes. A node that echoes keepalives MUST
NOT echo one that is itself an echo; since the two are identical on the
wire, it SHOULD echo at most once per interval per link
(linbpq-flexnet: 60 s towards (X)Net-like peers, unrestricted towards
PC/Flexnet, which keepalives more often and never echoes back).

### 6.3 Link measurement

Each side measures the link from the **timing** of the peer's CE frames.
Two behaviours of PC/Flexnet matter to an implementer:

6.3.1. **Do not answer too fast.** When a link-time frame from us
arrives earlier than PC/Flexnet expects the next one, it records the
link-cost sample as saturated (`4095`), and its reported cost for us
stays pinned at the 12-bit maximum. Towards a PC/Flexnet peer an
implementation SHOULD send at most one type-1 frame per **320 s** after
the initial handshake. Towards (X)Net, one per 20 s works. Lowering the
PC/Flexnet interval has been tried and reproduced the saturation.

6.3.2. **Do not re-init.** PC/Flexnet restarts its measurement for a
neighbour when it receives an init, and its session-start samples are
large (`600` and more). A spurious init on a healthy link therefore
inflates our cost for tens of minutes (§5.3.4).

6.3.3. **Your cadence becomes the peer's measurement.** (X)Net
samples the intervals between a neighbour's CE frames. A neighbour that
sent a keepalive every 20 s saw (X)Net report its link at about 17 s;
answering (X)Net's own keepalive and link-time frames as they arrive, and
sending nothing periodic in between, lets (X)Net report the true link
time. Conversely, gaps of 300 s or more between type-1 frames to (X)Net
left its measurement stuck at its initial value. Towards (X)Net: reply to
its frames promptly, and add no periodic traffic of your own faster than
its own ~190 s keepalive.

6.3.4. PC/Flexnet's reported cost for a peer is a smoothed average of
its recent samples, so an outlier decays over several exchanges rather
than disappearing immediately. A test of a change to link timing MUST
allow for that.

### 6.4 Session end

6.4.1. When a link goes down, a node MUST treat every destination
learned only through that neighbour as unreachable and withdraw it from
its other neighbours (§7.4).

6.4.2. PC/Flexnet has been observed to end AXIP sessions on a **fixed
lifetime** of about 5445 s, regardless of traffic, and to rebuild its
FlexNet state on the new AX.25 session (init, measurement, full table).
An implementation MUST handle a new SABM from an existing peer as a new
FlexNet session from the peer's point of view — re-send its own
destinations, and reset any per-transaction state held for that peer
(§8.3) — even if its own per-link state survived.

## 7. Routing

### 7.1 Metric

7.1.1. The metric is a round-trip time in **units of 100 ms**. The
(X)Net manual's statement that 2000 "run-time points" add about 200 s
confirms the unit.

7.1.2. The cost a node advertises for a learned destination is the cost
it learned plus its own cost to the neighbour that taught it.

7.1.3. **Wire ceiling.** PC/Flexnet carries link cost in a 12-bit field.
A node MUST clamp any **finite** cost it sends to **4095**. A larger
finite number is read as a corrupt value, not a worse route. The
infinity value `60000` is a signal, not a measurement, and is exempt.

7.1.4. Cost `60000` means unreachable. A record with that cost, or in a
frame ending with `'-'`, withdraws the destination.

### 7.2 Route selection

A node keeps, per destination, the cost learned from each neighbour and
uses the cheapest **as seen from itself**: the learned cost plus its own
cost to that neighbour — the same sum it advertises (§7.1.2). Comparing
learned costs alone makes a destination behind a slow link (RF) look
as cheap as one behind a fast link (AXUDP). On a tie it SHOULD keep the incumbent; (X)Net has
been observed to do so, and flapping between equal paths causes needless
advertisements.

### 7.3 Split horizon

A node MUST NOT advertise a destination back to the neighbour it
learned the route from. It MAY advertise it to a neighbour that also
knows it independently.

### 7.4 Withdrawal (poison reverse)

On losing a neighbour, a node MUST withdraw — send cost `60000` for —
every destination that has no remaining finite path, to every other
neighbour. Destinations still reachable another way SHOULD simply be
re-advertised at their new cost if it changed materially.

### 7.5 Count-to-infinity

Distance-vector routing without a topology database can loop. Captures
show destinations circulating between routers at a cost that rises by a
roughly constant factor on each lap until it reaches infinity. An
implementation:

- SHOULD hold a withdrawn destination down for a period (linbpq-flexnet
  uses 90 s) and ignore finite re-advertisements of it during that time,
  because neighbours echo a withdrawal back as a finite route;
- SHOULD detect a destination whose cost keeps rising geometrically and
  stop re-advertising it. A purely relative change threshold ("advertise
  if the cost moved by more than 10 %") cannot suppress such a ladder,
  since every rung exceeds it. linbpq-flexnet treats three consecutive
  rises that reach four times the lowest cost seen as a loop, withdraws
  the destination once and keeps the lowest-cost reference across the
  withdrawal;
- MUST NOT rely on being able to delete a route from a peer. (X)Net has
  no way to be told to forget a destination; a looping route only dies
  when every node stops feeding it.

### 7.6 Own destinations

7.6.1. A node advertises its own callsign at cost `1`. It MAY advertise
a contiguous SSID range of its own base callsign as one record
(`CALL lo-hi`), declaring the upper edge in its init (§5.3.2).

7.6.2. A node MAY advertise other callsigns it answers for locally —
for example application callsigns that do not share the node's base
callsign — as further records at cost `1`. It MUST advertise only
callsigns it will actually accept a connection for: every peer installs
the route, and a callsign nobody answers becomes a black hole across the
network. It MUST NOT learn such a callsign back from a peer.

7.6.3. A node SHOULD put its own records in a single record frame, so the
number of frames it sends does not grow with the number of own
callsigns (see §8.3 for why frame count matters).

7.6.4. A callsign advertised at cost 1 claims that callsign network-wide.
It MUST NOT be a callsign in use anywhere else, including on NET/ROM.

### 7.7 Scope — advertise only what you can carry

**A node MUST NOT advertise a destination it cannot deliver traffic to.**
Re-advertising makes neighbours prefer you, so an uncarryable
advertisement is worse than none: peers switch to the path through you
and every connection fails.

- A node that does not implement L2 forwarding (§10) can deliver only to
  itself and to its direct neighbours (by plain digipeat). It MUST limit
  re-advertisement to those.
- A node that implements L2 forwarding MAY advertise every destination it
  has learned.
- A node whose L2 forwarding can only leave on the port a frame arrived
  on — plain digipeating, and linbpq-flexnet's forwarding — MUST NOT
  advertise to a neighbour on one port what it learned from a neighbour
  on another. This matters as soon as a node has RF and AXUDP
  neighbours.

## 8. Route exchange rules

### 8.1 Initial exchange

After the link comes up (§6.1) each side sends its own destinations and,
if it re-advertises, its learned table (subject to §7.3 and §7.7).

### 8.2 The `3+` … `3-` transaction

8.2.1. A node that wants a neighbour's whole table sends `3+`.

8.2.2. The receiver MUST answer with its **whole** advertisable table —
not only the entries that changed since it last spoke to that peer —
followed by exactly one `3-` after the last record frame of the answer.

8.2.3. The `3-` MUST follow the last record of the answer. An
implementation that queues the answer behind a rate limiter MUST NOT emit
`3-` merely because its queue is momentarily empty between refills; it
SHOULD wait until the answer has actually drained.

8.2.4. A node MAY open a new link to (X)Net with `3+`, its own records
and `3-`; (X)Net answers with its table. A node MUST NOT send `3+` to a
PC/Flexnet peer outside its session start: PC/Flexnet has been observed to
disconnect the link on an unexpected `3+`.

8.2.5. Observed request frequency: PC/Flexnet sends `3+` roughly every
75–90 minutes on a stable link. (X)Net rarely sends it (typically only
around session setup).

### 8.3 Unsolicited records towards PC/Flexnet

8.3.1. (X)Net accepts record frames at any time. It pushes changes as
they happen and receives pushed changes without complaint.

8.3.2. **PC/Flexnet does not.** Captures show that PC/Flexnet tolerates
unsolicited record frames on a session until it has completed a `3+`
exchange; after the `3-` that closes the answer to its `3+`, it accepts
**at most two** further record frames and then disconnects the link —
synchronously, within tens of milliseconds of the offending frame, on a
link that is otherwise healthy. PC/Flexnet itself sends records almost
exclusively inside `3+` transactions.

8.3.3. Therefore, towards a PC/Flexnet peer, after answering its `3+` a
node MUST NOT send further record frames until that peer's next `3+`,
or until the peer starts a new AX.25 session (§6.4.2). The cost is that
the peer's view of the node refreshes only once per transaction.

8.3.4. The trigger is the number of record frames after the close, not
their content and not the placement of the `3-` token: the same records
are accepted before a transaction, and records shortly after an
*unsolicited* `3-` are accepted.

### 8.4 Pacing

A node that re-advertises SHOULD rate-limit record frames per peer.
Values in use and known to work:

| Towards | Rate | Burst |
|---|---|---|
| PC/Flexnet | 1 record frame per 5 s | 2 |
| (X)Net | 1 record frame per 2 s | 4 |

Between exchanges, re-advertisement SHOULD be **event-driven**: send a
record when the cost you would advertise to that peer has changed
materially since you last told it (linbpq-flexnet: by ≥ 10 % and at
least one 100 ms unit), not on a periodic sweep of the whole table.
Periodic whole-table pushes have been observed to degrade PC/Flexnet
links. Direct-neighbour entries MAY be re-offered periodically (for
example every 120 s) so they do not age out.

## 9. Path discovery (types 6 and 7)

A node asked by a user for the path to a destination (the `D <call>`
command on (X)Net) resolves it hop by hop.

### 9.1 A type-6 is a traversal, not a query

9.1.1. The originator sends a type-6 to the neighbour that is its next
hop towards the target, with chain `[originator, next-hop, target]`,
HOP `0x21` and a fresh QSO.

9.1.2. A node receiving a type-6 whose target is **adjacent** to it (a
direct neighbour, itself, or one of its own advertised callsigns)
answers with a **type-7** carrying the completed chain, and sends it back
to the station immediately before it in the chain.

9.1.3. A node receiving a type-6 whose target is **not** adjacent
**inserts its own next hop immediately before the target**, increments
HOP by one, and forwards the type-6 to that next hop. The QSO is
unchanged.

```
NODEA -> NODEB   '6' 0x21 "    7" "NODEA NODEB DEST"
NODEB -> NODEC   '6' 0x22 "    7" "NODEA NODEB NODEC DEST"     (NODEB not adjacent: forwards)
NODEC -> NODEB   '7' ...  "    7" "NODEA NODEB NODEC DEST"     (NODEC adjacent: answers)
NODEB -> NODEA   '7' ...  "    7" "NODEA NODEB NODEC DEST"     (relayed back)
```

9.1.4. **A type-7 is relayed without state.** Because the chain is
anchored on the asker, a node relaying a type-7 finds its own callsign in
the chain and sends the frame to the element immediately before it. A node
that is the first element of the chain asked the question itself: it
consumes the reply, matching it to its request by QSO.

9.1.5. A node that answers a type-6 from a cache instead of forwarding
it MUST anchor the answer on the asker: `[asker, us, …, target]`.

9.1.6. **Never answer with a chain the asker cannot use.** AX.25 allows
at most 8 digipeaters. An answer whose chain would require more than 8
(the chain minus its first and last element) MUST NOT be sent; forward
the traversal instead, or stay silent. Observed consequence of ignoring
this: the peer installs an unusable path and every connection to that
destination fails at link setup, while connects succeed when no answer
is given at all (the peer then falls back to the two-digi chain of §10
and each node extends it).

9.1.7. A forwarding node MUST NOT forward to a next hop that is already
in the chain or is the originator, and MUST NOT hand the frame back to
the station it came from. The chain is the only loop information the
frame carries.

9.1.8. When no answer arrives, (X)Net shows a cost for the destination
but no route line. A missing route line therefore usually means the
traversal died somewhere upstream, not that the local answer was wrong.

### 9.2 Header byte

Relaying nodes observed on the wire increment the HOP byte by exactly one
per forwarding hop. The byte in replies does not always equal
`0x20 + number of hops`; its full meaning is not settled (§14). An
implementation SHOULD reproduce the observed delta (copy and add one)
rather than recompute the byte from its own theory.

## 10. Carrying user sessions — L2 forwarding

### 10.1 Identity preservation

The user's callsign is the AX.25 source address of every frame from the
user end to end, and the destination's callsign is the AX.25 destination
address. Nodes appear only as digipeaters, marked `*` once they have
repeated the frame.

```
USER-1 -> DEST via NODEA* NODEB      SABM
DEST -> USER-1 via NODEB* NODEA      UA
```

A node originating a connection on behalf of one of its own users SHOULD
put its own callsign in the chain as the first, already-repeated
digipeater (`USER-1 -> DEST via NODEA* NODEB`), so the neighbour sees who
is relaying the user. PC/Flexnet has been observed to refuse (DM) a bare
user SABM with no digipeater from an unknown station.

### 10.2 Destination one hop beyond a node

For a destination adjacent to the next node, the originator's node sends
the two-digipeater chain `<itself>* <neighbour>`, and the neighbour
repeats it by ordinary digipeating. **A FlexNet node that advertises
direct neighbours MUST digipeat such frames.**

### 10.3 Destination several hops away — symmetric chain rewriting

For a destination further away, the originator sends the **same**
two-digipeater chain. Each transit node extends the chain on the way out
and contracts it on the way back:

- **Forward** (frame travelling towards the destination, the node is the
  last unrepeated digipeater, and the destination is not adjacent): set
  the node's own H-bit and **append its next hop** towards the
  destination as a new, unrepeated digipeater.
- **Reverse** (frame travelling back): **remove the entry this node
  appended** and set its own H-bit.

```
in   USER-1 -> DEST   NODEA* NODEB                  SABM
out  USER-1 -> DEST   NODEA* NODEB* NODEC           SABM   (NODEB appends NODEC)
in   DEST -> USER-1   NODEC* NODEB NODEA            UA
out  DEST -> USER-1   NODEB* NODEA                  UA     (NODEB removes NODEC)
```

10.3.1. The originator therefore only ever sees the chain it sent, and
AX.25 version 2's rule that a reply's digipeater list is the reverse of
the request's holds end to end. **Extending without contracting breaks
that rule** and the originator rejects the reply.

10.3.2. The rewrite applies to every frame type of the session: SABM,
UA, I, RR, RNR, REJ, DISC, DM, FRMR. Source and destination addresses are
never changed; nothing is encapsulated.

10.3.3. Captured (X)Net and PC/Flexnet transit nodes emit exactly this
shape. (X)Net routers were never observed sending a NET/ROM CREQ to
carry a user session to a FlexNet destination; they expect the
neighbour to route at L2. A FlexNet participant that relies on NET/ROM
L4 for multi-hop destinations cannot carry them.

### 10.4 State a transit node must keep

10.4.1. On the reverse path "the digipeater I appended" and "a
digipeater the originator put there" are indistinguishable by
inspection. A transit node MUST record, per circuit — keyed at least on
(user, destination, port) — which next hop it appended, and MUST remove
only an entry its own record says it appended. Any other frame falls
through to ordinary digipeating.

10.4.2. A node SHOULD **pin** the next hop for a circuit on its first
frame and keep it while that next hop's link is alive, even if the route
to the destination changes during the session. Re-resolving per frame
makes a mid-session route change return frames carrying a digipeater
the originator never used.

10.4.3. A circuit's state SHOULD follow the AX.25 session: a DISC starts
teardown, the answering UA or a DM completes it, and the state SHOULD be
kept briefly afterwards (linbpq-flexnet: 120 s) for retransmissions. An
open but silent circuit SHOULD be kept for a long time (linbpq-flexnet:
2 h). When the table is full, only closed or long-idle circuits may be
reclaimed.

### 10.5 Loop and length limits

- A node MUST NOT append a callsign already present in the chain.
- A node MUST NOT extend a chain beyond 8 digipeaters, or beyond the
  port's configured maximum. **The 8-digipeater limit is the only hop
  limit FlexNet has**; AX.25 has no TTL.
- A node SHOULD drop a frame on a circuit it forwards that already
  carries its own callsign as a repeated digipeater (the frame has
  looped back to it).

### 10.6 Hop-by-hop acknowledgement (optional)

Captures show (X)Net terminating AX.25 at each hop: it acknowledges
I-frames from its neighbour locally within about a millisecond, while
the far end's replies take tens of milliseconds. Forwarding end to end
(digipeating the session, as §10.3 describes) is legal and interoperates;
per-hop termination is an optimisation, not a requirement.

## 11. PID `0xCF` — the NET/ROM-compatible layer

### 11.1 Dispatch by content

PID `0xCF` on a FlexNet link carries FlexNet's L3RTT probes and may also
carry ordinary NET/ROM L3/L4 traffic. A receiver MUST dispatch by
content: search the payload for the ASCII string `"L3RTT:"`; if present,
handle it as an L3RTT probe (§11.2), otherwise pass the frame to the
host's NET/ROM layer. The string is usually not at offset 0: (X)Net
wraps probes in a NET/ROM L3 header, which puts it about 15 bytes in.

A receiver that consumes every `0xCF` frame as L3RTT silently drops
NET/ROM connection acknowledgements and data, and user sessions time out
even though their L3 handshake succeeded.

### 11.2 L3RTT probe

```
L3RTT:%11lu%11lu%11lu%11lu %-6.6s LEVEL3_V2.1 <ident> $M<n> $N CR
```

| Field | Meaning |
|---|---|
| four counters (`c1`–`c4`) | 10 ms tick values. The originator fills `c1`/`c2`; the responder echoes them and fills `c3`/`c4` with its own tick counter. |
| alias | Sender's 6-character node alias. |
| `LEVEL3_V2.1` | Protocol marker. |
| ident | Implementation identity (for example `(X)NET139`, or `linbpq-1.9`). |
| `$M<n>` | Numeric field; senders place a table size or cost here. |
| `$N` | End marker. |

11.2.1. **Link-down signal.** A responder with no reachable destinations
SHOULD reply with `c3 = 0` and `c4 = 0`; peers read both zero as "do not
route through me". Consequently a responder MUST NOT send zero counters
while healthy — for example from a tick counter that starts at zero.

11.2.2. When the probe arrives inside a NET/ROM L3 INFO envelope, the
reply MUST be returned in the same form: addressed to the prober's node
callsign, from the responder's node callsign, with the incoming TTL and
the circuit index / ID echoed. (X)Net matches replies to probes on those
envelope fields. The reply MUST NOT be addressed to the `L3RTT`
pseudo-destination of the probe; (X)Net re-broadcasts that address and
the reply loops.

### 11.3 NET/ROM L3/L4

Between some implementations, and towards NET/ROM nodes, PID `0xCF`
carries standard NET/ROM connection traffic (CREQ, CACK, INFO, IACK,
DREQ, DACK). It is not needed to carry user sessions between FlexNet
routers (§10.3.3).

## 12. Timing and limits

| Item | Value | Section |
|---|---|---|
| Cost unit | 100 ms | §7.1 |
| Infinity | 60000 | §7.1 |
| Maximum finite cost on the wire | 4095 | §7.1.3 |
| Own-destination cost | 1 | §7.6 |
| Keepalive cadence | ~180–300 s to (X)Net, ~30 s to PC/Flexnet | §6.2 |
| Minimum type-1 interval | 320 s to PC/Flexnet, 20 s to (X)Net | §6.3 |
| Peer AX.25 retry budget ((X)Net, fast link) | ~0.6 s | §3.3 |
| PC/Flexnet AXIP session lifetime | ~5445 s | §6.4.2 |
| PC/Flexnet `3+` interval | ~75–90 min | §8.2.5 |
| Record frames tolerated by PC/Flexnet after closing `3-` | 2 | §8.3 |
| Record-frame pacing | 1/5 s burst 2 (PC/Flexnet), 1/2 s burst 4 ((X)Net) | §8.4 |
| Maximum digipeaters (= hop limit) | 8 | §10.5 |
| Base callsign in a record | 6 characters | §5.6.1 |
| SSID | 0–15 | §5.2 |

## 13. Interoperability notes by implementation

### 13.1 (X)Net

- Sends 5-byte init, 241-byte keepalive without `CR`.
- Accepts pushed record frames at any time; pushes its own changes.
- Rarely sends `3+`; answers one at session start.
- Withdraws a neighbour's routes after an unsolicited type-4 (§5.7).
- Measures a neighbour from the intervals between its CE frames
  (§6.3.3); re-sends its table with cost `0` after session start
  (§5.6.5).
- Clamps a peer's advertised SSIDs to the peer's init MAXSSID (§5.3.2).
- Answers link-time frames very quickly (`"10" CR` within ~1 ms).
- Carries multi-hop sessions by chain rewriting (§10.3), terminates L2
  per hop (§10.6), never uses NET/ROM CREQ for them.
- Answers XID with FRMR (§3.2); short adaptive T1, ~0.6 s total retry
  budget (§3.3).
- Cannot be told to forget a learned destination (§7.5).
- The sysop manual documents per-link routing options (partner/subnet
  advertisement, one-way links, a +2000 penalty for Internet links).
  Their wire behaviour has not been captured.

### 13.2 PC/Flexnet

- Sends 6-byte init, 201-byte keepalive ending in `CR`.
- Sends `3+` about every 75–90 min and exchanges routes inside the
  transaction; disconnects after more than two record frames following
  the close (§8.3).
- Link-cost field is 12 bits; finite costs above 4095 are corrupt to it
  (§7.1.3).
- Samples link cost from frame timing; type-1 frames sent too soon
  saturate the sample (§6.3.1); an init restarts measurement (§6.3.2).
- Cycles AXIP sessions on a fixed ~5445 s lifetime and rebuilds its
  FlexNet state afterwards (§6.4.2).
- May send CE-shaped frames with PID `0xF0` (§4).
- Refuses connect banners / text on a FlexNet link (§3.5) and a bare
  user SABM without a digipeater (§10.1).
- After a peer withdraws a destination, PC/Flexnet may keep advertising
  its copy until its next transaction.

### 13.3 flexnetd

- A participant, not a router: advertises its own callsign (with an
  optional SSID range) and forwards nothing.
- Per-link settings select the behaviour each peer family needs:
  link-time frames on every peer event towards (X)Net and at most one
  per 320 s towards PC/Flexnet; `3+`-framed own records towards (X)Net
  and bare records towards PC/Flexnet (§8.2.4).
- Sends no type-4 frames (§5.7).
- Answers a type-6 whose target is itself; does not forward type-6
  traversals (§9.1.3).
- Deviation: echoes every keepalive it receives (§6.2.1). Harmless
  towards (X)Net, PC/Flexnet and linbpq-flexnet v2.5 or later, none of
  which echoes an echo; two flexnetd nodes linked to each other would
  echo without end.

### 13.4 linbpq-flexnet

- Sends 5-byte init, 241-byte keepalive without `CR`, type-1 value `5`.
- Identifies itself as `linbpq-1.9` in the L3RTT identity field.
- Implements §8.3 towards PC/Flexnet peers (configurable, on by
  default), §10 as an opt-in, and §9.1.3 forwarding as an opt-in.
- Opens and keeps up links to neighbours declared on KISS ports (§3.7);
  on AXUDP it waits for the neighbour's SABM.
- Echoes keepalives at most once per 60 s towards (X)Net-like peers
  (§6.2.1); advertises within one port only (§7.7).
- Deviation: by default it classifies a three-byte `"1n" CR` as a status
  frame rather than as a type-1 (§5.4.1); the conforming behaviour is
  enabled with `FLEXNETLT3BYTE YES`.

## 14. Open questions

These are not needed to interoperate but are not understood:

1. The init flag bytes (`'%' '!'` vs `' ' ' ' '!'`).
2. The exact meaning of type 4 and whether it should be echoed.
3. The meaning of cost `0` in a record.
4. The full semantics of the type-6/7 HOP byte, which in replies does
   not always equal `0x20 +` hops.
5. The semantics of the three-byte status-like frames `"1n" CR`
   observed from (X)Net (for example `"12" CR`) when not sent in reply to
   a link-time frame.
6. Whether (X)Net withdraws or falls silent when a sysop changes a
   link's routing options, and whether its `+` penalty applies to
   received or re-advertised routes.
7. Whether PC/Flexnet applies to pushed records from (X)Net the same
   limit it applies after a `3+` transaction (§8.3).

## 15. Security considerations

FlexNet has no authentication. Any station that can establish a link
with a node can advertise any destination at any cost, and a lower cost
attracts traffic. Implementations SHOULD:

- peer only with explicitly configured neighbours;
- advertise only destinations they can carry (§7.7) and callsigns they
  own (§7.6);
- validate every length, count and callsign in received frames before
  use, and bound every table;
- never rewrite a frame's chain unless they recorded the change (§10.4).

AXUDP traffic is unencrypted and unauthenticated; source-address
filtering on the UDP port is advisable.

## 16. Implementation checklist

1. AX.25 2.0 connected-mode link per neighbour; no XID (§3.2).
2. Never stall AX.25 processing for more than a fraction of a second
   (§3.3); be patient with slow peers (§3.4).
3. No banners or `0xF0` text on a FlexNet link (§3.5). Be able to open
   the link yourself, with back-off on RF (§3.7).
4. Classify CE frames by content, including those arriving with PID
   `0xF0` (§4, §5.1).
5. Send one init per session, with MAXSSID covering your advertised SSID
   range (§5.3); accept 5- and 6-byte inits.
6. Accept three-byte type-1 frames (§5.4.1).
7. Accept any keepalive `'2' ' ' …`; tell PC/Flexnet from (X)Net by the
   trailing `CR` (§5.5). If you echo keepalives, never echo an echo
   (§6.2.1).
8. Parse and build record frames as one `'3'` + N records, packed; honour
   the trailing `'-'` (§5.6).
9. Never prefix your records with `3+` (§5.6).
10. Rate-limit type-1 frames: 320 s to PC/Flexnet (§6.3.1). Never re-init
    a live link (§6.3.2).
11. Treat a new SABM from an existing peer as a new session (§6.4.2).
12. Clamp finite costs to 4095; infinity is 60000 (§7.1.3).
13. Split horizon, withdrawal on neighbour loss, hold-down, loop
    detection (§7.3–§7.5).
14. Choose next hops by learned cost plus your own link cost (§7.2).
    Advertise only what you can carry — not across ports if your
    forwarding cannot change port (§7.7) — and only callsigns you answer
    (§7.6).
15. Answer `3+` with the whole table and a single, final `3-` (§8.2).
16. After answering a PC/Flexnet `3+`, send it no records until its next
    `3+` or a new session (§8.3).
17. Pace record frames per peer; re-advertise on change, not by sweep
    (§8.4).
18. Type-6: answer when adjacent, otherwise insert your next hop and
    forward; relay type-7 by chain position; never answer with more than
    8 digipeaters (§9).
19. L2 forwarding: append on the way out, remove what you appended on
    the way back, pin the hop per circuit, 8-digipeater limit, no
    duplicate callsigns (§10).
20. Dispatch PID `0xCF` by content; mirror the L3 envelope on L3RTT
    replies; zero counters only when you have no routes (§11).

---

## Appendix A — Frame examples

### A.1 Init declaring SSIDs up to 8

```
30 38 25 21 0D                          '0' '8' '%' '!' CR
```

### A.2 Keepalive, (X)Net form

```
32 20 20 20 … 20                        '2' + 240 spaces (241 bytes, no CR)
```

### A.3 Link time

```
31 35 0D                                "15" CR        -> type 1, value 5
31 30 0D                                "10" CR        -> type 1, value 0
```

### A.4 Own record, SSID range 0–8, cost 1

```
33 4E 4F 44 45 41 20 30 38 31 20 0D
'3' 'N' 'O' 'D' 'E' 'A' ' ' '0' '8' '1' ' ' CR
```

`NODEA` padded to six characters, SSID-LO `0`, SSID-HI `8`, cost `1`.

### A.5 Three records in one frame

```
'3' "NODEB " '2' '2' "3 " "NODEC " '0' '0' "17 " "DEST  " '0' '0' "42 " CR
```

`NODEB-2` at 3, `NODEC` at 17, `DEST` at 42 (0.3 s, 1.7 s, 4.2 s).

### A.6 Withdrawal of two destinations

```
'3' "NODEC " '0' '0' "60000 " "DEST  " '0' '0' "60000 " '-' CR
```

### A.7 Tokens

```
33 2B 0D                                "3+" CR        request whole table
33 2D 0D                                "3-" CR        end of batch
```

### A.8 Path request forwarded by one node

```
'6' 0x21 "   12" "NODEA NODEB DEST"             NODEA -> NODEB
'6' 0x22 "   12" "NODEA NODEB NODEC DEST"       NODEB -> NODEC
```

### A.9 L2 forwarding of an I-frame and its RR

```
in   USER-1 -> DEST   NODEA* NODEB              I
out  USER-1 -> DEST   NODEA* NODEB* NODEC       I      (NODEB)
in   DEST -> USER-1   NODEC* NODEB NODEA        RR
out  DEST -> USER-1   NODEB* NODEA              RR     (NODEB)
```

## Appendix B — References

- *(X)Net 1.38 sysop manual* (German), <https://xnet.swiss-artg.ch/pdf/xnet138.pdf>
  — in particular the link routing options (§4.3.24.3.1, p. 37).
- DK7WJ / N2IRZ, FlexNet paper, Digital Communications Conference, 1995.
- RMNC/FlexNet and PC/FlexNet sysop documentation (English excerpt).
- AX.25 Link Access Protocol for Amateur Packet Radio, version 2.0 / 2.2.
- linbpq-flexnet (<https://github.com/onionuser79/linbpq-flexnet>),
  `FlexNetCode.c` — a complete implementation of this document; builders
  and parsers are commented with byte layouts.
- flexnetd (<https://github.com/onionuser79/flexnetd>) — a FlexNet
  daemon for Linux AX.25 / URONode implementing the link protocol and
  route exchange.

This document is maintained identically in both repositories.
