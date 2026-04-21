# xnet / linuxnet — Reverse-Engineering Findings

**Target:** `xnet/linuxnet` (ELF 32-bit LSB, Intel 80386, stripped, 368164 bytes)
**Build:** `(X)NET by DL1GJI, LEVEL3_V2.1` — `Compiled: Sep 20 2005 10:47:13`
**Author's signature (from help text):** `(c) Jimy, DL1GJI`
**Work date:** 2026-04-21
**Method:** Capstone disassembly + string/xref analysis + cross-reference with live `flexnet_capture_1h.json` and `flexnet_capture_port1.json`
**Purpose:** Establish ground-truth xnet FlexNet protocol behaviour, correct any errors in `PROTOCOL_SPEC.md`, and give `flexnetd` an accurate baseline.

---

## 1. Binary layout

| Section   | VA start     | Size    | File offset | Notes                              |
|-----------|--------------|---------|-------------|------------------------------------|
| `.text`   | `0x080494a0` | 284208  | `0x000014a0` | Main code                          |
| `.rodata` | `0x0808eb00` | 49600   | `0x00046b00` | Format strings + dispatch tables   |
| `.data` / `.bss` | 0x080a0000+ | — | — | Globals including port tables       |

### Key globals

| Address       | Meaning                                                        |
|---------------|----------------------------------------------------------------|
| `0x080a1908`  | Port config table — 20 slots × 48 bytes = 960 bytes            |
| `0x080a1cc8`  | Routing table base ptr (entries are 48 bytes each)             |
| `0x080a1ccc`  | Destination count (dword)                                      |
| `0x080a1cd4`  | Routing-table mutex (used by `0x8053ad0`/`0x8053af0`/`0x8053b00`/`0x8053b40`) |
| `0x080a1d00`  | Per-port runtime state — 20 slots × 36 bytes = 720 bytes       |
| `0x080a1fd0`  | **Global routing-table sequence number** (word) — increments every route change |
| `0x080b3ca8`  | Pointer to own node info (callsign at +0, alias at +9)         |

### Per-port runtime state (at `0x80a1d00 + port_idx * 36`)

| Offset | Width | Meaning                                                     |
|--------|-------|-------------------------------------------------------------|
| `+0x00` | byte  | CE session state: 1=idle, 2=got 3+, 3=got 3-, 4=sent 3+   |
| `+0x01` | byte  | ? (checked against 3 in `0x0804fe01`)                      |
| `+0x04` | dword | Destination-iteration index (round-robin, wraps mod N)     |
| `+0x08` | dword | Smoothed RTT toward peer, in internal 10-ms ticks. Initial 60000 (infinity). |
| `+0x0C` | dword | Peer's latest reported RTT (type-1 value × 100)            |
| `+0x10` | dword | Tick stamp used in L3RTT delta accounting                  |
| `+0x14` | dword | Tick stamp of last keepalive TX                            |
| `+0x18` | dword | Tick stamp of last '3+' TX (timeout at +360000 ticks)      |
| `+0x1C` | dword | Our most-recent measured delta (RTT/2 estimate)            |
| `+0x20` | word  | Peer's last advertised seq number (from type-4 RX)         |
| `+0x22` | word  | Seq value last sent in our own type-4 TX                    |

---

## 2. CE frame dispatcher (RX)

`0x08050250` parses an incoming AX.25 I-frame whose PID is `0xCE`. After null-terminating the payload at byte `data[len-2]`, the first byte is the type character:

```
al = byte[payload]
al += 0xD0                     ; ASCII digit -> 0..F
movsx eax, al
cmp eax, 7 ; ja 0x8050555      ; unknown -> logger
jmp dword[eax*4 + 0x808fca4]
```

### Jump table at `.rodata 0x0808fca4`

