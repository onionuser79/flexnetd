# FlexNet / (X)Net Protocol — Protocol Findings
# Based on live captures from IW2OHX-14 <-> IR3UHU-2 and IW2OHX-14 <-> IW2OHX-13
# Initial: 2026-04-10 | Phase 1/2: 2026-04-13 | Phase 3: 2026-04-13 | Phase 4: 2026-04-13 | M2: 2026-04-14
# Author: IW2OHX + Claude Sonnet 4.6 + Claude Opus 4.6
# Verified against: DCC1995 paper (DK7WJ/N2IRZ), xnet138.pdf, RMNC_FlexNet.html
# xnet linuxnet binary RE: 2026-04-21 (RE_NOTES_XNET.md)
# Status: ALL INVESTIGATION ITEMS RESOLVED
# =======================================================================
#
# IMPORTANT — v0.7.2 corrections (xnet binary RE, April 2026)
# -----------------------------------------------------------
# The xnet linuxnet binary was reverse-engineered end-to-end on
# 2026-04-21 (see RE_NOTES_XNET.md for full details).  Several
# entries in this file were subsequently corrected where they
# conflicted with the actual on-wire behaviour of xnet.  Look for
# [v0.7.2 RE] markers throughout this file for the updated text.
# =======================================================================

## CRITICAL: PID assignments (confirmed from xnet138.pdf section 4.3.12)

  PID=CE (0xCE) = FlexNet native protocol
  PID=CF (0xCF) = NET/ROM protocol
  PID=F0 (0xF0) = Plain AX.25 / UI frames

---

## PID=CF (0xCF) — NET/ROM-compatible Routing Layer

### L3RTT — Round-trip time probe

  Payload: L3RTT: <counter>  <val1>  <val2>  <node_serial>  <NODE_ALIAS>  LEVEL3_V2.1  (X)NET<ver>  $M<rtt>  [$N]

  Fields (confirmed Phase 1 + Phase 3):

    counter     : monotonic ms tick counter (10-digit). Used as RTT echo token.

    val1        : CONFIRMED: 0 when no reply ($M=60000), non-zero when active.
                  Range 0,3,4,5.

    val2        : CONFIRMED: 0 when no reply ($M=60000), non-zero when active.
                  KEY RULE: val1=0 AND val2=0 = reliable "link down" indicator.
                  Never simultaneously zero during normal operation.

    node_serial : Node-specific fixed identifier.
                    NETUHU (IR3UHU-2): 3076478712
                    BOLNET (IW2OHX-14): 67717840

    NODE_ALIAS  : 6-char name of the NODE SENDING the probe.

    $M<rtt>     : RTT in 100ms units. $M=60000 = no reply.

    $N          : End-of-record sentinel.

  LT field: LT=2=initiator, LT=1=responder. ~4% exceptions at link recovery.


### D_TABLE — Destination table exchange

  Monitor format : CALLSIGN-SSID   RTT/SSID_MAX  [PORT[TYPE] 'ALIAS']
  D command format: CALLSIGN  SSID_LO-SSID_HI  RTT  (4 entries/line)

  RTT=60000 = poison reverse. PORT: 0=RF, 1=HAMNET, 16-23=AXIP/AXUDP.
  TYPE: 8=Xnet/DAMA, 7=FlexNet, 6=BPQ, 5=JNOS, 4=legacy.

  On link drop: immediate D_TABLE with all RTT=60000 (t=0.0s).
  D_TABLE over NET/ROM continues even when CE native session is down.


### CREQ / CACK — Connection request / acknowledgement (Phase 4)

  CREQ: L3 fm <node> to <dest> LT <lt> CREQ IN=<in> ID=<id> Window=4 <originator> <forwarding_node>
  CACK: L3 fm <node> to <node> LT <lt> CACK IN=<in> ID=<id> Window=4

  originator    : ORIGINATING USER CALLSIGN -- identity preserved at L3.
  forwarding    : local node callsign forwarding the CREQ.
  Window        : always 4.
  ID            : session ID, constant for session lifetime.

  Multi-hop: CREQ propagates forward through all hops. CACK comes from the
  final destination node directly (not from the intermediate neighbor).

  LT values in CREQ/CACK: 11, 14, 17 observed. Packed field, exact bits TBD.
  Different from the LT=1/2 used in L3RTT.


### DREQ / DACK — Disconnect request / acknowledgement

  Same structure as CREQ/CACK but no callsign payload.


