# FlexNet / (X)Net Protocol — Reverse Engineering Findings
# Based on live captures from IW2OHX-14 <-> IR3UHU-2 and IW2OHX-14 <-> IW2OHX-13
# Initial capture: 2026-04-10  |  Phase 1/2 captures: 2026-04-13
# Author: IW2OHX + Claude Sonnet 4.6
# Verified against: DCC1995 paper (DK7WJ/N2IRZ), xnet138.pdf, RMNC_FlexNet.html
# =======================================================================

## CRITICAL: PID assignments (confirmed from xnet138.pdf section 4.3.12)

  PID=CE (0xCE) = FlexNet native protocol
  PID=CF (0xCF) = NET/ROM protocol
  PID=F0 (0xF0) = Plain AX.25 / UI frames

  NOTE: (X)Net speaks BOTH FlexNet (CE) and NET/ROM (CF) simultaneously.
  What appears in the monitor as "pid CF" frames are NET/ROM-compatible
  routing frames. The "pid CE" frames are native FlexNet.

---

## PID=CF (0xCF) — NET/ROM-compatible Routing Layer

### L3RTT — Round-trip time probe

  Payload (ASCII, single long line, may wrap at ~80 chars in monitor output):

    L3RTT: <counter>  <val1>  <val2>  <node_serial>  <NODE_ALIAS>  LEVEL3_V2.1  (X)NET<ver>  $M<rtt>  [$N]

  All fields are space-separated on one logical line. The monitor may
  wrap this at terminal width — join all body lines before parsing.

  Fields confirmed from Phase 1 clean ASCII capture (monitor -x +11):

    counter     : monotonic millisecond tick counter (10-digit decimal)
                  Increments ~500 per probe (~500ms between probes)
                  Used as the RTT echo token: B echoes A's counter back,
                  A measures wall-clock elapsed time -> $M value

    val1        : VARIABLE numeric field, varies per probe exchange
                  Seen values: 2, 13, 14, 16, 17, 20, 83
                  When $M=60000 (link down): val1=2 consistently
                  When link is active: val1 varies (possibly last RTT sample)
                  NEEDS FURTHER INVESTIGATION to confirm semantics

    val2        : LOW-RANGE numeric field
                  Seen values: 0, 2, 3, 4
                  When $M=60000 (link down): val2=0 consistently
                  When link active: val2 = 2, 3, or 4
                  Likely: neighbor count, port count, or SSID-max for this link
                  NEEDS FURTHER INVESTIGATION to confirm semantics

    node_serial : Fixed decimal integer, node-specific hardware identifier
                  Different per node alias:
                    NETUHU (IR3UHU-2): 3076478712
                    BOLNET (IW2OHX-14): 67717840
                  Appears fixed for the lifetime of the node installation

    NODE_ALIAS  : 6-char node name (e.g. NETUHU, BOLNET)
                  This is the alias of the NODE SENDING the probe, not
                  the receiving node. Confirms whose RTT is being measured.

    $M<rtt>     : Measured RTT in 100ms units
                  $M=60000 = probe failed (link down or no reply)
                  $M=19 = 1.9 seconds RTT (typical for AXUDP tunnel)
                  $M=6  = 0.6 seconds RTT (good RF link)

    $N          : End-of-record sentinel token

  LT field (in the L3 frame header, not the payload):
    LT=2 : this node initiated the polling session (sent first probe)
    LT=1 : this node is the responder (echoing the probe back)
    ~4% exceptions: link recovery events where roles briefly swap

  RTT echo mechanism:
    1. Node A sends L3RTT with counter=C1 (LT=2)
    2. Node B echoes counter=C1 back in reply (LT=1)
    3. Node A computes elapsed time -> stores as $M
    4. Both nodes carry this $M in subsequent frames as link cost


### D_TABLE — Destination table exchange (NET/ROM PID)

  Monitor format: CALLSIGN-SSID   RTT/SSID_MAX  [PORT[TYPE] 'ALIAS']
  D command format: CALLSIGN  SSID_LO-SSID_HI  RTT  (4 entries per line)

  Fields:
    RTT        : route cost in 100ms units
    SSID_MAX   : highest SSID served by this node entry
    PORT       : 0=RF, 1=HAMNET/AMPRNet, 16-23=AXIP/AXUDP tunnel links
    TYPE       : node capability level (8=Xnet/DAMA, 7=FlexNet, 6=BPQ, 5=JNOS, 4=legacy)
    ALIAS      : 6-char node name, ONLY present for aliased FlexNet nodes
    RTT=60000  : distance-vector infinity / poison reverse

  Multi-port example: VA3BAL  39/6  0[7] 'XRBAL'  1[7] '44.135.92.2/32'  19[4] ?


### User session transit frames (CREQ/CACK/DREQ/DACK/I/IACK)

  ~20 CREQ + ~85 I frames per hour observed as transit traffic.
  Full payload structure: NOT YET DECODED (Phase 4 investigation item).

---

## PID=CE (0xCE) — Native FlexNet Protocol

### CE keepalive / null frame — FULLY CONFIRMED

  Total length : 241 bytes (fixed, always exactly 241)
  Structure:
    Byte 0        : 0x32 ('2')
    Bytes 1-237   : 0x20 (space, 237 bytes)
    Bytes 238-240 : 0x31 0x30 0x0D ('10' + CR)

  Period : ~189 seconds average (nominal target ~90-100s)
  Purpose: prevent AX.25 L2 T3 inactivity timer from expiring between cycles.

  IMPORTANT: TCP DELIVERY SPLIT (confirmed from Phase 1/2 captures)
    The 241-byte frame arrives at the telnet monitor client split into TWO
    TCP chunks (Nagle / buffer boundaries):
      Chunk 1: '2' + N spaces  (190-231 bytes observed)  -> one monitor frame
      Chunk 2: remaining spaces + '10
