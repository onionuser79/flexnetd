# flexnetd

Native FlexNet **CE/CF** routing daemon for Linux AX.25. Peers directly with
(X)Net and PC/Flexnet nodes over AX.25 (RF, AXIP or AXUDP) and feeds URONode;
replaces the legacy text-polling `flexd`. C11, single-threaded poll loop,
`libax25`. **v1.0.0.** GPL v3. This repo is **public**.

It is also the **protocol reference implementation** for the sibling
`linbpq-flexnet` project. When the two disagree about a wire format, a live
capture wins; absent a capture, this repo is canonical.

## Commands

```bash
./sync-and-build.sh            # rsync to iw2ohx-gw + remote `make` (see below)
./sync-and-build.sh debug      # any make target passes straight through

make check-deps                # libax25 + axconfig.h present?
make                           # release: flexnetd + flexdest
make syntax                    # compile each TU to /dev/null — fast sanity pass
make debug                     # -g3 -O0 -DDEBUG, no sanitisers (safe under sudo)
make asan                      # -fsanitize=address,undefined — run WITHOUT sudo
sudo make install              # /usr/local/sbin + conf if absent
```

**Build on the target, not on macmini** — macOS has no `libax25`, so nothing
here compiles locally. `sync-and-build.sh` rsyncs to
`iw2ohx-gw:/home/iw2ohx/xnet_investigation_agent/flexnetd` and runs `make`
there; that remote tree is a build sandbox, so **edit here and sync, never edit
the remote copy**.

## Build reality vs the global C standards

Two deliberate gaps — know them before "fixing" them:

- `CFLAGS_COMMON` is `-Wall -Wextra -Wshadow -Wformat=2 -std=c11`. It does
  **not** carry `-Wpedantic`, `-Wconversion` or `-Werror`. Adding them is a
  real cleanup task, not a one-line edit — raise it before starting.
- **There is no unit-test framework.** `tools/test_multi_bind.c` is a manual
  hardware probe (`make test_multi_bind`, run with `sudo`), not a test suite.
  Per the global rule: **propose a framework before writing ad-hoc test code.**
  Until then, verification is `make syntax` + `make asan` + a live capture.

## Hard rules

1. **Never guess a wire format.** Every byte emitted was derived from a capture
   of real peers and re-verified by a follow-up capture after deploy. Change
   one thing, build, deploy, re-capture.
2. **Cite the source of every wire constant** — a captured frame, or
   `PROTOCOL_SPEC.md`. A constant with no citation is a guess and does not
   belong in the code yet.
3. **Observation language in anything public** (README, spec, comments, commit
   messages): "captures show that the peer…", never claims about a peer's
   internals. This is framed as a protocol implementation, not as analysis of
   someone else's software.
4. **A wrong byte produces silence, not an error.** There is no peer-side log.
   Document the byte layout above every build/parse function.
5. No credentials, no sys passwords, no telnet logins anywhere in the repo.

## Code map

| Area | File |
|------|------|
| `main()`, daemonise, signal + poll loop | `flexnetd.c` |
| Config parser (`Port` lines, per-port options) | `config.c` |
| AX.25 L2 session management | `ax25sock.c` |
| PID=CE native FlexNet protocol | `ce_proto.c` |
| PID=CF NET/ROM-compatible FlexNet protocol | `cf_proto.c` |
| Poll cycle + native session handler (largest TU) | `poll_cycle.c` |
| Destination routing table | `dtable.c` |
| State-file writers (`gateways`, `destinations`) | `output.c` |
| Callsign utilities | `util.c` · logging `log.c` |
| Shared types and prototypes | `flexnetd.h` |
| Standalone destination query tool (no libax25) | `flexdest.c` |

## Operational gotchas

- **The listen callsign must NOT appear in `/etc/ax25/ax25d.conf`.** flexnetd
  binds it itself via `ax25_listen()` per port; if ax25d also binds it, one of
  the two silently loses. This is the single most common bring-up failure.
- **Outbound transit needs the URONode patch** —
  `patches/uronode-m2-digipeater-path.patch` sets `AX25_IAMDIGI` before
  `connect()`. Without it the SABM goes out with no `H` bit on our callsign,
  the neighbour doesn't see itself as an intermediate hop, and the connect is
  dropped.
- The `dev` column in `gateways` is the **kernel interface** (`ax1`), not the
  `axports` port name. Getting this wrong routes nowhere.
- State files land in `/usr/local/var/lib/ax25/flex/` (`gateways`,
  `destinations`); URONode reads them.
- Legacy `flexd` must be stopped **and** disabled — flexnetd replaces it.
- Runs via `flexnetd.service` (`Type=forking`, `-d -c
  /usr/local/etc/ax25/flexnetd.conf`, `After=ax25.service`).
- `make install` never overwrites an existing `$(CONFDIR)/flexnetd.conf`, so on
  an upgrade the installed config is whatever is already there — diff it against
  this repo's `flexnetd.conf` after a schema change. (Note the parent directory
  holds an **old-schema** copy using `Neighbor`/`Interface`/`Role responder`
  instead of `Port` lines; it is not what this repo installs.)
- **`URONode/` is deliberately untracked** (gitignored): a local URONode source
  tree for developing and testing `patches/`. It has its own upstream and
  licence and is not part of this project, so changes there are not versioned
  here.

## Live deployment (station facts — deliberately NOT in the public docs)

As of 2026-09-23: runs on iw2ohx-gw as **IW2OHX-3**, **started by hand**
(`flexnetd.service` installed but disabled, like `ax25.service` there). One
port: `Port xnet IW2OHX-14 IW2OHX-3 route_advert=0 lt_reply=0
advert_mode=full`. The PC/Flexnet port (`IW2OHX-12`) is **not configured**
and has not run since 2026-04-21 — so ROADMAP's "PC/Flexnet link cost" item
needs it re-armed to be tested. ⚠ The repo's `flexnetd.conf` is now a
**generic template** (`NODEA-3`/`NODEB-14`) since 2026-09-29 — never copy it
over the live `/usr/local/etc/ax25/flexnetd.conf` (`make install` won't).
⚠ `config.c` still defaults `flex_listen_call` to `IW2OHX-9` (legacy
single-port path only).

## Documentation set (product docs — keep them product docs)

Marco's rule, 2026-09-29: public docs are **product documentation, not a
diary** — no station callsigns or test narratives; examples use fictional
calls (`NODEA-3`, `NODEB-14`, `NODEC-12`, `USER-15`, `DEST`).

- `README.md` — install + parameter reference + running/troubleshooting.
- `RELEASE_NOTES.md` — per release: change, upgrade notes. Replaces the old
  `RELEASE_HISTORY.md` (git history). `ax25d.conf` / `ax25d.conf.patch`
  (station configs) were removed the same day.
- `ROADMAP.md` — short: v1.1, candidates, out of scope.
- `PROTOCOL_SPEC.md` — RFC-style spec, **byte-identical with
  linbpq-flexnet's**: edit one, `cp` to the other, `cmp`, commit both.
- `CONTRIBUTING.md` — build targets, rules, release steps.
- `patches/README.md` — why the URONode patch exists and how to apply it.
