# FlexNet / (X)Net Protocol — Findings
# Based on live captures from IW2OHX-14 <-> IR3UHU-2 and IW2OHX-14 <-> IW2OHX-13
# Initial capture: 2026-04-10  |  Phase 1/2 captures: 2026-04-13  |  Phase 3: 2026-04-13
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

  All fields space-separated on one logical line. Join all body lines before parsing.

  Fields (confirmed from Phase 1 clean ASCII capture + Phase 3 disruption test):

    counter     : monotonic millisecond tick counter (10-digit decimal)
                  Increments ~500 per probe (~500ms between probes)
                  Used as the RTT echo token: B echoes A's counter back,
                  A measures wall-clock elapsed time -> $M value

    val1        : VARIABLE numeric field — CONFIRMED SEMANTICS (Phase 3):
                  val1=0 when probe received NO REPLY ($M=60000, link down)
                  val1>0 when link is active — value varies each probe cycle
                  Observed range: 0, 3, 4, 5 (varies with link quality/load)
                  Hypothesis: count of active FlexNet links on the sending node,
                  or a rolling link-quality counter. Zero = isolated node.

    val2        : LOW-RANGE numeric field — CONFIRMED SEMANTICS (Phase 3):
                  val2=0 when probe received NO REPLY ($M=60000, link down)
                  val2>0 when link is active — value varies each probe cycle
                  Observed range: 0, 2, 3, 4, 5
                  Hypothesis: similar to val1, possibly active neighbor count
                  or a different quality metric. Zero = isolated node.

                  KEY RULE (confirmed from Phase 3): val1=0 AND val2=0
                  together are a RELIABLE indicator that the sending node
                  received no L3RTT reply on this link ($M=60000).
                  They are never simultaneously zero during normal operation.

    node_serial : Fixed decimal integer, node-specific hardware identifier
                  Different per node alias:
                    NETUHU (IR3UHU-2): 3076478712
                    BOLNET (IW2OHX-14): 67717840
                  Fixed for the lifetime of the node installation.
                  Set at Xnet install time or derived from hardware.

    NODE_ALIAS  : 6-char node name (e.g. NETUHU, BOLNET)
                  Alias of the NODE SENDING the probe, not the receiver.

    $M<rtt>     : Measured RTT in 100ms units
                  $M=60000 = probe failed (link down or no reply)
                  $M=19    = 1.9s RTT (typical for AXUDP tunnel)
                  $M=6     = 0.6s RTT (good RF link)

    $N          : End-of-record sentinel token

  LT field (in L3 frame header, not payload):
    LT=2 : this node initiated the polling session
    LT=1 : this node is the responder
    ~4% exceptions at link recovery events

  RTT echo mechanism:
    1. Node A sends L3RTT with counter=C1 (LT=2)
    2. Node B echoes counter=C1 back (LT=1)
    3. Node A measures elapsed time -> $M
    4. Both nodes use this $M as link cost in D-table advertisements


### D_TABLE — Destination table exchange (NET/ROM PID)

  Monitor format : CALLSIGN-SSID   RTT/SSID_MAX  [PORT[TYPE] 'ALIAS']
  D command format: CALLSIGN  SSID_LO-SSID_HI  RTT  (4 entries per line)

  Fields:
    RTT=60000  : distance-vector infinity / poison reverse
    PORT       : 0=RF, 1=HAMNET/AMPRNet, 16-23=AXIP/AXUDP tunnel links
    TYPE       : 8=Xnet/DAMA, 7=FlexNet, 6=BPQ, 5=JNOS, 4=legacy
    ALIAS      : 6-char name, only present for aliased FlexNet nodes

  Multi-port example: VA3BAL  39/6  0[7] 'XRBAL'  1[7] '44.135.92.2/32'  19[4] ?

  D_TABLE during link disruption (Phase 3 observation):
    When the FlexNet link to IR3UHU-2 was removed (RO FL D 11 IR3UHU-2),
    IW2OHX-14 immediately sent a D_TABLE to IR3UHU-2 advertising all
    destinations with RTT=60000 (poison reverse for that link).
    IR3UHU-2 continued sending its full D_TABLE to IW2OHX-14 on the
    next polling cycle (~49s later) — 73 D_TABLE frames observed during
    the DISABLED phase via the still-active NET/ROM session.


