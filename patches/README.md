# URONode Patches for FlexNet Support

These patches are applied to [URONode](https://github.com/Online-Amateur-Radio-Club-M0OUK/uronode)
to enable digipeater path preservation when connecting to FlexNet destinations.

## uronode-m2-digipeater-path.patch

**File:** `gateway.c`
**Applies to:** URONode master (tested against commit `e0c14b4`)
**Status:** Production-tested on IW2OHX since 2026-04-14

### What it does

When a URONode user connects to a FlexNet destination (e.g., `c ir5s`),
URONode builds an AX.25 SABM with a digipeater via-list. This patch
ensures our node's callsign appears in that via-list with the H bit
(has-been-repeated) correctly set, so:

1. The destination node sees our callsign in the path
2. The FlexNet neighbor (xnet) correctly processes the frame

### The problem

Without this patch, outbound FlexNet connects produce:
```
fm IW7CFD-15 to IR5S via IW2OHX-3 IW2OHX-14 ctl SABM+
```
Note: no `*` on IW2OHX-3. The neighbor (IW2OHX-14/xnet) sees an
unrepeated digi that isn't its own callsign, and drops the frame.

### Root cause

The Linux kernel's `ax25_connect()` (in `net/ax25/af_ax25.c`) has a
two-condition gate for honoring H bits from userspace:

```c
if ((fsa->fsa_digipeater[ct].ax25_call[6] & AX25_HBIT) && ax25->iamdigi)
    digi->repeated[ct] = 1;    /* H bit honored */
else
    digi->repeated[ct] = 0;    /* H bit CLEARED */
```

Simply setting `fsa_digipeater[0].ax25_call[6] |= 0x80` is not enough.
The socket must also have `AX25_IAMDIGI` set via `setsockopt()`.

### The fix

Three changes to `gateway.c`:

1. **`#define AX25_IAMDIGI 12`** fallback (may be missing from older
   libax25 headers)

2. **`setsockopt(fd, SOL_AX25, AX25_IAMDIGI, &1, sizeof(1))`** before
   `connect()` for `AF_FLEXNET` family connections — tells the kernel
   to honor H bits from userspace

3. **`fsa_digipeater[0].ax25_call[6] |= 0x80`** — sets the H bit on
   our callsign (first digi in the via-list, already-repeated)

Plus a bonus fix: the `do_connect()` argv building loop had undefined
behavior (`k++` used as both index and increment in the same expression).
Replaced with a clean for-loop.

### Result

After the patch:
```
fm IW7CFD-15 to IR5S via IW2OHX-3* IW2OHX-14 ctl SABM+
```
The `*` on IW2OHX-3 means H bit is set. xnet processes the frame,
connection succeeds, and the destination's `U` command shows:
```
IR5S>IW7CFD-15 v IQ5KG-7 IW2OHX-3
```

### How to apply

```bash
cd /path/to/uronode-source/
git apply /path/to/uronode-m2-digipeater-path.patch
make clean && make
sudo make install
```

Or manually with `patch`:
```bash
cd /path/to/uronode-source/
patch -p1 < /path/to/uronode-m2-digipeater-path.patch
```

### Prerequisites

This patch works together with flexnetd's gateways file, which must
include our callsign as a digipeater:
```
addr  callsign  dev  digipeaters
00000 IW2OHX-14 ax1 IW2OHX-3
```

flexnetd v0.5.0+ writes this automatically when `FlexListenCall` is
configured.