| Type | Handler VA     | Role                                                    |
|------|----------------|---------------------------------------------------------|
| 0    | `0x080502a8`   | CE init handshake. Stores peer `max_ssid = byte[1]`. Prints `"Init fm %a/%d (%02x %02x)"` (debug) |
| 1    | `0x08050310`   | **Link-time measurement RX** — parse decimal, update smoothed RTT |
| 2    | `0x080503f2`   | **Keepalive RX** — reply with current `"1%ld\r"`, then check '3+' |
| 3    | `0x08050436`   | **Routing / compact records** + token '+'/'-'          |
| 4    | `0x08050515`   | **Seq sync** — store parsed decimal at `word[row+0x20]` |
| 5    | `0x08050555`   | *Unhandled* — logs `"%s"` and drops the frame          |
| 6    | `0x08050530`   | Path-request (delegates to `0x0804fbb0`)               |
| 7    | `0x08050540`   | Path-reply (delegates to `0x0804fa00`, but first increments `byte[frame+1]` — TTL) |

### CE send helper `0x0804f1e0`

Shared send path: allocates buffer, sets `buf[0x57] = 0xCE`, `memcpy`'s the null-terminated payload to `buf+0x58`, sets length, enqueues via `0x0805c520`.

---

## 3. Frame formats — confirmed wire formats

### Type 0 — Link setup (5 bytes)

```
0x30, (0x30 + max_ssid), 0x25, 0x21, 0x0D
```

Matches spec.

### Type 1 — Link-time / RTT

- **TX format:** `sprintf(buf, "1%ld\r", value)` — rodata `0x0808fc9d`.
- **Computed value (type-2 reply path at `0x080503f2`):**
  ```
  value = smoothed_ticks / 100        ; fast-div by 100 via mul 0x51eb851f; shr eax,5
  ```
  Smoothed value is in internal 10-ms ticks, so on-wire unit is seconds.
- **RX (at `0x08050310`):**
  ```
  delta_since_last = now_ticks - [row+0x10]      ; save to [row+0x1C]
  value*100 -> [row+0x0C]                         ; peer's reported RTT
  if smoothed[row+8] == 0xEA60 (60000): init = delta
  smoothed = (15*smoothed + new_delta + 8) >> 4  ; IIR filter with alpha=1/16
  if delta > 0xEA5F (59999): fail path, abort
  ```

**Observed on wire:** `"1600\r"`, `"12\r"`, `"10\r"`, `"11\r"` — these are SEPARATE frames, not keepalive tails.

### Type 2 — Keepalive (the big PROTOCOL_SPEC correction)

- **TX format:** `sprintf(buf, "2%240s", "")` — rodata `0x0808fb47` + null arg at `0x0808fb46`.
- **Result:** `0x32 + 240 × 0x20` = **241 bytes, ALL spaces after the leading '2'. No `10\r` / `11\r` / `12\r` trailer.**
- **Builder:** `0x0804f360`
- **Trigger:** `0x080507b0` → `0x080505a0` when `(now - [row+0x14]) > 0x2BF20` (180000 ms = **180 s**, not 189 s).
- **TX timestamping:** after send, `[row+0x14] = now_ticks`.

### Evidence (monitor capture `flexnet_capture_1h.json`)

A keepalive reported as `framelen = 241` but whose hex dump appeared to end in `20 31 32 0D` was examined directly:

```
0000 32 20 20 ... 20             (241 bytes of '2' + spaces)
0000 31 32 0D                     (NEW frame offset restarts — this is a type-1)
```

The monitor hex-pretty-printer concatenated two back-to-back AX.25 frames into one entry, with the offset restarting to `0000` at the boundary. The spec's earlier claim that keepalives end with `31 30 0D`/`31 31 0D`/`31 32 0D` was a misreading of that offset restart.

### Type 2 — pcf keepalive variant

PCFlexnet uses a DIFFERENT keepalive length: **201 bytes = `'2' + 200 × 0x20`**. Confirmed from `flexnet_capture_port1.json` — frame `IW2OHX-12 -> IW2OHX-14` with `framelen=201`, all-spaces payload.

### Type 3 — Routing / route tokens

- **Plain tokens:** `"3+\r"` (`0x0808fc33`) and `"3-\r"` (`0x0808fc6c`).
- **Compact record wire format:** Built byte-by-byte in `0x0804fdd0`. Each record is:
  ```
  7-byte callsign + SSID range bytes + decimal RTT in ASCII + space-separator
  ```
  Actual parser logic (type-3 RX at `0x08050436`):
  ```
  loop while len > 8 and byte[6] <= byte[7] (SSID range valid):
    strtol(record+8, &next_ptr, 10)   → parsed RTT
    if RTT != 0: RTT += [port+0x28]   (port-specific RTT bias)
    call dest_update(port_row, record, RTT)   ; 0x0804f850
    skip spaces until next record
  then:
    if byte = '-': state=3; send type-4 if seq changed
    if byte = '+': state=2; trigger our 3+ response; send '3-\r'; state=1
  ```
