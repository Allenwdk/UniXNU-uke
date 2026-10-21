# NativeUsbSerial

uke (Xiaomi Pad 7, SM7675) USB2 peripheral debug driver.

**Current revision: read-only probe plus an opt-in warm CDC-ACM/msgbuf path.**
The probe maps the DWC3 wrapper, QSCRATCH, eUSB2 PHY, GCC/GDSC, TCSR and
apps-SMMU windows and dumps the state ABL/UEFI left behind. With
`native-usb-serial=1` the driver performs the warm handoff, enumerates a
CDC-ACM device and relays the kernel msgbuf. The warm path has not been
hardware-validated yet; the probe is the first hardware gate.

## Boot arguments

| Argument | Meaning |
| --- | --- |
| `native-usb-probe=1` | Read-only dump; no enumeration |
| `native-usb-serial=1` | Warm handoff, CDC-ACM enumeration and msgbuf relay |
| `native-usb-serial-log=1` | Reserved: msgbuf relay |
| `native-usb-serial-dump=1` | Reserved: send the full msgbuf |
| `native-usb-serial-cold=1` | Reserved: WCD939x/repeater cold init |

Without either `native-usb-probe` or `native-usb-serial`, `start()` returns
false and no hardware is touched.

## Probe output

`IOLog` lines start with `NativeUsbSerial:` and also land in msgbuf/framebuffer.
They cover:

- `GCC gdscr/cfg` and the USB branch gates (`master`, `sleep`, `utmi`,
  `cfg_noc`, `aggre`) plus the two BCRs and the TCSR USB2 clock-ref gate.
- DWC3 `GSNPSID`, `GUID`, `GCTL`, `GSTS`, `GHWPARAMS0..3`, `GUSB2PHYCFG`,
  `GUSB3PIPECTL`, `DCFG`, `DCTL`, `DEVTEN`, `DSTS` and the event ring.
- QSCRATCH `GENERAL_CFG` and `HS_PHY_CTRL`.
- eUSB2 PHY registers and the EUD enable/detect registers.
- apps-SMMU IDR/SCR/GFSR, every valid SMR/S2CR (the USB stream `0x40` is
  marked), and the top bypass bank's SCTLR/CBAR/CBA2R.

## Build

```sh
python3 scripts/build-arm-driver.py NativeUsbSerial
```

The archive is `build/native-usb-serial/libNativeUsbSerial.a`. The kernel link
uses `-Wl,-force_load` and the `KernelConfigTables.cpp` personality from
`patches/xnu/zzzzzzzzz-native-usb-serial.patch`.

Host-side pure logic (descriptors, TRB/byte rings, msgbuf cursor, PHY table):

```sh
drivers/NativeUsbSerial/tests/run.sh
```

## Board facts and license

`UkeUsbBoard.h`, `UkeUsbGcc.h`, `UkeUsbPhy.h` and `UkeUsbDwc3.h` transcribe
addresses, offsets and bit values from the stock uke DT, the uke-mainline USB2
overlay (GPL-2.0-only OR BSD-2-Clause), and Linux gcc-cliffs/tcsrcc/gdsc/DWC3
sources (GPL-2.0). No Linux driver code is copied. The driver itself is
BSD-2-Clause-Patent; see `LICENSE`.