### User session transit frames (CREQ/CACK/DREQ/DACK/I/IACK)

  ~20 CREQ + ~85 I frames per hour as transit traffic.
  Full payload structure: NOT YET DECODED (Phase 4 investigation item).

---

## PID=CE (0xCE) — Native FlexNet Protocol

### CE type-0 — Link setup / init handshake — NEWLY DECODED (Phase 3)

  5-byte frame carrying SSID range and capability flags.
  Sent immediately when a FlexNet AX.25 session is established.

  Wire format:
    Byte 0: 0x30           (init handshake marker, always 0x30)
    Byte 1: 0x30 + max_ssid (upper SSID bound: 0x3E = 14, 0x32 = 2)
    Byte 2: 0x25           (capability flags)
    Byte 3: 0x21           (capability flags)
    Byte 4: 0x0D           (CR terminator)

  Examples observed:
    IW2OHX-14 -> IR3UHU-2:  0x30 0x3E 0x25 0x21 0x0D  (max_ssid=14)
    IW2OHX-14 -> IW2OHX-13: 0x30 0x32 0x25 0x21 0x0D  (max_ssid=2)

  Error response from Xnet when link not configured:
    '02%! FlexLink:No Link to 11:IR3UHU-2 defined'
    Observed when IR3UHU-2 tries to reconnect after RO FL D.

  Init message on successful reconnect:
    '02%! FlexLink:Init fm IR3UHU-2/2 (25 21)'
    Indicates Xnet accepted the link setup after RO FL A.

  IMPORTANT: Byte 0 MUST always be 0x30 (not 0x30+min_ssid).
    If min_ssid=3, byte 0 would be 0x33 which Xnet interprets as
    a routing data frame (type '3'), breaking the handshake.


### CE type-1 — Link time measurement — NEWLY DECODED (Phase 3)

  Reports link delay in 100ms ticks. Sent after keepalive exchange.

  Format: '1' <decimal_integer> '\r'
  Example: '1600\r' = 600 ticks = 60,000ms = initial/infinity value
  Example: '12\r'   = 2 ticks   = 200ms delay (normal)

  Observed during Phase 3 recovery:
    At +68.6s after RO FL A: IR3UHU-2 sends '1600\r' (initial value)
    Value converges from 600 down to single digits over several cycles.
    The neighbor uses this to compute the Q/T link quality metric.


### CE type-2 — Keepalive / null frame — FULLY CONFIRMED

  Total length: 241 bytes (fixed)
  Structure:
    Byte 0        : 0x32 ('2')
    Bytes 1-237   : 0x20 (space, 237 bytes)
    Bytes 238-240 : 0x31 0x30 0x0D ('10' + CR)

  Period: ~189s average.
  Implementation: b'\x32' + b'\x20'*237 + b'\x31\x30\x0d' as single AX.25 I-frame.

  TCP DELIVERY SPLIT: when viewed via telnet monitor, the 241-byte frame
  arrives in two TCP chunks. The '10\r'/'11\r'/'12\r' tail fragments
  seen in captures are NOT separate protocol messages — they are keepalive
  tails orphaned by TCP segmentation. AX.25 delivers the frame atomically.