### I / IACK — User data frames (transit)

  I:    L3 fm <src> to <dst> LT <lt> I IN=<in> ID=<id> S(<seq>) R(<ack>) [<len>]
  IACK: L3 fm <src> to <dst> LT <lt> IACK IN=<in> ID=<id> S(<seq>) R(<ack>)


### Digipeater path preservation — CONFIRMED (Phase 4)

  FlexNet preserves the originating user callsign using AX.25 DIGIPEATER
  SEMANTICS -- NOT by rewriting L3 fields as NET/ROM does.

  The user callsign is the AX.25 L2 SOURCE ADDRESS throughout.
  Intermediate nodes appear in the via field, marked with * when heard.

  Example (IW7BIA-15 -> IR5S via IW2OHX-14 and IR3UHU-2):
    IW7BIA-15 to IR5S via IW2OHX-14* IR3UHU-2  ctl SABM+
    IR5S to IW7BIA-15 via IR3UHU-2* IW2OHX-14  ctl UA-

  Example (IW2OHX via RF digi DB0FHN -> IW2OHX-14 -> IR3UHU-2 -> IR5S):
    IW2OHX to IR5S via DB0FHN* IW2OHX-14* IR3UHU-2  ctl SABM+
    IR5S to IW2OHX via IR3UHU-2* IW2OHX-14 DB0FHN   ctl UA-

  Example (outbound from URONode — IW7CFD connects to IR5S via IW2OHX-3):
    IW7CFD-15 to IR5S via IW2OHX-3* IW2OHX-14  ctl SABM+
    Destination U output: IR5S>IW7CFD-15 v IQ5KG-7 IW2OHX-3

  The CREQ L3 payload also carries the originator explicitly:
    CREQ payload: <originating_callsign> <forwarding_node>


