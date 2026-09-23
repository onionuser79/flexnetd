# flexnetd — Roadmap

**Current: v1.0.0** (2026-04-21) · live on `iw2ohx-gw` as **IW2OHX-3**

v1.0.0 closed every milestone this daemon set out to reach, and it has run
since without a code change. **What moved since April is not in this daemon —
it is what the sibling `linbpq-flexnet` project learned on the wire.** So this
roadmap is mostly one question asked four ways: *which of those findings does
flexnetd have to absorb, and which does its narrower role excuse it from?*

---

## Where we are — observed live, 2026-09-23

| | |
|---|---|
| process | two PIDs (parent + one CE child), up since **2026-09-21 09:31** |
| started by | **hand, not systemd** — `flexnetd.service` is installed but `disabled`, the same pattern as `ax25.service` on this host (the AX.25 stack is brought up manually after a reboot) |
| ports configured | **one**: `Port xnet IW2OHX-14 IW2OHX-3 route_advert=0 lt_reply=0 advert_mode=full` |
| link | `IW2OHX-14`, up **53 h 31 m**, rtt **1**, 0 % retries |
| destinations | **211** learned, merged file refreshed every 30 s |
| what we advertise | exactly **one** record: `IW2OHX-3`, SSID 3-3, RTT=1 |
| PC/Flexnet port | **not in the installed config.** `destinations.pcf` is empty and `linkstats.pcf` has not been written since **2026-04-21 23:32** |

Two things follow from that table and they shape everything below.

**flexnetd is a leaf by construction.** `send_own_routes()` emits its own
callsign and nothing else — there is no `advertised[]`, no split-horizon, no
poison-reverse, no transit. That is a *design*, not a gap: it feeds URONode,
it does not route for strangers. It is also why none of the sibling's
black-hole and count-to-infinity failure modes can happen here.

**The multi-port / PC/Flexnet capability is built but dormant.** M6 shipped
it, the per-port override machinery works, and the installed config no longer
uses it. Every PC/Flexnet item below is therefore blocked on one decision (§B).

---

## Open work at a glance

```
 v1.0.0 ── released 2026-04-21, unchanged since ───────────────────────────►
   │
   ├─► A  absorb the 2026-09 wire findings   know ▓▓▓▓▓  build ▓░░░░
   │      4 of 6 are a doc line, 1 is a defect in our own spec, 1 is code
   │
   ├─► B  PC/Flexnet port: re-arm or retire  ◆ DECISION — blocks C
   │      dormant since 2026-04-21; the code is there, the config is not
   │
   ├─► C  PCF displayed RTT → 1 tick         know ▓▓▓▓▓  build ░░░░░
   │      v1.0's one open follow-up — the sibling already answered it
   │
   └─► D  stay a leaf?                       ◆ DECISION — recommend: yes
          transit cost the sibling 3 releases and 5 mechanisms; nothing
          in flexnetd's role needs it
```

| # | Item | Size | Gate | If ignored |
|---|------|------|------|-----------|
| A | Absorb the wire findings | small | none | the spec misleads the next reader, and it is the citation target |
| B | PCF port decision | — | **Marco** | C, and half of M6 stays unexercised |
| C | PCF RTT convergence | small | B | nothing breaks; the peer's cost display stays coarse |
| D | Leaf vs transit | — | **Marco** | nothing — the status quo is the recommendation |

None of these is urgent. The link has been up 53 h at rtt 1 and the daemon has
not needed a change in five months. **This is a maintenance roadmap, not a
development one**, and the honest summary is that item A is the only one with
a cost to leaving undone.

---

## A — Absorb the 2026-09 wire findings

The sibling project spent September on captures that settle questions this
repo's spec had answered provisionally or wrongly. `PROTOCOL_SPEC.md` is the
**citation target** for both repos, so a stale line here costs more than a
stale line in code.

