# USB serial debug channel (uke)

Status: **the warm CDC-ACM path now gets through GDSC, clocks and the GCC
resets on hardware and stops at the DWC3 core soft reset; the ordering fix
(`7a29ee1`) is built but not yet booted.** The read-only probe and the warm
CDC-ACM/msgbuf path are built into the full kernel; no `/dev/ttyACM0` has
appeared yet.

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
| QSCRATCH | `0xa6f8800` (wrapper + `0xf8800`), size `0x400` |
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

## First hardware result (2026-10-23)

`native-usb-serial=1` on `uke-usb_defconfig` printed

```text
NativeUsbSerial: start probe=0 serial=1
NativeUsbSerial: warm: gdsc
NativeUsbSerial: warm: clocks
NativeUsbSerial: warm: bcr
NativeUsbSerial: warm: dwc3-reset
NativeUsbSerial: DWC3 soft reset did not complete
NativeUsbSerial: warm init failed
NativeUsbSerial: serial start failed
```

so GDSC, the branch gates and the GCC resets are all fine, and the TCSR word
was never touched. Two ordering bugs are the likely cause and are fixed in
`7a29ee1`:

- the QSCRATCH UTMI pipe-clock select ran *after* `DCTL.CSFTRST`; the wrapper
  has to hand the core its pipe clock first (`dwc3-qcom` order);
- the eUSB2 PHY reset pulse ran without the reprogramming table, which wipes
  what ABL left in the PHY register file. The pulse now only happens with
  `native-usb-phy=1`.

`GUSB2PHYCFG.SUSPHY` is also cleared now, per the DT's
`snps,dis_u2_susphy_quirk`.

## Second hardware result: the reset is real, the QSCRATCH address was not

With the register dump in place the same boot printed

```text
warm: dwc3-reset gsnpsid=33313130 gctl=00102000 gsts=7e000000 dctl=00f00000 gusb2phycfg=00102500
DWC3 soft reset did not complete (dctl=40f00000 gsnpsid=33313130 gsts=7e000000 dsts=00400004)
```

`dctl` reads back `00f00000` and, after the write, `40f00000`: bit 30 stuck,
which means the register file is live and writable and the core simply never
finishes the reset. `dsts=0x00400004` reports `CONNECTSPD=4` (SuperSpeed) and
`DEVCTRLHLT`, the state ABL left the controller in.

Before that, both boots died at `warmInit+0x21c` with `esr=0x96000010`. DFSC
`0x10` is a *synchronous external abort*, not a translation fault, so the
mapping was valid and the physical page did not answer: the transcribed
QSCRATCH address `0x0b58800` is simply not a register block. `dwc3-qcom.c`
maps QSCRATCH at `core_base + 0xf8800` (`SDM845_QSCRATCH_BASE_OFFSET`,
`0x400` bytes), so the page is `0x0a6f8800`, inside the wrapper window the DT
describes. The uke overlay enables `qcom,select-utmi-as-pipe-clk`, and the
driver implements it as three writes to `GENERAL_CFG`: set
`PIPE_UTMI_CLK_DIS` (bit 8), then set `PIPE_UTMI_CLK_SEL` (bit 0) with
`PIPE3_PHYSTATUS_SW` (bit 3), then clear bit 8. The warm path now does exactly
that at the corrected address, before `DCTL.CSFTRST`, because the core cannot
finish its soft reset without the pipe clock.

## Where the link stands

`DCTL.CSFTRST` never self-clears on this board: not after picking the UTMI pipe
clock, and not after stopping the running core (`RUN_STOP=0`,
`DSTS.DEVCTRLHLT=1`) first. It is now off by default and the warm path keeps
ABL's core state.

A full GCC `USB30_PRIM_BCR` assert/deassert was tried as a substitute block
reset. It re-gated the branch clocks, so the QSCRATCH page read back zero
afterwards until they were re-enabled, and more importantly the host never saw
a connection again. The assert is removed; only the release plus a branch-clock
re-enable remain.

With the PHY initialised (`native-usb-phy=1`) the host does log a device:

```text
usb 3-1: new full-speed USB device number 115 using xhci_hcd
usb 3-1: device descriptor read/64, error -71
```

but the controller side reports

```text
serial: run     dctl=80f00000 dsts=00120000
serial: run+1s  dctl=80f00000 dsts=00120000 devten=09000000f gevt=00000000
```

i.e. RUN_STOP is set and DEVTEN is written, yet `CONNECTSPD=0` and
`GEVNTCOUNT=0`: no connection is latched and no event is generated, so
`handleSetup()` never runs and the host times out on the first descriptor.
The link is also **full speed** while `DCFG` asks for high speed, which is what
a PHY without its 19.2 MHz reference looks like: `dwc3_qcom_select_utmi_clk()`
only routes the clock, `TCSR_USB2_CLKREF_EN` (`0x1f4b118` bit 0) feeds it.
`native-usb-tcsr=1` enables that bit write-only (the read of the same page
hangs the tablet) and `native-usb-fs=1` advertises full speed instead of high
speed, so one build can test both.

## Warm CDC-ACM + msgbuf (implemented, opt-in)

1. Warm init: GDSC on, USB branch gates/resets, QSCRATCH UTMI pipe clock,
   DWC3 soft reset, apps-SMMU
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
enumerate. `native-usb-probe=2` additionally reads the DWC3 wrapper and
QSCRATCH, which is the most a read-only boot can tell us.

`native-usb-phy=1` programs the eUSB2 PHY register file and `native-usb-tcsr=1`
touches the TCSR clock-ref word at `0x1f4b118`. Both are opt-in and off by
default: the first probe boot died on the log line that read TCSR, and the
stock DT assigns `0x1f4b000` to the ADSP as its `ipc_hwmutex` page even though
the address also falls inside the `tcsrcc` window.

The USB payload also needs the UFS bring-up arguments the CPU baseline uses,
or the kernel panics with `Failed to mount root device` before the driver is
any use: `native-ufs-root=1 native-ufs-allow-unverified=1
sheng-data-persist=2 sheng-log=1 sheng-smp=0`. `sheng-log=1` keeps the pram
ramoops ring recording, which is the only way to read the log back when the
serial path itself is what fails.

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

CI run `39663424201` (`build-uke-kernel`, branch `uke-usbserial`) is green:
`archive-10-build-arm-driver.log` reports
`NativeUsbSerial: -Werror build and actual ARM64/iOS 17 object audit passed`,
and the 52,146,176-byte kernel contains the `NativeUsbSerial` methods, the
boot-arg strings and the `'IOClass' = NativeUsbSerial` personality. The
downloaded artifact is at
`/home/allenwdk/uke-port/ci/uke-kernel-full-usbserial/kernel.macho`
(sha256 `2d31d5c03d22e76abc86d451a7f39f50e79a966b31091db26863744b0795c6f1`).