### CE type-3 — Routing data / token exchange

  Token signals (3 bytes):
    '3+\r' (0x33 0x2B 0x0D) : request token / ready to send routes
    '3-\r' (0x33 0x2D 0x0D) : release token / end of routing batch

  These always appear in matched pairs (one per direction, same second).
  During Phase 3 POST phase: 8 pairs observed — high route churn as
  network reconverges after link restoration.

  Compact routing records:
    Format: '3' [CALLSIGN SSID RTT]+ ['+' | '-']? CR
    SSID: 1-2 digits immediately following callsign
    RTT: decimal, 100ms units; '?' prefix = indirect measurement

  CE ROUTING TRIGGER CONDITIONS — CONFIRMED (Phase 3):
    CE compact records are sent at polling cycle boundaries (~240s),
    NOT as real-time triggered updates on RTT changes.
    No CE routing frames observed in the first 30s after link drop.
    The fast-convergence mechanism is the D_TABLE poison-reverse (t=0.0s),
    not CE compact frames.

---

## PID=F0 (0xF0) — AX.25 UI Beacon

  Destination: QST, period: ~120s, purpose: node identification.

---

## Link Disruption Sequence (Phase 3 — confirmed from live capture)

  Command issued: RO FL D 11 IR3UHU-2

  t= 0.0s : DISC+    IW2OHX-14->IR3UHU-2    AX.25 L2 disconnect (immediate)
  t= 0.0s : UA-      IR3UHU-2->IW2OHX-14    disconnect acknowledged
  t= 0.0s : CE +/-   both directions         token exchange at cycle boundary
  t= 0.0s : D_TABLE  IW2OHX-14->IR3UHU-2    poison-reverse: all RTT=60000
  t= 0.0s : CE init  IW2OHX-14->IR3UHU-2    IW2OHX-14 tries CE re-init
  t= 7.4s : SABM+    IR3UHU-2->IW2OHX-14    IR3UHU-2 tries to reconnect
  t= 7.4s : UA-      IW2OHX-14->IR3UHU-2    L2 accepted (NET/ROM still works)
  t= 7.5s : CE err   IR3UHU-2->IW2OHX-14    'FlexLink:No Link to 11:IR3UHU-2 defined'
  t=49.0s : D_TABLE  IR3UHU-2->IW2OHX-14    IR3UHU-2 sends full table next cycle
  t=197s  : CE err   IR3UHU-2->IW2OHX-14    IR3UHU-2 retries init, error again

  Command issued: RO FL A 11 IR3UHU-2

  t=31s   : L3RTT    IW2OHX-14->IR3UHU-2    LT=2, new polling cycle starts
  t=65s   : CE init  IR3UHU-2->IW2OHX-14    'FlexLink:Init fm IR3UHU-2/2 (25 21)'
  t=65s   : CE init  IW2OHX-14->IR3UHU-2    0x30 0x3E 0x25 0x21 0x0D confirm
  t=68s   : CE KA    IW2OHX-14->IR3UHU-2    first keepalive of restored session
  t=68s   : CE LT    IR3UHU-2->IW2OHX-14    '1600\r' (initial link-time value)
  t=410s  : CE +/-   both directions         route exchange resumes, reconverging

  Key observations:
    - AX.25 disconnect is IMMEDIATE on RO FL D (t=0.0s)
    - IR3UHU-2 retries within 7.4s — NET/ROM L2 accepted, CE rejected
    - Poison-reverse D_TABLE is sent at t=0.0s (not deferred to next cycle)
    - D_TABLE exchange over NET/ROM continues throughout FlexNet outage
    - CE native session reinitialises within ~65s of RO FL A
    - Link-time starts at 600 (RTT_INFINITY) and converges down over cycles

---

## Polling Cycle Structure (~240 seconds / 4 minutes)

  [AX.25 SABM/UA] — not visible in monitor +11 (PID-less L2, filtered out)
  ~19x L3RTT probe (LT=2) + reply (LT=1) + RR acks
  Full D-table exchange both directions (~32+29 I-frames, PID=CF)
  CE token exchange: 3+\r + compact records + 3-\r per direction
  [AX.25 DISC/UA]
  Between cycles: CE keepalive every ~189s

  1h capture statistics: 21 L3RTT, 22 D-TABLE, 184 RR,
  140 CE (43 keepalive, 93 routing, 4 status), 0 SABM/DISC (filtered)