| Finding (where it was proven) | flexnetd today | Action |
|---|---|---|
| **Symmetric digi-chain rewriting** is how multi-hop transit works — forward: set own H-bit + append next hop; reverse: remove what you appended (captured on PCF `IW2OHX-12`, 2026-09-17) | spec §5.2 **already updated**; no code, and none needed — a leaf never transits | ✅ done — record *why* the code stays absent |
| **CE type-6 is a traversal, not a query** — the request carries the chain `[asker … target]`, and a node that cannot finish it **inserts its own next hop before the target** and forwards (2026-09-18) | we answer only when the target is us; otherwise `"not us — ignored (forwarding is M2.1)"` (`poll_cycle.c:995`) | implement insert-and-forward, **or** write the decline down as policy. Cheap: no per-traversal state — the chain says who asked |
| ⚠ **Spec §2.9 contradicts our own code.** It names the two callsign fields `DEST_CS ' ' NEXT_HOP_CS`; `ce_build_path_request()` emits `ORIGIN ' ' TARGET`, and the capture chain `IW2OHX-4 IW2OHX-12 IR3UGM` confirms asker-first | **the code is right, the spec text is wrong** | fix §2.9 — it is the one place a reader could implement backwards from our own document |
| **PC/Flexnet accepts at most two record frames after the `3-` that closes a `3+` answer, then DISCs** — 30/30, reacting within 0.06 s (2026-09-22) | a PCF port already never sees an unsolicited record: `route_advert=0` disables the periodic refresh, and `advert_mode=record` keeps `3+` off that port entirely (we still open with `3+` on an (X)Net port, which wants it) | ✅ **we were right for four months without knowing why.** Write the mechanism into the config comments and spec §2.6 so nobody "optimises" `route_advert` back on |
| **PCF's AXIP link lifetime is a fixed 5445 s, not an idle timeout** — four consecutive cycles to the second (2026-09-22) | the 300 s proactive KA was designed as a session-keeper against an *idle* timeout | it cannot prevent the cycle; nothing to fix, but stop treating a cycle as a defect and verify we re-seed cleanly on the reconnect |
| **Outbound link-time replies must be ≥ 320 s apart for a PCF peer** | `lt_reply=320` on PCF ports since v0.7.3 | ✅ the two implementations independently agree |

Effort: an afternoon, most of it documentation. The type-6 forwarding half is
the only real code, and it is optional — see §D.

---

## B — PC/Flexnet port: re-arm or retire ◆

**Decision needed.** The installed config has a single `Port xnet` line;
`destinations.pcf` has been empty since 2026-04-21. The M6 per-port machinery,
the three PCF-specific knobs and the `advert_mode=record` path are all still in
the code and all currently unexercised.

Three honest options:

| | What it means | Cost |
|---|---|---|
| **Re-arm** | add the `Port pcf IW2OHX-12 …` line back, `route_advert=0 lt_reply=320 advert_mode=record` | one config line + a restart; gives §C something to measure and re-exercises half of M6 |
| **Retire** | declare flexnetd an (X)Net-family daemon, mark the PCF paths unmaintained in the README | honest, and it closes §C as "won't do" |
| **Leave dormant** | today's state, undeclared | the code rots quietly and the README keeps promising a peer family we do not run |

Recommendation: **re-arm** long enough to close §C, then decide. `IW2OHX-12`
is on the LAN and the sibling peers with it daily, so the risk is a config
line, not a field trip.

---

## C — PC/Flexnet displayed RTT → 1 tick

v1.0's only open follow-up, and **the sibling has already answered the first
of its three investigation areas**:

> *"Alternative link-time values (currently hard-coded `2`)"*

linbpq-flexnet v2.1.28 measured exactly this. PCF's per-sample arithmetic is
`sample = KA_arrival − (smoothed + 4) × 32`, so an advertised link-time of `2`
opens a 19.2 s window; a keepalive landing outside it scores ~100 ticks.
**Advertising `5` widens the window to 28.8 s and lands samples at 1-2 ticks**,
which is the steady state (X)Net peers show. That repo pairs it with a
keepalive threshold just inside the window (29 s).

So the experiment is specified, not open: set the advertised link-time to 5 on
a PCF port, hold the keepalive just under the window, and read PCF's `L *`.
Expect `1/1` or `2/2`. The other two areas (ts_ahead negotiation, and the
300 s KA's interaction with PCF's own model) stay open, and the 5445 s
lifetime finding above says the *link* will cycle regardless — judge the cost
row, not the uptime.

