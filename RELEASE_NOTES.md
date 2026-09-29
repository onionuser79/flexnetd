# Release notes

Newest first. Configuration details are in [README.md](README.md);
protocol details in [PROTOCOL_SPEC.md](PROTOCOL_SPEC.md).

| Version | Date | Highlights |
|---|---|---|
| [1.0.0](#v100--2026-04-21) | 2026-04-21 | First production release |
| [0.7.x](#v07x--2026-04-19--2026-04-21) | 2026-04-19 → 21 | Multiple neighbours, PC/Flexnet interoperability, per-port settings |
| [0.6.0](#v060--2026-04-19) | 2026-04-19 | Path discovery |
| [0.5.0](#v050--2026-04-15) | 2026-04-15 | Node identity in outbound connections |
| [0.4.x](#v04x--2026-04-13--2026-04-14) | 2026-04-13 → 14 | Protocol corrections, link statistics, `flexdest` |
| [0.3.0](#v030--2026-04-11) | 2026-04-11 | First public release |

---

## v1.0.0 — 2026-04-21

First production release. Stable against (X)Net 1.39; operational
against PC/Flexnet 3.3g, whose displayed cost for the link is higher
than ideal (routing and link stability are unaffected).

- **Keepalive cadence.** flexnetd now answers the neighbour's keepalives
  and sends one of its own only when none has gone out for 300 s (was
  20 s). (X)Net measures a neighbour from the intervals between its
  frames, so the 20 s keepalive made (X)Net report the link at about
  17 s; it now reports the real link time.
- The shipped `flexnetd.conf` is the recommended configuration, with the
  per-port settings for both peer families.

**Upgrading from 0.7.x:** no configuration change needed. Check your
`Port` lines against the recommended values in the README.

## v0.7.x — 2026-04-19 → 2026-04-21

**Multiple neighbours and PC/Flexnet.**

| Version | Change |
|---|---|
| 0.7.8 | New per-port `advert_mode`: `full` (`3+`, own records, `3-`) for (X)Net, `record` (records only) for PC/Flexnet, which drops the link on an unexpected `3+`. |
| 0.7.7 | flexnetd no longer sends type-4 frames: (X)Net 1.39 withdraws a neighbour's routes about 20 s after receiving one. Received type-4 frames are still parsed. |
| 0.7.6 | Documentation: `lt_reply=0` recommended on (X)Net ports. |
| 0.7.5 | A destination re-sent with cost 0 no longer overwrites its real cost. (X)Net re-sends its table with cost 0 shortly after session start. |
| 0.7.4 | The `destinations` file is written after every route batch. Before, a rate limit kept only the first ~20 entries of a neighbour's initial table dump. |
| 0.7.3 | New per-port `lt_reply` (and global `LinkTimeReplyInterval`). Link-time frames to (X)Net are sent in reply to each of its keepalive and link-time frames; the 320 s interval PC/Flexnet needs had left (X)Net's measurement of the link stuck at its initial value. |
| 0.7.2 | Protocol format corrections: the keepalive is `'2'` + 240 spaces with no trailer (`"10" CR` seen after it in monitor dumps is a separate link-time frame); both 241- and 201-byte keepalives accepted; type-4 is `'4' DECIMAL CR`. |
| 0.7.1 | Per-port `route_advert`; destination files merged across ports; link-time value restored to 2 after 0.7.1.1 set it to 0 and caused a regression (0.7.1.2). |
| 0.7.0 | Up to 4 FlexNet neighbours, one per AX.25 port, sharing one listen callsign; per-port `destinations` and `linkstats` files merged under a lock, keeping the cheapest route per destination. Link-time frames to PC/Flexnet limited to one per 320 s — faster replies make PC/Flexnet record a saturated cost of 4095. |

**Upgrading to 0.7.x:** the configuration moves to `Port` lines (the
single-port keywords `Neighbor`, `PortName`, `FlexListenCall` are still
read if there is no `Port` line). For PC/Flexnet neighbours use
`route_advert=0 lt_reply=320 advert_mode=record`; for (X)Net
`route_advert=0 lt_reply=0 advert_mode=full`.

## v0.6.0 — 2026-04-19

**Path discovery.** CE type-6/7 path requests and replies, with a
pending-query table (30 s timeout), optional background probing
(`PathProbeInterval`, off by default), a `paths` cache file, and
`flexdest -r <call>` to show the recorded hop chain.

## v0.5.0 — 2026-04-15

**Node identity in outbound connections.** The `gateways` file carries
the listen callsign as a digipeater, and the URONode patch in `patches/`
marks it as already repeated, so a user's connection leaves as
`USER -> DEST via NODEA-3* NODEB` and the destination sees which node the
user came from. The patch is needed because the Linux kernel ignores
user-supplied "repeated" bits in `connect()` unless the socket has
`AX25_IAMDIGI` set.

## v0.4.x — 2026-04-13 → 2026-04-14

- **0.4.1**: `linkstats` file ((X)Net `L`-table format, 30 s refresh);
  `flexdest` destination query tool; the `destinations` file names the
  real next hop.
- **0.4.0**: protocol corrections — SSID encoding for 10–15
  (`:;<=>?`), init frame byte 0 always `'0'`, no double init in server
  mode, L3RTT counters zero only when there are no routes. `-l` option for
  logging to a file.

## v0.3.0 — 2026-04-11

First public release: native FlexNet CE/CF peering with one (X)Net
neighbour, route exchange, `gateways` and `destinations` files for
URONode, server mode (FlexNet neighbour → FlexNet session, any other
caller → URONode), legacy client mode.