- **Observed SSID range encoding:** `byte[6]` = low SSID, `byte[7]` = high SSID, both `0x30 + N`. This matches the `%3d-%-3d` display format seen elsewhere in xnet.

### Type 4 — Routing-table sequence number (NOT in PROTOCOL_SPEC.md)

**This is a protocol message not documented in the previous spec.**

- **TX format:** `sprintf(buf, "4%u\r", word[0x80a1fd0])` — rodata `0x0808fcca`.
- **TX trigger:** `0x080507b0`, checked every second per port. Sends when `word[row+0x22] != word[0x80a1fd0]` — i.e., our own global routing seq has advanced since we last told this peer.
- **RX handler (`0x08050515`):** `strtol(payload+1, NULL, 10)` → `word[row+0x20]`. No response.
- **Purpose:** Sequence-number gossip so peers know when to (re-)request routes. Differs from BGP-style updates — it's a hint, not an authoritative value.

### Type 5 — Unhandled

`0x08050555`: logs `"%s"` with the payload and drops. Xnet does not implement type 5.

### Type 6 — Path request (D command over multi-hop)

- **TX format:** `sprintf(buf, "6 %c%4d%a %a\r", flag, seq, next_hop_cs, dest_cs)` — rodata `0x0808ff29`.
- **Flag char:** `0x20` (space) when first byte of input is zero (fresh request), `0x60` (`` ` ``) otherwise (forwarded).
- **Custom `%a`:** xnet's own AX.25-callsign formatter (implemented in the `0x0806e280` family of functions).
- **Builder/forwarder:** `0x0804fbb0` — same function is called by type-6 RX (`0x08050530`).
- **Byte `payload[1]` is a TTL / forward counter** — incremented on every forward, capped at `0x50` (80) before the frame is dropped.
- **Self-loop detection:** emits `"Loop to %s detected"` (rodata `0x0808fbf9`) when our own callsign appears in the path list.

The spec's `flags='!'` is not correct — the actual flag byte is ` ` (space) or `` ` `` (backtick) depending on origin state.

### Type 7 — Path reply

- **TX format:** (no single sprintf; built by walking callsign list, using `"%a "` and `"%a"` pieces).
- **RX handler `0x0804fa00`:**
  - First byte forced back to `'7'`
  - `byte[1]` is a hop counter (incremented on every forward by dispatcher at `0x08050540`, capped at 80)
  - Extracts first callsign. If it matches ours → we are the addressee; call `strtol` on the seq field and invoke callback `0x08054150`. Else → look up the destination and forward via `0x0804f1e0`.

---

## 4. CF / L3RTT — PID=0xCF

- **TX format:** `"L3RTT:%11lu%11lu%11lu%11lu %-6.6s LEVEL3_V2.1 (X)NET%d $M%u $N\r"` — rodata `0x08093760`.
- **Builder:** `0x0806a3f0`. Sets `buf[0x57] = 0xCF` before enqueue.
- 4 × 11-digit unsigned longs + 6-char alias + version + `$M<rtt>` + `$N` sentinel — matches PROTOCOL_SPEC.md verbatim.

---

## 5. Timing / scheduling

### Main polling loop (`0x08050830`)

```
port_idx = 0
loop:
    sleep(1000 ms)
    if port_idx > 19: port_idx = 0; continue
    row = 0x80a1908 + port_idx * 48       ; port cfg
    if byte[row] == 0: goto next           ; port not configured
    call port_tick(row)                    ; 0x080507b0
    if byte[row+0x1a] == 0:                ; active-port flag
        call three_plus_check(row)         ; 0x08050220 -> 0x0804fdd0
    next: port_idx++
    jmp loop
```

### Per-port tick `0x080507b0`

```
row_state = 0x80a1d00 + port.idx * 36
if byte[port.cfg + 0x1a] == 0 AND word[row_state+0x22] != word[0x80a1fd0]:
    call type4_send(port, row_state)       ; 0x08050760

delta = now_ticks - dword[row_state+0x14]
if delta > 0x2BF20 (180000 ms):
    if byte[port+0x1a] == 0:  call keepalive_and_routing_tx(port)   ; 0x080505a0
    else:                     call passive_port_tick(port)          ; 0x080506c0
```

### Keepalive + routing TX `0x080505a0`

```
row_state = 0x80a1d00 + port.idx * 36
link = [port+0x24]
if link != 0:
    if byte[link] > 4 AND byte[link+0x53] <= 7:
        call keepalive_tx(link, row_state, port)   ; 0x0804f360
        [row_state+0x14] = now_ticks                ; rearm 180 s timer
    else:
        # session not fully up yet: run link-state helper
        call session_helper(link)                   ; 0x0804f510
else:
    ... (no active link — build a D-record advertisement)
```

### '3+' emit check `0x0804fdd0`

Called from per-port tick AND from type-3-plus RX. Top-level guards:
- Port struct non-null, `[port+0x24]` (link) non-null
- Port-state row `[edi+1] == 3` (routing-ready)
- Link `byte[link] > 4 AND byte[link+0x53] == 0`

Then iterates destinations, computes an internal "advertisement debt" metric `[ebp-0x1c]` based on measured RTT vs peer's smoothed value, and only emits `"3+\r"` via the shared sender when:
- Metric > 20
- State byte == 1 (idle)
- Destination callsign != our own

After TX: `state = 4`, timestamp `[row+0x18] = now`. A subsequent tick will reset state → `1` if `(now - [row+0x18]) > 0x57E40` (~360000 ms = **6 minutes** timeout).

---

## 6. Discrepancies with `PROTOCOL_SPEC.md`

| # | Topic                     | PROTOCOL_SPEC.md                                                             | RE finding                                                                                                      | Action |
|---|---------------------------|------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------------------|--------|
| 1 | Keepalive wire format     | 241 bytes = `0x32 + 237×0x20 + 0x31 0x30 0x0D` ("2...10\r")                 | 241 bytes = `0x32 + 240×0x20` (no trailer). PCF variant is 201 bytes. The `10\r` / `11\r` / `12\r` seen in monitor are SEPARATE type-1 frames concatenated in display. | **Fix spec** |
| 2 | Keepalive period          | "~189 s"                                                                    | Exactly 180000 ms (`0x2BF20`). The 189 s was sampling error.                                                    | **Fix spec** |
| 3 | Type 4 (seq)              | *Not mentioned*                                                              | `'4' + decimal_seq + '\r'`. Sent whenever peer is not up-to-date with our routing-table seq (`word 0x080a1fd0`). | **Add to spec** |
| 4 | Type 6 flag byte          | `flags='!'`                                                                 | Space (`0x20`) or backtick (`0x60`). `0x20` = fresh request, `0x60` = forwarded.                                | **Fix spec** |
| 5 | Type 6/7 TTL mechanism    | *Not mentioned*                                                              | `byte[1]` increments on every forward and is capped at 80 (`0x50`) — prevents path-request loops.              | **Add to spec** |
| 6 | Type 5                    | *Not mentioned*                                                              | Reserved but unimplemented: xnet logs and drops.                                                                | **Add to spec** |
| 7 | '3+' timeout              | *Not mentioned*                                                              | If no '3-' reply within 360 s (`0x57E40`), xnet resets state. Affects how aggressively a peer can re-request.  | **Add to spec** |
| 8 | Link-time wire unit        | Unspecified                                                                  | Wire is `smoothed_ticks / 100` where internal ticks are 10 ms, so on-wire value is seconds.                      | **Clarify spec** |
| 9 | '3+' emit gating           | "cycle-boundary token exchange"                                              | Actual gating: per-port 1-s tick, state==1, internal "advertisement debt" > 20, counted across destinations.    | **Clarify spec** |
| 10| Keepalive origin           | Assumed symmetric xnet↔xnet                                                | xnet and pcf use different keepalive lengths (241 vs 201), but both are `'2' + N spaces` with no trailer.       | **Clarify spec** |

### Items the spec got right

- PID assignments (CE/CF/F0) ✓
- Type 0 init sequence ✓
- L3RTT format ✓
- SSID encoding `0x30+N` for 0..15 ✓
- Type 3 compact record + plus/minus tokens ✓
- Identity preservation via AX.25 digipeater semantics ✓
- D-table / CREQ / CACK over PID=0xCF ✓
- Poison reverse (RTT=60000 on link drop) ✓

---

## 7. Implications for `flexnetd`

1. **Keepalive builder (`ce_build_keepalive`)** — CURRENTLY writes `'2' + 237 spaces + '1' '0' '\r'` per the old spec. **This is wrong.** Xnet sends `'2' + 240 spaces`. PCF sends `'2' + 200 spaces`. Either form is valid on-wire, but flexnetd's current output is an asymmetric hybrid. Recommendation: drop the `10\r` tail; use `sprintf(buf, "2%240s", "")` style, or just memset 240 spaces.
2. **Type 4 support** — flexnetd currently ignores type 4 both in and out. Adding TX would let peers know when our dest table has changed without waiting for the next '3+' cycle. RX is trivial (just store the seq).
3. **'3+' cadence** — our current v0.7.1.2 hardcodes 320 s retry ("ts_ahead"). Xnet's actual emit is driven by internal "advertisement debt" ≥ 20, with a 360 s timeout after sending '3+'. Matching this behaviour would likely stabilize PCF's Q/T.
4. **Keepalive period** — align flexnetd's period with xnet's 180 s exactly (currently we use 189 s from the old spec).
5. **Type 6/7 TTL** — our forwarder logic should increment `byte[1]` and drop at 80. Low risk, small patch.

---

## 8. Methodology / tools

- `re_linuxnet.py` (this repo) — lightweight disassembly helper: `d VA [size]` / `refs VA` / `calls VA` / `fnstart VA` / `str VA`
- Capstone 5.0.7 (x86 32-bit)
- Cross-validation against `flexnet_capture_1h.json` (IR3UHU-2↔IW2OHX-14, April 2026) and `flexnet_capture_port1.json` (IW2OHX-12↔IW2OHX-14, April 2026)

### Function cheat sheet

| VA         | Role                                                          |
|------------|---------------------------------------------------------------|
| `0x0804ee10` | Decode AX.25-encoded callsign into 8-byte ASCII form        |
| `0x0804ee70` | Compare two callsigns                                        |
| `0x0804ef20` | Resolve neighbor record from port struct                     |
| `0x0804ef50` | Destination lookup by callsign                               |
| `0x0804f140` | Read RTT of a destination                                    |
| `0x0804f190` | Read port's time-base offset                                 |
| `0x0804f1e0` | **Send CE frame**                                            |
| `0x0804f360` | **Build keepalive** (`sprintf "2%240s"`)                      |
| `0x0804f510` | Session down / reset handler                                 |
| `0x0804f850` | **Destination update/insert**                                 |
| `0x0804f920` | Callsign-to-destination lookup                                |
| `0x0804fa00` | Type-7 RX handler                                             |
| `0x0804fb20` | Early '3+' handshake variant (used on direct match in 0x804fbb0) |
| `0x0804fbb0` | Type-6 RX/forward + originator emit                           |
| `0x0804fdd0` | **'3+' emit check + compact-record send**                     |
| `0x0805023b` | Wrapper: lock → `0x0804fdd0` → unlock                         |
| `0x08050250` | **CE frame dispatcher (PID=0xCE entry point)**                |
| `0x080502a8` | Type-0 handler (Init handshake)                               |
| `0x08050310` | Type-1 handler (link-time RX)                                 |
| `0x080503f2` | Type-2 handler (keepalive RX, replies with type-1)            |
| `0x08050436` | Type-3 handler (routing / tokens)                             |
| `0x08050515` | Type-4 handler (seq sync)                                     |
| `0x08050530` | Type-6 RX (→ 0x0804fbb0)                                      |
| `0x08050540` | Type-7 RX (→ 0x0804fa00)                                      |
| `0x08050555` | Type-5 unknown logger                                         |
| `0x080505a0` | Keepalive + routing TX                                        |
| `0x08050760` | **Build type-4 seq frame**                                    |
| `0x080507b0` | Per-port periodic tick                                         |
| `0x08050830` | **Main 1-s scheduler loop (iterates 20 ports)**                |
| `0x0806a3f0` | **Build L3RTT frame (PID=0xCF)**                              |