Blocked on §B.

---

## D — Stay a leaf? ◆

**Recommendation: yes**, and write it down as a decision rather than leaving
it as an omission.

The sibling built the transit role and it cost three releases and five
distinct failure mechanisms — phantom destinations that poison-reverse could
not retract, a 67-destination black hole from advertising what it could not
carry, a geometric count-to-infinity climb in 43 of 204 destinations, and two
PC/Flexnet teardown mechanisms. It was worth it there: a BPQ node sits between
two clouds. flexnetd feeds URONode from one neighbour.

What that buys flexnetd, and what it should keep buying:

- advertising exactly one record can never create a black hole;
- no `learned[]` re-advertisement means no loop, no hold-down, no split-horizon
  to get wrong;
- and the PCF quiesce the sibling needed a 20.9 h capture to discover is, here,
  simply the absence of a feature.

If transit is ever wanted, the prerequisite is **not** the advertisement plane
— it is §5.2 digi-chain rewriting in the kernel AX.25 path, which is a much
larger piece of work than anything on this page.

---

## Engineering debt

Both are flagged in `CLAUDE.md` as things not to "fix" casually. They belong
here so they are visible rather than tribal:

| Item | State | Note |
|---|---|---|
| **No unit-test framework** | `tools/test_multi_bind.c` is a manual hardware probe, not a suite. Verification is `make syntax` + `make asan` + a live capture | The sibling now has `tools/unit/` — tests that **extract the function under test verbatim from the source** so they cannot drift. That pattern ports cleanly to `ce_proto.c`'s builders and parsers, which are pure functions over byte buffers. Propose before writing |
| **A stale comment on a live path** | `send_own_routes()`'s header still says mode 2 (`'3+'`/record/`'3-'`) is "reserved for future use (currently no caller)" — v0.7.8 gave it a caller for every `advert_mode=full` port | One-line fix, but it is the kind of comment that makes the next reader believe we never send `3+` |
| **Warning flags short of the house standard** | `CFLAGS_COMMON` has `-Wall -Wextra -Wshadow -Wformat=2 -std=c11`; no `-Wpedantic`, `-Wconversion`, `-Werror` | A real cleanup task, not a one-line edit. Measure the count per file first, the way the sibling did, so the size is known before it starts |

---

## Shipped

| Milestone | Released | What it delivered |
|---|---|---|
| **M1** protocol correctness | v0.4.0 | SSID encoding 0-15 (`0x30+N`, `:;<=>?` for 10-15), init byte 0 always `0x30` with no double-init in server mode, L3RTT c3/c4 zero-when-down |
| **M3** link-stats file | v0.4.1 | `linkstats` in (X)Net `L`-table format, 30 s refresh, per-port files merged under `flock` |
| **M4** destination query | v0.4.1 | `flexdest` — exact / prefix / SSID match, no libax25 dependency, real next-hop in the VIA field |
| **M2** digipeater path preservation | v0.5.0 (M2.1 in v0.7.0) | Outbound H-bit chains via the URONode patch. **The discovery: `ax25_connect()` ignores user H-bits unless `AX25_IAMDIGI` is set on the socket** — without it the kernel silently clears every `repeated[]` flag |
| **M5 / M5.3** path query | v0.6.0 | CE type-6/7 build + parse, QSO correlator with 30 s timeout, path cache, `flexdest -r`, round-robin probing that skips `rtt ≥ Infinity` and our own call |
| **M6** multi-neighbour / multi-port | v0.7.0-v0.7.8 | Up to 4 concurrent CE/CF sessions, one per port, sharing one listen callsign via `SO_BINDTODEVICE`; per-port merge picking the lowest RTT per `(callsign, ssid_range)`; the three per-port overrides `route_advert` / `lt_reply` / `advert_mode` |
| **KA cadence** | v0.7.9 → **v1.0.0** | 20 s → 300 s. The 20 s proactive keepalive *was* (X)Net's RTT sample stream, converging its display to 171 ticks; at 300 s its own 189 s keepalives preempt ours and it reads **Q=2 RTT=2**, matching the linbpq reference |