'                 -> separate monitor frame
    The '10
', '11
', '12
' frames seen in captures are all keepalive tails.
    They are NOT separate protocol messages.
    AX.25 delivers the full 241 bytes atomically — the split is monitor-only.

  IMPLEMENTATION: send exactly 241 bytes as a single AX.25 I-frame:
    b'\x32' + b'\x20' * 237 + b'\x31\x30\x0d'


### CE compact routing records — triggered updates

  Format: '3' [CALLSIGN SSID RTT]+ ['+' | '-']? CR
  Example: '3KP3FT 112714 K1YMI 0?2786\r'
    '3'     : subtype identifier
    CALLSIGN: no hyphen, SSID follows immediately as 1-2 digits
    RTT     : decimal, 100ms units
    '?'     : indirect RTT (not directly measured)
    '+'/'-' : route improvement / withdrawal suffix

  Trigger conditions: NOT YET CONFIRMED (Phase 3 item)
  Statistics from 1h capture: 93 routing update frames total


### CE status frames (3 bytes)

  '3+\r' (0x33 0x2B 0x0D) : positive status / route improvement ACK
  '3-\r' (0x33 0x2D 0x0D) : negative status / route withdrawal

---

## PID=F0 (0xF0) — AX.25 UI Beacon

  Destination: QST, period: ~120s, purpose: node identification

---

## Polling Cycle (~240 seconds)

  [SABM/UA — not visible in monitor +11, filtered as PID-less L2 frames]
  ~19x L3RTT probe (LT=2) + reply (LT=1) + RR
  Full D-table exchange both directions (~32+29 I-frames)
  [DISC/UA]
  Between cycles: triggered CE updates + CE keepalive every ~189s

  1h capture statistics (two active neighbors):
    21 L3RTT, 22 D-TABLE, 184 RR, 140 CE (43 keepalive, 93 routing, 4 status)
    0 SABM/DISC (filtered by monitor)

---

## Routing Algorithm

  Type: Distance-vector, metric = RTT in 100ms units, infinity = 60000
  Full table exchange every ~4 minutes + triggered CE updates

  Priority order (DCC1995):
    1. Destination table (autorouter, RTT-based)
    2. Link table (sysop-configured)
    3. Heard list (last 200 callsigns)
    4. SSID routing

  Poison reverse: advertise RTT=60000 for destinations reachable only via
  the recipient neighbor — prevents routing loops.
  81% of D-table entries alternate between 60000 and real RTT.

---

## Key Constants (confirmed)

  RTT unit              : 100ms
  RTT infinity          : 60000
  Polling interval      : ~240s
  Probe interval        : ~7.5s within polling session
  Probes per cycle      : ~19 per direction
  CE keepalive period   : ~189s average
  CE keepalive length   : 241 bytes
  CE keepalive bytes    : 0x32 + 237x 0x20 + 0x31 0x30 0x0D
  T3 default (Xnet)     : 180000ms

---

## Open Investigation Items (updated 2026-04-13)

  RESOLVED:
    #3 Mystery '10\r' frames: TCP split of keepalive tail. Not a protocol message.
    #4 CE null frame layout: 0x32 + 237x 0x20 + 0x31 0x30 0x0D — confirmed.

  OPEN:
    #1 val1 in L3RTT: variable, val1=2 when link down. Semantics TBD.
    #2 val2 in L3RTT: range 0-4, val2=0 when link down. Semantics TBD.
    #5 CE routing update triggers: Phase 3 (deliberate link disruption test).
    #6 CE SSID boundary for SSID>=10: parser untested, low risk.
    #8 D command path trace: issue D while monitor runs, observe wire.
    #9 CREQ/CACK user session + identity preservation: Phase 4, critical.

---

## Xnet Internal Processes

  FlexRTT, FlexLink, INP, Link, RxNRBC

---

## Implementation Notes

  1. AX.25 connected session to each FlexNet neighbor
  2. Every ~240s: ~19 L3RTT pairs (PID=CF) then full D-table exchange
  3. Between cycles: CE compact updates for RTT changes (threshold TBD)
  4. Every ~90s: CE keepalive — 241 bytes, single AX.25 I-frame
  5. Distance-vector routing, metric=RTT, infinity=60000, poison reverse
  6. TYPE=8, ALIAS required for named nodes
  7. LT=2 if initiator, LT=1 if responder
  8. Do NOT reconstruct keepalive bytes from telnet monitor hex — TCP splits it

---

## Source Documents

  - DCC1995-FlexNet-DK7WJ-N2IRZ.pdf : conference paper by FlexNet author DK7WJ
  - xnet138.pdf : (X)Net 1.38 sysop manual (German)
  - RMNC_FlexNet_and_PC_FlexNet.html : English sysop manual excerpt
  - flexnet1.pdf : Italian FlexNet overview (1995)
  - flexnet_capture_1h.json : 1h live capture, IW2OHX-14, April 2026
  - phase1_results.json : clean ASCII capture (monitor -x +11), April 2026
  - phase2_results.json : CE frame classification analysis, April 2026
