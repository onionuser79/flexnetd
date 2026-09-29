# URONode patch for FlexNet

## uronode-m2-digipeater-path.patch

| | |
|---|---|
| **File patched** | `gateway.c` |
| **Applies to** | [URONode](https://github.com/Online-Amateur-Radio-Club-M0OUK/uronode) master, tested at commit `e0c14b4` |
| **Needed for** | Outbound FlexNet connections from URONode users |

### What it does

When a URONode user connects to a FlexNet destination, URONode opens an
AX.25 connection through the FlexNet neighbour, using the digipeater
list from flexnetd's `gateways` file:

```
addr  callsign  dev  digipeaters
00000 NODEB-14  ax1  NODEA-3
```

The node's own callsign (`NODEA-3`) must go out marked as already
repeated, so the neighbour sees which node is relaying the user and
treats itself as the next hop:

```
without the patch   USER-15 -> DEST via NODEA-3 NODEB-14    SABM   (neighbour drops it)
with the patch      USER-15 -> DEST via NODEA-3* NODEB-14   SABM   (connects)
```

### Why a patch is needed

The Linux kernel's `ax25_connect()` honours a "repeated" (H) bit supplied
by user space only if the socket has `AX25_IAMDIGI` set; otherwise it
silently clears it:

```c
if ((fsa->fsa_digipeater[ct].ax25_call[6] & AX25_HBIT) && ax25->iamdigi)
    digi->repeated[ct] = 1;
else
    digi->repeated[ct] = 0;
```

The patch makes three changes to `gateway.c`:

1. Defines `AX25_IAMDIGI` (12) if the libax25 headers lack it.
2. Sets `AX25_IAMDIGI` on the socket before `connect()` for FlexNet
   connections.
3. Sets the H bit on the first digipeater (the node's own callsign).

It also fixes undefined behaviour in `do_connect()`'s argument loop
(`k++` used as index and increment in one expression).

### Applying

```bash
cd /path/to/uronode-source
git apply /path/to/flexnetd/patches/uronode-m2-digipeater-path.patch
# or: patch -p1 < /path/to/flexnetd/patches/uronode-m2-digipeater-path.patch
make clean && make
sudo make install
```

### Verifying

Connect from URONode to a FlexNet destination and check the user list on
the destination node: the entry should show your node's callsign in the
path.