| Version | Scope |
|---|---|
| v0.3.0 | Baseline CE/CF peering, route exchange, Q/T=1 |
| v0.4.0 · v0.4.1 | M1 · M3 + M4 |
| v0.5.0 · v0.6.0 · v0.7.0 | M2 · M5 · M6 |
| v0.7.1 - v0.7.6 | Per-port `route_advert`, destinations merge, link-time back to 2, keepalive format alignment, `lt_reply` per port, destinations-file truncation fix, `dtable_merge` skips RTT=0 incoming |
| v0.7.7 | Proactive type-4 TX disabled — (X)Net V1.39 treats it as unknown and **withdraws routes** |
| v0.7.8 · v0.7.9 | Per-port `advert_mode` · proactive KA 20 s → 300 s |
| **v1.0.0** | **Production release — M1-M6 and M5.3 closed** |

Full narratives: **[`RELEASE_HISTORY.md`](RELEASE_HISTORY.md)**.

### Discarded designs — do not re-propose

| Approach | Why it is dead |
|---|---|
| CREQ frame builder for L3 connections (M2.1) | FlexNet L3 rides AX.25 digipeater chains. **Confirmed 2026-09-17** by a capture taken on a PC/Flexnet node mid-transit — no encapsulation, no L4 handshake, `src`/`dst` never rewritten |
| CREQ/CACK/DREQ session state machine | No tested peer emits such a handshake |
| Proactive CE type-4 "routes changed" TX | (X)Net V1.39 treats it as unknown and withdraws routes |
| 20 s proactive keepalive on every link | It becomes the peer's RTT sample stream. Replaced by a 300 s safety net |
| Global-only `route_advert` / `lt_reply` | Peer families diverge; replaced by per-port overrides |
| Sending `3+` on a periodic re-advertisement | PCF accepts `3+` only in token state 0, which we cannot observe remotely; it DM'd the link ~2 s later. `3+` now goes out only on an (X)Net port's opening exchange (`advert_mode=full`) |

---

## Lessons that outlived their release

1. **A wrong byte produces silence, not an error.** There is no peer-side log.
   Every constant in this daemon came from a capture and was re-verified by a
   second capture after deploy — that is the only feedback channel there is.
2. **Our own cadence becomes the peer's measurement.** (X)Net fed our 20 s
   keepalive into its IIR and displayed 17.1 s. The fix was to transmit
   *less* and let the peer's own rhythm drive the filter.
3. **Peer families diverge; a global knob does not survive contact.** Three
   settings differ between (X)Net and PC/Flexnet, and every one of them was
   found by watching a link die.
4. **The kernel will quietly undo you.** `ax25_connect()` clears user-supplied
   H-bits unless `AX25_IAMDIGI` is set — the connect still succeeds, just
   without the identity that made it routable.
5. **Being small is a feature.** Most of what the sibling project fought
   through in September cannot happen here, because this daemon advertises one
   record and forwards nothing.

---

## Out of scope

- **Being a FlexNet router.** The three real routers are (X)Net, PC/Flexnet
  and RMNC/Flexnet. flexnetd is a correct participant that feeds URONode.
- **A shared protocol module with `linbpq-flexnet`.** Considered in both
  repos, set aside in both. The spec is the shared artefact; the code is not.
- **Peer populations beyond (X)Net V1.39 and PC/Flexnet 3.3g.** Other releases
  are expected to work on the documented subset but are not bench-tested, and
  chasing them without a peer to test against would violate rule 1.

---

| Where to look | For |
|---|---|
| `CLAUDE.md` | Build reality, hard rules, operational gotchas. Wins over this file on a conflict |
| `PROTOCOL_SPEC.md` | CE/CF wire formats — the citation target for every constant |
| `README.md` | Operator manual: install, annotated config, kernel tuning, troubleshooting |
| `RELEASE_HISTORY.md` | The milestone narratives this file used to carry |
| `../linbpq-flexnet/` | The sibling. Its `research/README.md` indexes the captures behind §A |
