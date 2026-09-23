# flexnetd — Release history (archive)

The milestone narratives that `ROADMAP.md` carried until 2026-09-23. Kept
verbatim because they record *why* each knob has the value it has and what the
wire showed at the time — none of it is written down elsewhere — but it is
history, not plan. `ROADMAP.md` is the current document.

Two caveats when reading it:

- **Some conclusions were later refined by the sibling project.** Most
  importantly, `PROTOCOL_SPEC.md` §5.1's "extending the digi chain breaks
  AX.25 V2" was too strong — §5.2 has the real mechanism, captured on
  PC/Flexnet `IW2OHX-12` on 2026-09-17.
- **State statements are frozen at April 2026.** The two-port
  (X)Net + PC/Flexnet deployment described below is not what the installed
  config runs today; see `ROADMAP.md`.

---

## Completed milestones

### M1 — Protocol correctness (v0.4.0)

Correctness fixes aligning the daemon with the on-wire protocol.

- **M1.1** SSID encoding 0–15: single-char `0x30+N` (`:;<=>?` for 10–15)
- **M1.2** Init frame byte 0 always `0x30`; no double-init in server mode
- **M1.3** L3RTT c3/c4 semantics: zero when link down, non-zero when active
- **Debug logging** via `-l <logfile>` for dual console + file output

### M2 — Digipeater path preservation (v0.5.0, M2.1 in v0.7.0)

**M2.2 (outbound)** — URONode gateways file includes IW2OHX-3 as a
digipeater.  URONode outbound connects build a SABM like
`fm USER to DEST via IW2OHX-3 IW2OHX-14`, with the `H` bit set on
IW2OHX-3, giving proper digipeater path preservation.

Key kernel behaviour discovered: `ax25_connect()` only honours
user-supplied `H` bits if `AX25_IAMDIGI` is also set on the socket.
Without it, the kernel silently clears every `repeated[]` flag.
The URONode patch (`patches/uronode-m2-digipeater-path.patch`)
sets `AX25_IAMDIGI` before connect and then `|= 0x80` on the first
digipeater byte.

Confirmed live 2026-04-14: IW7CFD → URONode → IR5S produces the
expected `IR5S>IW7CFD-15 v IQ5KG-7 IW2OHX-3` in the destination's
`U` output.

**M2.1 (inbound transit)** — in place on multi-port deployments:
when the inbound via-list contains our callsign, the kernel AX.25
digipeater path (enabled via `/proc/sys/net/ax25/<iface>/digi`)
forwards the connection across ports.

### M3 — Link-stats output file (v0.4.1)

`/usr/local/var/lib/ax25/flex/linkstats`, refreshed every 30 s, in
(X)NET `L`-table format.  Tracks Q/T, RTT, tx/rx bytes, frames,
connect time, destination count.  Multi-port deployments write one
per-port file and merge via `flock`.

### M4 — Destination query (`flexdest`) (v0.4.1)

Standalone tool `flexdest`:
- Exact match: `flexdest IR5S`
- Prefix wildcard: `flexdest IW*`
- SSID-specific: `flexdest IR5S-7`
- No libax25 dependency — reads the destinations file directly.
- `-r` flag shows the recorded hop chain from the paths cache.

VIA field in the destinations file carries the actual next-hop
callsign (e.g. `IW2OHX-12` or `IW2OHX-14`) rather than `00000`.

### M5 — Route path display (v0.6.0)

Full CE type-6 / type-7 path query protocol:
- `ce_build_path_request` / `ce_build_path_reply`
- `ce_parse_path_frame`
- Pending-query table with QSO correlator and 30-second timeout
- Periodic probing with configurable `PathProbeInterval`
- Path cache file (`/usr/local/var/lib/ax25/flex/paths`)
- `flexdest -r` reads the cache and prints the recorded hop chain

Peer compatibility note: (X)NET V1.39 does NOT emit type-7 replies.
`PathProbeInterval 0` is recommended in production against that peer
family to avoid wasteful airtime.  PC/Flexnet does reply.

### M6 — Multi-neighbor / multi-port (v0.7.0 – v0.7.8)

Up to 4 concurrent CE/CF sessions, each on its own port, each with
its own neighbor and optional per-port overrides.  All sessions
share one listen callsign (IW2OHX-3) via `SO_BINDTODEVICE`.

Per-port merge of destinations and linkstats: each CE child writes
its own file (e.g. `destinations.xnet`, `destinations.pcf`) and a
`flock`-serialised merge produces the unified files, picking the
lowest-RTT entry per `(callsign, ssid_range)` pair across peers.

**Per-port overrides (the three peer-specific knobs):**

| Knob           | (X)NET V1.39 | PC/Flexnet 3.3g | Global default |
|----------------|--------------|-----------------|----------------|
| `route_advert` | `0`          | `0`             | `0`            |
| `lt_reply`     | `0`          | `320`           | `320`          |
| `advert_mode`  | `full`       | `record`        | `full`         |

Rationale for each setting is captured in the production
`flexnetd.conf` and in README §5.

### M5.3 — Path probing hardening (v0.6.0 onwards)

Background probing of the destination table with
round-robin rotation; probes skip entries with `rtt >= Infinity` or
whose callsign is our own.  Replies populate the path cache read by
`flexdest -r`.  Interval configurable; default disabled against
(X)NET peers.

### Proactive keepalive cadence tuning (v0.7.9 → v1.0.0)

Final tuning step that delivered the production-stable release.

The 20-second proactive keepalive introduced during M6 as a
PC/Flexnet session-keeper was dominating (X)NET's IIR smoothed-RTT
filter.  (X)NET uses inter-frame delta of our outbound CE frames as
its sample stream; at 20 s cadence it converged the displayed RTT to
~200 ticks (20 s) instead of the ~2 ticks (200 ms) seen with
linbpq-flexnet.

**Evidence (capture 2026-04-21):**
- Our outbound CE bursts: Δ = 20-22 s (avg 20.7)
- (X)NET V1.39 native KAs: Δ = 189 s (9× longer)
- (X)NET IIR wire value converged to 171 ticks (17.1 s) as we
  expected for the 20 s sample stream
- linbpq-flexnet (IW2OHX-13, stable Q=2 RTT=2 for 8+ days): NO
  proactive timer, pure echo on peer events

**Fix:** proactive KA threshold 20 s → 300 s.  (X)NET's 189 s native
KAs always preempt our proactive timer, so we behave like
linbpq-flexnet on an active link (event-driven echo) while still
giving PC/Flexnet a 5-minute safety-net nudge if it goes silent.

**Result:** xnet's L-table for IW2OHX-3 converges to Q=2 RTT=2 within
minutes of session setup — matching the linbpq-flexnet reference.

---