---

## Routing Algorithm

  Type: Distance-vector, metric=RTT (100ms units), infinity=60000
  Full table every ~4 minutes + CE token-gated route exchange per cycle
  Poison reverse: RTT=60000 advertised immediately on link drop (t=0.0s)

  Priority order (DCC1995):
    1. Destination table (autorouter, RTT-based)
    2. Link table (sysop-configured)
    3. Heard list (last 200 callsigns)
    4. SSID routing

---

## Key Constants (confirmed)

  RTT unit              : 100ms
  RTT infinity          : 60000
  Polling interval      : ~240s
  Probes per cycle      : ~19 per direction
  CE keepalive period   : ~189s average
  CE keepalive length   : 241 bytes = 0x32 + 237x 0x20 + 0x31 0x30 0x0D
  CE init frame         : 5 bytes = 0x30, 0x30+max_ssid, 0x25, 0x21, 0x0D
  CE link-time initial  : '1600\r' (600 = RTT_INFINITY ticks)
  Beacon period         : ~120s (F0 UI to QST)
  T3 default (Xnet)     : 180000ms

---

## Open Investigation Items (updated 2026-04-13 after Phase 3)

  RESOLVED:
    #3  Mystery '10\r' frames: TCP split of keepalive tail. Not a protocol msg.
    #4  CE null frame layout: 0x32 + 237x 0x20 + '10\r' — confirmed.
    #5  CE routing trigger: cycle-boundary token exchange, not immediate RTT.
        Poison-reverse D_TABLE IS immediate (t=0.0s on link drop).
    #1  val1: 0 when $M=60000 (no reply), non-zero when active. Confirmed.
    #2  val2: 0 when $M=60000 (no reply), non-zero when active. Confirmed.
        val1=0 AND val2=0 = reliable "link down / no L3RTT reply" indicator.

  NEW DISCOVERIES (Phase 3):
    CE type-0 init: 5-byte frame, byte1=0x30+max_ssid, caps 0x25 0x21
    CE type-1 link time: '1<ticks>\r', initial=600 (RTT_INFINITY), converges
    Xnet error on missing link: '02%! FlexLink:No Link to N:CALL defined'
    IR3UHU-2 reconnects within 7.4s of link drop (NET/ROM L2 accepted)
    D_TABLE exchange over NET/ROM continues throughout CE outage

  STILL OPEN:
    #6  CE SSID boundary for SSID>=10: untested, low risk.
    #8  D command path trace: issue D while monitor runs, observe wire.
    #9  CREQ/CACK user session + identity preservation: Phase 4, critical.

---

## Xnet Internal Processes

  FlexRTT, FlexLink, INP, Link, RxNRBC

---

## Implementation Notes

  1.  AX.25 connected session to each FlexNet neighbor (L2)
  2.  On connect: send CE type-0 init (0x30, 0x30+max_ssid, 0x25, 0x21, 0x0D)
  3.  Every ~240s: ~19 L3RTT pairs (PID=CF) then full D-table exchange
  4.  After first keepalive: send CE type-1 link time ('1600\r', converges down)
  5.  Every ~90s: CE keepalive — 241 bytes as single AX.25 I-frame
  6.  CE route exchange: '3+\r' + compact records + '3-\r' per cycle
  7.  On link drop: immediately advertise RTT=60000 for all affected destinations
  8.  L3RTT payload: val1=val2=0 when no reply, non-zero when active
  9.  Distance-vector, metric=RTT, infinity=60000, poison reverse
  10. TYPE=8, ALIAS required for named nodes, LT=2 if initiator
  11. Do NOT reconstruct keepalive bytes from telnet monitor hex (TCP splits it)

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
