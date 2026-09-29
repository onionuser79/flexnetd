# Roadmap

**Current release: v1.0.0.** What has shipped is in
[RELEASE_NOTES.md](RELEASE_NOTES.md).

flexnetd does what it set out to do: a stable FlexNet participant that
feeds URONode. The remaining work is refinement.

## Next: v1.1

| Item | What | Why |
|---|---|---|
| **PC/Flexnet link cost** | Advertise a link-time value of 5 (instead of 2) on PC/Flexnet ports, and send keepalives to them just under 29 s | PC/Flexnet's displayed cost for a flexnetd link settles too high. The same change brought linbpq-flexnet's cost at PC/Flexnet down to the 1–2 its (X)Net neighbours show. Needs a PC/Flexnet port to verify on. |
| **Re-seed on PC/Flexnet reconnect** | Confirm own routes are re-sent when PC/Flexnet restarts the AX.25 session | PC/Flexnet recycles AXIP links every ~5445 s ([spec §6.4.2](PROTOCOL_SPEC.md)) |
| **Configuration comments** | Explain in `flexnetd.conf` why `route_advert=0` and `advert_mode=record` are required on PC/Flexnet ports ([spec §8.3](PROTOCOL_SPEC.md)) | So nobody turns periodic advertisement back on |
| **Unused keywords** | Remove or implement `KeepaliveInterval`, `BeaconInterval`, `TriggerThreshold`, `ProbeCount` | They are parsed but have no effect |

## Later — candidates, not scheduled

| Item | Why |
|---|---|
| Forward CE type-6 path queries ([spec §9.1.3](PROTOCOL_SPEC.md)) instead of ignoring those not addressed to us | Lets neighbours see complete routes through the node. Stateless; small. |
| Unit tests for the frame builders and parsers in `ce_proto.c` / `cf_proto.c` | They are pure functions over byte buffers; today verification is `make syntax`, `make asan` and live captures |
| Stricter compiler warnings (`-Wpedantic -Wconversion`) | Measure per file first |
| Config reload on `SIGHUP` | Today a restart is needed |

## Out of scope

- **Transit.** flexnetd advertises only its own callsign and carries no
  other stations' traffic. That rules out whole classes of routing
  failure (black holes, count-to-infinity). Carrying transit would need
  FlexNet L2 chain rewriting ([spec §10](PROTOCOL_SPEC.md)) in the
  kernel AX.25 path — for a routing node, use linbpq-flexnet or a
  dedicated FlexNet router.
- NET/ROM CREQ/CACK session handling — FlexNet user sessions are plain
  AX.25 connections.
- A shared code library with linbpq-flexnet. The protocol specification
  is the shared artefact.