### Linux AX.25 kernel: AX25_IAMDIGI requirement — CONFIRMED (2026-04-14)

  When originating an outbound SABM with digipeaters, the Linux kernel's
  ax25_connect() only honors H bits from userspace if the socket has
  AX25_IAMDIGI set. Without it, all repeated[] flags are cleared:

    /* net/ax25/af_ax25.c — ax25_connect() */
    if ((fsa->fsa_digipeater[ct].ax25_call[6] & AX25_HBIT) && ax25->iamdigi)
        digi->repeated[ct] = 1;    /* H bit honored */
    else
        digi->repeated[ct] = 0;    /* H bit CLEARED */

  Required socket setup before connect():
    int val = 1;
    setsockopt(fd, SOL_AX25, AX25_IAMDIGI, &val, sizeof(val));  /* value 12 */
    sa.ax.fsa_digipeater[0].ax25_call[6] |= 0x80;               /* H bit */

  Without AX25_IAMDIGI, SABM goes out with no H bits set, and the neighbor
  drops the frame (unrepeated digi doesn't match its callsign).

  AX25_IAMDIGI = 12 (from <linux/ax25.h>, may be missing from older libax25)

---

## PID=CE (0xCE) — Native FlexNet Protocol

### CE SSID encoding — CONFIRMED for all values 0-15 (item #6)

  ALL CE frames use the same SSID encoding: SSID value N = ASCII char (0x30 + N)

  N=0..9  -> '0'..'9'  (0x30..0x39, decimal digits)
  N=10    -> ':'        (0x3A)
  N=11    -> ';'        (0x3B)
  N=12    -> '<'        (0x3C)
  N=13    -> '='        (0x3D)
  N=14    -> '>'        (0x3E)
  N=15    -> '?'        (0x3F)

  Consistent across ALL CE frame types: type-0 init, compact records, type-6/7.
  Confirmed from 86 CE frames with SSID>=10 in the 1h capture. Examples:
    OE2XZR;;140  -> OE2XZR SSID=11 RTT=140
    OE2XZR==140  -> OE2XZR SSID=13 RTT=140
    HB9AK >>842  -> HB9AK  SSID=14 RTT=842
    IK1NHL4?5    -> IK1NHL SSID=4, indirect RTT=5

  DISAMBIGUATION: '?' has two meanings:
    As SSID (0x3F = SSID 15): in the callsign+SSID field position
    As RTT prefix (indirect):  immediately before the RTT digits
  Context determines which -- SSID follows callsign, prefix follows SSID.

  Parser: ssid = ord(c) - 0x30  for c in range '0'..'?' (0x30..0x3F)


### CE type-0 — Link setup / init handshake (Phase 3)

  5 bytes: 0x30, 0x30+max_ssid, 0x25, 0x21, 0x0D
  Byte 0 MUST be 0x30. Using 0x30+min_ssid misclassifies as routing type '3'.
  Error response: '02%! FlexLink:No Link to N:CALL defined'
  Success: '02%! FlexLink:Init fm IR3UHU-2/2 (25 21)'


### CE type-1 — Link time measurement (Phase 3)

  Format: '1' <decimal_ticks> '\r'
  Initial: '1600\r' (600 ticks = RTT_INFINITY). Converges down over cycles.
  Used by neighbor for Q/T link quality metric.


### CE type-2 — Keepalive / null frame [v0.7.2 RE — CORRECTED]

  xnet wire format (confirmed by RE of linuxnet at VA 0x0804f360):
    241 bytes = 0x32 + 240 × 0x20
    Built as:  sprintf(buf, "2%240s", "");

  PCFlexnet variant:
    201 bytes = 0x32 + 200 × 0x20

  Both are pure `'2'` + N spaces with NO trailing bytes.  RX should
  accept any length ≥ 2 whose body after the `'2'` is all whitespace.

  Period: xnet emits a keepalive when (now - last_ka_tx) > 180000 ms
  (0x2BF20, VA 0x080507f7).  Previous spec said "~189 s" — that was
  sampling error; exact value is 180 s.

  HISTORICAL NOTE: the pre-RE spec said the trailer was 0x31 0x30 0x0D
  ("10\r") and called this "confirmed".  That was a misread of a
  monitor capture where a keepalive and the type-1 link-time frame
  that immediately followed it were concatenated into a single
  hex-dump entry — the offset field resets to 0000 at the frame
  boundary, easy to miss.  The '10\r' / '11\r' / '12\r' bytes are
  ALWAYS a separate type-1 link-time frame (see CE type-1 below),
  never part of a keepalive.


### CE type-3 — Routing data / token exchange

  Tokens: '3+\r' = request token, '3-\r' = release token. Bidirectional pairs.
  Compact records: '3' [CALLSIGN SSID RTT]+ ['+' | '-']? CR
    SSID: 0x30+N encoding. '?' RTT prefix = indirect measurement.
  Triggered at cycle boundaries (~240s), NOT on individual RTT changes.


### CE Frame Type Dispatch — REVISED (2026-04-19)

  Complete frame-type table, all CE frames dispatch by first ASCII byte
  of the payload:

    byte 0   | purpose
    ---------|-------------------------------------------------------------
    '0' (0x30)| Initial handshake (upper SSID announcement)
    '1' (0x31)| RTT-Pong / link time in milliseconds
    '2' (0x32)| RTT-Ping / keepalive null frame (241 bytes)
    '3' (0x33)| Routing tokens ('3+', '3-') and compact routing records
    '4' (0x34)| Routing-table sequence gossip  [v0.7.2 RE — CORRECTED]
              Wire: '4%u\r'  (xnet rodata 0x0808fcca, builder 0x08050760)
              Xnet increments a global 16-bit routing-seq on every dtable
              mutation; per-port tick sends '4<seq>\r' whenever our seq
              differs from the value last sent to that peer.  RX just
              stores the seq (xnet VA 0x08050515) — no reply, no echo.
              Purpose: tell the peer "routes changed, pull if you care"
              without the overhead of a full '3+' cycle.
              Previous spec entry ("Destination filter") was an initial
              guess; the actual semantic is routing-seq gossip.
    '5' (0x35)| unused / default fallthrough
    '6' (0x36)| Route REQUEST or Traceroute REQUEST  (see below)
    '7' (0x37)| Route REPLY or Traceroute REPLY      (see below)


### CE type-6 — Route / Traceroute REQUEST

  Wire format:
    '6'  HOP_BYTE  QSO_FIELD(5 bytes)  ORIGIN_CALL  ' '  TARGET_CALL

  HOP_BYTE = 0x20 + hop_count   (ASCII-offset counter)
              initial sender emits 0x20 (HopCount = 0).
              each forwarder increments before re-emitting.

  QSO_FIELD = 5 bytes, written by initial sender as sprintf("%5u", qso_id).
              right-aligned decimal with space padding ("    1", "12345").

  Route vs Traceroute is signalled by bit 0x40 of the FIRST byte of
  QSO_FIELD:
    bit 0x40 clear  →  Route request
    bit 0x40 set    →  Traceroute request
  (the initial sprintf puts ' ' (0x20) or digits in that byte; to mark
  Traceroute, the sender replaces that byte with one that has bit 0x40
  set, for example 0x60.)

  Example request (HopCount=0, QSO=1, IW2OHX-3 → IR5S):
    '6 ' + '    1' + 'IW2OHX-3' + ' ' + 'IR5S' + '\r'
    hex:  36 20 20 20 20 20 31 49 57 32 4F 48 58 2D 33 20 49 52 35 53 0D


### CE type-7 — Route / Traceroute REPLY

  Wire format (outbound from target, accumulates hops on return path):
    '7'  HOP_BYTE  QSO_FIELD(5 bytes)  ' '  HOP_1  ' '  HOP_2  ...  HOP_N

  HOP_BYTE = 0x20 + current hop count (grows as reply travels back).
  QSO_FIELD = same value as in the matching request (correlator).
  HOP_i    = callsign-with-SSID in printable form (e.g. "IW2OHX-14").

  Captured example (HopCount=4, QSO=1, path IW2OHX-14 → IR3UHU-2 → IQ5KG-7 → IR5S):
    '7$    1IW2OHX-14 IR3UHU-2 IQ5KG-7 IR5S'
    ├─ '7'         = reply
    ├─ '$' (0x24)  = HopCount byte, 0x24 - 0x20 = 4
    ├─ '    1'     = QSO number = 1
    └─ callsigns separated by single ' '

  Route vs Traceroute: same bit 0x40 convention on the first byte of
  QSO_FIELD as for the request.  A responder preserves the bit from the
  request (Route request yields Route reply; Traceroute request yields
  Traceroute reply).

  Correlation: the QSO_FIELD byte sequence is the only correlator
  between request and reply — the originator must pick a value unique
  among its in-flight queries.

---

## PID=F0 (0xF0) — AX.25 UI Beacon

  Destination: QST, period ~120s.
  Example: '"IW2OHX-14 - Digi (X)Net DLC7 di Bollate (MI)"'

---

## Link Disruption Sequence (Phase 3)

  RO FL D 11 IR3UHU-2:
    t=0.0s : DISC/UA, CE +/-, D_TABLE RTT=60000 (all immediate)
    t=7.4s : IR3UHU-2 reconnects L2, gets CE error
    t=49s  : D_TABLE over NET/ROM continues
  RO FL A 11 IR3UHU-2:
    t=31s  : L3RTT resumes | t=65s: CE reinit | t=68s: KA + '1600\r' LT
    t=410s : CE route exchange resumes

---

## Polling Cycle Structure (~240 seconds)

  [AX.25 SABM/UA] not visible in monitor +11
  ~19x L3RTT probe (LT=2) + reply (LT=1) + RR acks
  Full D-table exchange both directions (PID=CF)
  CE token exchange: 3+\r + compact records + 3-\r per direction
  [AX.25 DISC/UA]
  Between cycles: CE keepalive every 180s [v0.7.2 RE — exact value
                  0x2BF20 = 180000 ms, VA 0x080507f7; spec prev said ~189s]

---

## Routing Algorithm

  Distance-vector, metric=RTT (100ms), infinity=60000.
  Poison reverse at t=0.0s on link drop.
  Priority: destination table > link table > heard list > SSID routing.

---

## Key Constants (all confirmed)

  RTT unit              : 100ms
  RTT infinity          : 60000
  Polling interval      : ~240s
  CE keepalive (xnet)   : 241 bytes = 0x32 + 240x 0x20  [v0.7.2 RE — CORRECTED]
  CE keepalive (pcf)    : 201 bytes = 0x32 + 200x 0x20  [v0.7.2 RE]
  CE keepalive period   : 180s (exact, VA 0x080507f7)   [v0.7.2 RE — was ~189s]
  CE init frame         : 5 bytes = 0x30, 0x30+max_ssid, 0x25, 0x21, 0x0D
  CE link-time initial  : '1600\r' (600 ticks)
  CE SSID encoding      : ord(c) - 0x30 for c in '0'..'?' (0x30..0x3F)
  CE type-6 request     : '6' + HopCount byte + 5-char QSO + origin + ' ' + target
  CE type-7 reply       : '7' + HopCount byte + 5-char QSO + ' ' + hop1 ' ' ... hopN
  Traceroute selector   : bit 0x40 of first byte of QSO field (set = trace)
  CREQ Window           : always 4
  CREQ session ID       : ID field, constant for session lifetime
  T3 default            : 180000ms

---

## Investigation Status — ALL ITEMS RESOLVED

  #1  val1=0 when $M=60000, non-zero when active. CONFIRMED.
  #2  val2=0 when $M=60000, non-zero when active. val1=0 AND val2=0 = link-down. CONFIRMED.
  #3  Mystery '10\r': [v0.7.2 RE — RE-OPENED AND RESOLVED]
      Originally believed to be a TCP-split keepalive tail.  The xnet
      linuxnet RE (April 2026) proved '10\r' / '11\r' / '12\r' are
      ordinary CE type-1 link-time frames with decimal values 0/1/2
      (rodata 0x0808fc9d = "1%ld\r").  The frames are short and often
      arrive immediately after a keepalive, which earlier monitor
      captures concatenated in the hex dump — the 0000 offset reset
      at the frame boundary was the clue that was missed.  RESOLVED.
  #4  CE null frame: 0x32 + 240x 0x20 (xnet)  OR  0x32 + 200x 0x20 (pcf).
      Both variants are pure '2' + N spaces with NO trailer.
      [v0.7.2 RE — CORRECTED from "0x32 + 237x 0x20 + '10\r'"]
  #5  CE trigger: cycle-boundary token exchange. D_TABLE poison-reverse IS immediate. CONFIRMED.
  #6  CE SSID >=10: char substitution 0x30+N. 86 real-data examples confirmed. RESOLVED.
  #8  D command: CE type-6/7 request-reply for multi-hop paths. Format REVISED 2026-04-19:
      frame = TYPE + HopCount_byte (0x20 + N) + 5-char QSO field + callsign(s).
      Route vs Traceroute encoded in bit 0x40 of first byte of QSO field.
      DECODED.
  #9  CREQ/CACK + identity: AX.25 digipeater semantics. L2 source = user callsign. DECODED.
      CREQ payload: <originator> <forwarding_node>. CACK from final destination.

  Minor open: LT field in CREQ/CACK (values 11,14,17,4) — packed, exact bits TBD.
  Does not affect basic implementation.

---

## Implementation Notes (complete)

  1.  AX.25 connected session to each FlexNet neighbor
  2.  On connect: CE type-0 init (0x30, 0x30+max_ssid, 0x25, 0x21, 0x0D)
  3.  Every ~240s: ~19 L3RTT pairs (PID=CF) then full D-table exchange
  4.  After first keepalive: CE type-1 link time ('1600\r', converges down)
  5.  Every 180s (xnet) or on-demand: CE keepalive — 241 bytes (xnet)
      or 201 bytes (pcf) as single AX.25 I-frame.  Content is
      '2' + N spaces, no trailer.  [v0.7.2 RE]
  6.  CE route exchange: '3+\r' + compact records + '3-\r' per cycle
  7.  On link drop: immediately advertise RTT=60000 for all affected dests
  8.  L3RTT: val1=val2=0 when no reply, non-zero when active
  9.  Distance-vector routing, metric=RTT, infinity=60000, poison reverse
  10. TYPE=8, ALIAS required, LT=2 if L3RTT initiator
  11. D command (multi-hop): CE type-6 to next-hop, await type-7 reply
  12. D command (direct): local D-table lookup, no frames
  13. CREQ: originator=user_callsign, forwarding=local_callsign, Window=4
  14. Identity: L2 source = user callsign. Intermediate nodes = AX.25 digipeaters.
  15. CE SSID parser: ssid = ord(c) - 0x30, for c in '0'..'?'
  16. Do NOT reconstruct keepalive from telnet monitor hex (TCP splits it)

---

## Source Documents

  - DCC1995-FlexNet-DK7WJ-N2IRZ.pdf : conference paper by FlexNet author DK7WJ
  - xnet138.pdf : (X)Net 1.38 sysop manual (German)
  - RMNC_FlexNet_and_PC_FlexNet.html : English sysop manual excerpt
  - flexnet1.pdf : Italian FlexNet overview (1995)
  - flexnet_capture_1h.json : 1h live capture, IW2OHX-14, April 2026
  - phase1_results.json : clean ASCII capture (monitor -x +11), April 2026
  - phase2_results.json : CE frame classification analysis, April 2026
  - phase3_results.json : link disruption capture (RO FL D/A), April 2026
  - phase4_monitor.txt : D command + user session capture, April 2026
