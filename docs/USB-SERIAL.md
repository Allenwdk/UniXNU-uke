# USB serial debug channel (uke)

Status: **read-only probe plus the warm CDC-ACM/msgbuf implementation are on
`uke-usbserial`; the serial path is opt-in and awaits hardware validation.**

## Why

uke has no usable UART. The kernel console currently lands in the framebuffer,
and a black screen or reset loses the last lines. The USB-C port is the only
external debug transport. The target is a standard CDC-ACM port (Linux
`/dev/ttyACM0`, macOS `/dev/cu.usbmodem*`) carrying the kernel `msgbuf`, so
the host can capture live logs with `tio`/`picocom`.

The first milestone is a read-only probe: it tells us whether ABL/UEFI leaves
the DWC3, eUSB2 PHY, GCC/GDSC and apps-SMMU in a state we can take over. No
register is written and no DMA is allocated until that is known.

## Hardware facts

| Item | Value |
| --- | --- |
| DWC3 wrapper | `0xa600000`, size `0x100000` |
| QSCRATCH | `0xb58800`, size `0x400` |
| eUSB2 PHY | `0x88e3000`, size `0x154`; EUD at `0x88e2000`/`0xc278000` |
| USB GDSC | `0x139004` / `0x139008` (GCC window `0x100000`) |
| USB30 reset | `0x139000` bit 0 (`GCC_USB30_PRIM_BCR`) |
| eUSB2 PHY reset | `0x112000` bit 0 (`GCC_QUSB2PHY_PRIM_BCR`) |
| USB clocks | master `0x139018`, sleep `0x139024`, UTMI `0x139028`, cfg-noc `0x139088`, aggre `0x13908c` (all bit 0) |
| TCSR USB2 clock ref | `0x1f4b118` bit 0 |
| apps-SMMU | `0x15000000`, DWC3 stream `0x40` |
| DWC3 IRQ | SPI 133 |
| eUSB2 ref clock | 19.2 MHz |
| WCD939x (cold path) | I2C `0xa80000` addr `0x0e`, reset GPIO 119 |
| PMIC repeater (cold path) | SPMI addr `0xfd00` |

Sources: stock uke DT, the uke-mainline `sm7675-uke-usb2.dtso` overlay
(`GPL-2.0-only OR BSD-2-Clause`), and Linux `gcc-cliffs.c`, `tcsrcc-uke.c`,
`gdsc-regulator-uke.c`, `phy-msm-snps-eusb2-uke.c`, `dwc3-qcom.c` (GPL-2.0).
Only register facts are transcribed.

## Probe boot

The board file currently adds `native-usb-probe=1`. The driver matches the
ADT `usb` node (`compatible = "qcom,dwc-usb3-msm"`), maps the windows and
prints one-line summaries through `IOLog`:

- GCC GDSC and branch gates, BCRs and the TCSR clock-ref gate.
- DWC3 `GHWPARAMS`, `GCTL/GSTS`, `DCFG/DCTL/DEVTEN/DSTS`, event ring.
- QSCRATCH `GENERAL_CFG`/`HS_PHY_CTRL`.
- eUSB2 PHY register snapshot and EUD state.
- apps-SMMU IDR/SCR/GFSR, valid SMR/S2CR (USB stream `0x40` is marked), and
  the top bypass bank state.

Because there is no serial console yet, read the lines on the framebuffer
diagnostic page or take a photo after boot. A later revision can also write
the probe result into the existing pram log ring.

### Interpreting the probe

- `GCC gdscr` bit 1 set = hardware-controlled GDSC; the warm driver refuses
  to touch it. `cfg` bit 16 set = powered up.
- `DWC3 dcfg` speed bits 2:0 should be 0 (HS); `dsts` bit 22 clear = controller
  not halted; `dctl` bit 31 = run/stop.
- `GUSB2PHYCFG` bit 6/8 should be clear for the LPM/suspend quirks.
- `SMMU ... (usb)` line means stream `0x40` already has a valid SMR. If it
  points at a translated context, the bypass planner will refuse and print the
  existing SMR/S2CR instead of overwriting it.
- A zero/`0xffffffff` PHY snapshot usually means the PHY rail or reference
  clock is off; that is the signal to add the cold-init path.

## Warm CDC-ACM + msgbuf (implemented, opt-in)

1. Warm init: GDSC on, USB branch gates/resets, eUSB2 PHY 19.2 MHz sequence,
   QSCRATCH UTMI pipe clock + VBUS override, DWC3 soft reset, apps-SMMU
   stream `0x40` bypass, physically-contiguous DMA.
2. DWC3 device mode: event ring, EP0 control, EP1 notification, EP2 bulk IN,
   EP3 bulk OUT; CDC-ACM descriptors from `UsbDescriptors.h`.
3. A kernel thread snapshots `msgbuf` under `bsd_log_lock_safe()` and pushes
   new bytes to EP2; `native-usb-serial-dump=1` sends the full ring.
4. Host: `dmesg` shows `cdc_acm`, then

   ```sh
   tio -b 115200 /dev/ttyACM0
   ```

   The baud rate is nominal for CDC-ACM; the device ignores it.

The default board command line still selects `native-usb-probe=1`; add
`native-usb-serial=1` (for example with `--kernel-arg native-usb-serial=1`) to
start the warm path. Both arguments together dump the probe first and then
enumerate.

The first version deliberately does **not** become the `kprintf` console and
does **not** implement KDP. Those need early-boot or polled paths and are a
separate step. USB3/QMP, host mode, Type-C/UCSI and suspend/wake are out of
scope.

## If warm handoff fails

The mainline profile keeps the WCD939x USB2 route and the PMIC eUSB2 repeater
configured through Linux drivers. If the probe shows those are not live, add
the cold path:

- GENI I2C at `0xa80000` (addr `0x0e`) and TLMM GPIO 119 for WCD939x;
- SPMI writes to the PMIC repeater at `0xfd00` (device-mode
  `qcom,param-override-seq`, then poll `RPTR_OK`).

The uke-mainline patch series documents the exact register values; only facts
are transcribed into XNU.

## Build and tests

```sh
python3 scripts/build-arm-driver.py NativeUsbSerial
drivers/NativeUsbSerial/tests/run.sh
```

The archive is linked with `-Wl,-force_load` and the personality from
`patches/xnu/zzzzzzzzz-native-usb-serial.patch`. The kernel is built in GitHub
Actions; the local Linux host only runs the pure C++ tests.
