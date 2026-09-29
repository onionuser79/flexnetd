# Contributing

## Building

flexnetd needs libax25 and kernel AX.25 headers, so it builds only on
Linux. `sync-and-build.sh` is a helper that copies the tree to a Linux
build host with rsync and runs `make` there; edit it for your own host.

| Target | Produces |
|---|---|
| `make` | `flexnetd` and `flexdest` (release, `-O2`) |
| `make syntax` | compiles each file to `/dev/null` — a fast check |
| `make debug` | `flexnetd_debug` (`-g3 -O0 -DDEBUG`, no sanitisers — safe to run as root) |
| `make asan` | `flexnetd_asan` with address/undefined sanitisers — run **without** sudo |
| `make test_multi_bind` | `tools/test_multi_bind`, a manual check that one callsign can be bound on several ports (run with sudo) |
| `make check-deps` | checks for libax25 and its headers |

Compiler flags are `-Wall -Wextra -Wshadow -Wformat=2 -std=c11`. Keep
the build free of warnings.

## Testing

There is no unit-test framework yet. Before submitting: `make syntax`,
`make asan` with a run against a real neighbour, and a capture of the
link showing the frames you changed. Propose a test framework before
adding ad-hoc test code (see [ROADMAP.md](ROADMAP.md)).

## Rules

1. **Capture first, then code.** Every wire format must come from a
   capture of real peers and be re-verified with a capture after the
   change. A wrong byte produces silence, not an error: peers keep no log
   you can read.
2. **Cite every wire constant** — a section of
   [PROTOCOL_SPEC.md](PROTOCOL_SPEC.md) or a captured frame — and
   document the byte layout above every builder and parser.
3. **Describe peers in observation terms** in anything public: "captures
   show that the peer…", never claims about another implementation's
   internals.
4. **No credentials** anywhere in the repository.
5. **PROTOCOL_SPEC.md is shared with linbpq-flexnet** and must stay
   identical in both repositories. Change it in one, copy it to the other.

## Code conventions

C11, 4-space indent, 100 columns, `snake_case`, file-local functions
`static`. Bounded string functions only (`snprintf`, `strncpy`). Comments
explain *why* — an invariant or an observed peer behaviour.

## Releases

1. Bump `FLEXNETD_VERSION` in `flexnetd.h`.
2. Update the version in the README title, add an entry to
   `RELEASE_NOTES.md`, update `ROADMAP.md`.
3. Run the release against live neighbours before tagging.
4. Annotated tag `vX.Y.Z`, push, publish a GitHub release from the
   release-notes entry.
