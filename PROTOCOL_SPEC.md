# FlexNet / (X)Net Protocol — Reverse Engineering Findings
# Based on live captures from IW2OHX-14 <-> IR3UHU-2 and IW2OHX-14 <-> IW2OHX-13
# Initial: 2026-04-10 | Phase 1/2: 2026-04-13 | Phase 3: 2026-04-13 | Phase 4: 2026-04-13
# Author: IW2OHX + Claude Sonnet 4.6
# Verified against: DCC1995 paper (DK7WJ/N2IRZ), xnet138.pdf, RMNC_FlexNet.html
# Status: ALL INVESTIGATION ITEMS RESOLVED
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


### Identity preservation — CONFIRMED (Phase 4)

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

  The CREQ L3 payload also carries the originator explicitly:
    CREQ payload: <originating_callsign> <forwarding_node>

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


### CE type-2 — Keepalive / null frame (confirmed)

  241 bytes: 0x32 + 237x 0x20 + 0x31 0x30 0x0D. Period ~189s.
  Send as single AX.25 I-frame: b'\x32' + b'\x20'*237 + b'\x31\x30\x0d'
  TCP SPLIT: telnet monitor delivers in two chunks. '10\r'/'11\r'/'12\r'
  tail fragments are NOT separate messages -- keepalive tails only.


### CE type-3 — Routing data / token exchange

  Tokens: '3+\r' = request token, '3-\r' = release token. Bidirectional pairs.
  Compact records: '3' [CALLSIGN SSID RTT]+ ['+' | '-']? CR
    SSID: 0x30+N encoding. '?' RTT prefix = indirect measurement.
  Triggered at cycle boundaries (~240s), NOT on individual RTT changes.


### CE type-6 — D command path query (Phase 4)

  Multi-hop destinations only. Direct neighbors = local lookup, no frame sent.
  Format: '6' <flags> '    ' <seq> <originator> ' ' <next-hop> ' ' <dest> '\r'
  flags='!', length ~30 bytes.


### CE type-7 — D command path reply (Phase 4)

  Format: '7' <flags> '    ' <seq> <originator> ' ' <next-hop> [' ' <hop>...] ' ' <dest> '\r'
  flags='$', length ~38+ bytes. Each hop appends itself.
  Example: '7$    1IW2OHX-14 IR3UHU-2 IQ5KG-7 IR5S'
  Path to IR5S: IW2OHX-14 -> IR3UHU-2 -> IQ5KG-7 -> IR5S

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
  Between cycles: CE keepalive every ~189s

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
  CE keepalive          : 241 bytes = 0x32 + 237x 0x20 + 0x31 0x30 0x0D
  CE init frame         : 5 bytes = 0x30, 0x30+max_ssid, 0x25, 0x21, 0x0D
  CE link-time initial  : '1600\r' (600 ticks)
  CE SSID encoding      : ord(c) - 0x30 for c in '0'..'?' (0x30..0x3F)
  CE type-6 query       : ~30 bytes, flags='!'
  CE type-7 reply       : ~38+ bytes per hop, flags='$'
  CREQ Window           : always 4
  CREQ session ID       : ID field, constant for session lifetime
  T3 default (Xnet)     : 180000ms

---

## Investigation Status — ALL ITEMS RESOLVED

  #1  val1=0 when $M=60000, non-zero when active. CONFIRMED.
  #2  val2=0 when $M=60000, non-zero when active. val1=0 AND val2=0 = link-down. CONFIRMED.
  #3  Mystery '10\r': TCP split of keepalive tail. Not a protocol message. RESOLVED.
  #4  CE null frame: 0x32 + 237x 0x20 + '10\r'. CONFIRMED.
  #5  CE trigger: cycle-boundary token exchange. D_TABLE poison-reverse IS immediate. CONFIRMED.
  #6  CE SSID >=10: char substitution 0x30+N. 86 real-data examples confirmed. RESOLVED.
  #8  D command: CE type-6/7 query-reply for multi-hop. Direct = local lookup only. DECODED.
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
  5.  Every ~90s: CE keepalive — 241 bytes as single AX.25 I-frame
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
