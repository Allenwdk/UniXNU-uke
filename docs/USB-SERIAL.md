# USB serial debug channel (uke)

Status: **usbbr10..16 (2026-10-25 night) closed every theory except one.
The rails/xo.lvl/EUD_DETECT/200MHz-RCG votes and the GCTL.CORESOFTRESET are
all in place, yet both reset variants (DCTL.CSFTRST and GCTL
CORESOFTRESET, bit 11, 100 ms pulse) never complete in the handoff state —
they only release "once all clocks are synchronized", and one clock into
the core (almost certainly the PHY UTMI clock) never synchronizes. ABL's
handoff itself is K70-grade (GDSC on, branches on, master RCG already at
200 MHz = 0x00100105, TCSR clkref gate reads 1). The RPMH read path is
unusable (cx.lvl/mx.lvl read 0 while the CPU runs on them — AOP does not
service runtime reads), so whether the VRM/ARC SET votes actually apply is
THE open question; the ground-truth probe is a PMIC SPMI read of the
L2B/L4B status registers through the SCM SIO service. A stuck reset also
poisons the device domain (GEVNTADR/DEPCMD/DALEPENA writes drop, `ep0
done` becomes a false success), so `native-usb-softreset` and
`native-usb-gctlreset` both stay off by default.** No `/dev/ttyACM0` yet.

## Why

uke has no usable UART. The kernel console currently lands in the
framebuffer, and a black screen or reset loses the last lines. The USB-C
port is the only external debug transport. The target is a standard CDC-ACM
port (Linux `/dev/ttyACM0`, macOS `/dev/cu.usbmodem*`) carrying the kernel
`msgbuf`, so the host can capture live logs with `tio`/`picocom`.

## State by layer (2026-10-25, after usbbr16)

| Layer | State | Evidence |
| --- | --- | --- |
| GCC power/clock tree | ✅ on | GDSCR `f822f800` (PWR_ON), all five branch CBCRs `00000001`, master RCG `0x00100105` = GPLL0/6 = 200 MHz left by ABL, TCSR clkref gate reads 1 through SCM |
| DWC3 register file | ✅ alive | DCFG/DEVTEN/GCTL writes hold and read back |
| `DCTL.CSFTRST` device reset | ❌ never releases | stuck `dctl=40000000` across every variant (alone, with RUN cleared in the same write, QSCRATCH retiming pre-set, manual clear retries) |
| `GCTL.CORESOFTRESET` (bit 11) | ❌ never releases | usbbr16: 100 ms pulse, bit does not read back set, domain ends up MORE poisoned (RUN_STOP stops latching, `run dctl=00f00000`) |
| Endpoint commands | ❌ stall | `DEPSTARTCFG` (cmd 9) completes; the first real command after it hangs with CmdAct (`DEP0 cmd=2 readback 00000402`), SETEPCONFIG-first and SETTRANSFRESOURCE-first both tried |
| Event ring | ❌ silent | `gevt=00000000` forever; host never sees our device (only the ABL-era full-speed connect + `-71` ~1 s after each reboot) |
| eUSB2 PHY register file (MMIO) | ✅ alive | the 38.4 MHz table writes and reads back (`common0=4b4b4b4b`, `ctrl2=c8c8c8c8`) |
| PHY internal APB bridge | ❌ no echo | status stays 0, RDDATA stays 0 in every state produced (pure warm, after rails votes, after BCR + table) — the PHY digital core (PLL → UTMI clock) is down or unclocked |
| PMIC rails L2B/L4B + ARC xo.lvl | ❓ unknown | TCS votes complete `idle=1`, but runtime reads are unusable (below), so "applied" vs "silently dropped" cannot be told apart yet |

**Synthesis:** the handoff state is complete on the clock tree and the DWC3
register file, but one clock into the core — almost certainly the PHY's
UTMI clock — never arrives, so both reset variants wait for "all clocks
synchronized" forever and the endpoint engine never runs. The only
unverified link in the chain is whether the eUSB2 rails / xo.lvl votes
actually apply.

## Eliminated theories (do not revisit)

| Theory | Killed by |
| --- | --- |
| 19.2 vs 38.4 MHz reference frequency | usbbr3 fixed the table (`9ae353a`); nothing unlocked |
| TCSR clkref wrong address / gate off | real gate `0x1fb1118` reads 1 through SCM (usbbr9); writing it (CPU or SCM) changes nothing |
| Master/core clock parked at 19.2 MHz | usbbr13: RCG already `0x00100105` (200 MHz) from ABL; our write is a no-op |
| `GUSB2PHYCFG.SUSPHY` set | vendor defines SUSPHY = BIT(6); warm `usb2=0x2222000e` never had bit 6 set |
| WCD939x repeater is a USB prerequisite | stock DT: no USB node references any repeater; `wcd939x_i2c@e` is referenced only by the sound card |
| Reset write-encoding variants | alone / with RUN_STOP in the same write / QSCRATCH retiming pre-set / clear-and-retry / GCTL CORESOFTRESET — all fail identically |
| APB echo false-negative from 4-bit WRDATA fields | usbbr12 switched to 0x5/0xa with the nibble split; still silent |

## Operational gotchas

- **Never enable `native-usb-softreset` or `native-usb-gctlreset`.** A
  stuck reset poisons the device domain: GEVNTADR/DEPCMD/DALEPENA (and
  after GCTL reset even RUN_STOP) writes are dropped, `ep0 done` becomes a
  false instant success and every downstream signal is meaningless.
- **Diagnostic payloads must pass `offline-legacy-noconsole=0`** — the
  board's default `=1` mutes the panel after the first copied frame and
  the `serial:` stage never reaches the screen.
- **`fastboot set_active a` after every flash**: XNU never marks the boot
  successful, so ABL's slot retries exhaust after a few flash/reboot
  cycles and the device drops to fastboot ~5 s after `fastboot reboot`.
- **`unixnu.toml` must stay under ~1024 B** or the loader silently drops
  the whole args line.
- **Panel reading**: photos are the only device-side channel (no UART);
  consecutive photos capture different scrolling segments, so anchor photo
  requests to a specific line (the `rails: end` tail line is the stable
  anchor), and treat OCR'd hex on dense lines as unreliable — the driver
  prints a consolidated tail line for exactly this reason.

## Image lineage (2026-10-25 night, all `--base vendor_boot_uke_full.img`)

| Image | sha256 | Args beyond the board set | What it proved |
| --- | --- | --- | --- |
| usbbr10 `1aa8a1be…` | kernel `ebf81196…` | `rails=1 phy=1 softreset=1` | rail votes complete `idle=1`; APB echo still dead; noconsole hides `serial:` |
| usbbr11 `ce68ea35…` | | + `xo.lvl=3`, EUD_DETECT, `noconsole=0` | still no-echo; stuck CSFTRST poisons the domain (event adr=0) |
| usbbr12 `4826bc96…` | kernel `969bb4f6…` | `softreset=0`, 4-bit echo, `rcg=1` | domain coherent (event adr latches); `DEP0 cmd=2` still stuck; RCG already 200 MHz |
| usbbr13 `9d03406d…` | | + RPMH read-backs, tail line | reads all 0 — including rails; RCG before==after |
| usbbr14 `c42f6552…` | | + cx.lvl/mx.lvl known-on reads | known-on reads ALSO 0 ⇒ runtime reads unusable, zeros prove nothing |
| usbbr15 `3fffe7b3…` | | + polls counter | `p=0/0/0` ambiguous by design (first poll precedes first sleep; µs round trip) |
| usbbr16 `fb11b4aa…` | | + `gctlreset=1` | GCTL CORESOFTRESET never completes either; poisons harder (RUN_STOP drops) |


## Hardware facts (as verified 2026-10-25)

| Item | Value |
| --- | --- |
| SoC | SM7675 = Qualcomm "Cliffs" (stock trees: `Xiaomi_Kernel_OpenSource-uke`, dtb dumps in `android_device_xiaomi_uke-kernel/dtbs`) |
| DWC3 core | `0xa600000`, DWC_usb31 (`GSNPSID 0x33313130`); USB2-only board, no pipe3 |
| QSCRATCH | `0xa6f8800` (wrapper + `0xf8800`), size `0x400` |
| eUSB2 PHY wrapper | `0x88e3000`; the register file is **8-bit wide** — 32-bit reads come back byte-replicated |
| EUD | enable `0x88e2000`, detect `0xc278000`; reads `0` (inactive) |
| GCC base | `0x100000`; USB30 page mapped at `0x139000` |
| USB GDSC | `0x139004` (reads `f822f800` in the warm state = on) |
| USB30 reset | `0x139000` bit 0 (`GCC_USB30_PRIM_BCR`) |
| eUSB2 PHY reset | `0x112000` bit 0 (`GCC_QUSB2PHY_PRIM_BCR`) |
| USB branch gates | master `0x139018`, sleep `0x139024`, UTMI `0x139028`, cfg-noc `0x139088`, aggre `0x13908c` (enable = bit 0) |
| USB RCGs | master src `0x3902c`, mock-utmi src `0x39044` + postdiv `0x3905c` (both `HW_CLK_CTRL_MODE`, parked at CXO = 19.2 MHz — the parked state is the correct rate) |
| TCSR USB2 clkref | **`0x1fb1118`** (tcsrcc `0x1f40000` + `0xb1118`, `qcom,pineapple-tcsrcc`) — **reads `1` through SCM: the gate is on** |
| TCSR reachability | every CPU-side access to the TCSR region **stalls the bus** (`0x1fb1118` write, `0x1fcb118`/`0x1f4b118` reads); only the ADSP `ipc_hwmutex` window (`0x1f4a000+0x2000`, per DT `hwmutex_mappings`) accepts writes |
| eUSB2 reference clock | **38.4 MHz** (DT `xo_board` = 76.8 MHz, `RPMH_CXO_PAD_CLK` = ARC/2), **not** the 19.2 MHz mainline boards use |
| PHY PLL constants | FSEL `4` (bits 6:4 of COMMON0), `PLL_FB_DIV_7_0 = 0xc8`, `DIV_11_8 = 0x0` (vendor `phy-msm-snps-eusb2.c` 38.4 MHz pair) |
| SMMU | apps-SMMU `0x15000000`, DWC3 stream `0x40` (bypassed in software) |
| DWC3 IRQ | SPI 133 |
| Repeater (line side) | WCD939x on GENI I2C `0xa80000` addr `0x0e`, reset GPIO 119 |
| PHY rails | vdd = pmxr2230 L2B 0.88 V, vdda12 = L4B 1.2 V (rpmh resources) |
| AOP command DB | `0x81c60000`/`0x20000` (`aop_cmd_db_region`), magic `db 30 03 0c`, VRM block slv=4; ARC slv=3, BCM slv=5 |
| apps_rsc | `0x17a00000`, drv-2 at `0x17a20000` (`qcom,tcs-offset 0xd00`): ACTIVE x3 (TCS 0-2), SLEEP x2, WAKE x2, FAST x1; per-TCS/per-command offsets from `RSC_DRV_ID` (major >= 3: stride 672/cmd 24, TCS regs WAIT 0x20 CONTROL 0x24 STATUS 0x28 ENABLE 0x2C, CMD MSGID 0x34/ADDR 0x38/DATA 0x3C/STATUS 0x40/RESP 0x44); AMC = CONTROL BIT16/BIT24 |
| SCM SIO | `smc #0` FAST\|SMC64\|SIP\|FNID(`SVC_IO 0x05<<8` \| `IO_READ 0x01`/`IO_WRITE 0x02`), arginfo `0x1`/`0x2` — **works from XNU** (`scmSmc()` in the driver) |

Sources: stock uke DT + dtbo, the vendor kernel (`dwc3-msm-core.c`,
`phy-msm-snps-eusb2.c`, `gcc-cliffs.c`, `tcsrcc-pineapple.c`,
`clk-rcg2.c`), and the decompiled dtb dumps. Only register facts are
transcribed.

## Evidence chain (how the current picture was established)

1. **The endpoint commands never run.** `DEPSTARTCFG` (cmd 9) completes
   silently; the first real command after it hangs with `CmdAct` stuck
   (`DEP0 cmd=1` in the warm state, `cmd=2` in the vendor order), on the
   ABL warm state and on a freshly BCR-reset core alike.
2. **A stuck `CSFTRST` poisons the domain.** After it sticks (it never
   self-clears here — the databook releases it only once "all clocks are
   synchronized"), DEPCMD/DALEPENA/GEVNTADR writes stop latching and every
   later command "completes" as a dropped write. usbbr2 proved this on a
   fresh block; usbbr3+ removed the attempt.
3. **The GCC block reset itself works.** After the vendor
   `dwc3_msm_link_clk_reset` order (branch gates down, BCR pulse, gates up
   iface-first) the GDSCR reads back with `PWR_ON`, `GSNPSID` survives and
   the replayed registers hold.
4. **The PHY wrapper register file is alive and holds our values** — the
   byte-replicated reads decode to exactly the programmed state
   (`COMMON0=0x4b`, `CFG_CTRL_2=0xc8`). But that only proves the config
   bus is mapped.
5. **The PHY's internal APB bridge is dead.** The write-read echo test
   (vendor CV9 pattern `0x5=0xc0`, `0x10=0xa5` through `0x138/0x13c/0x130`)
   returns silence, **both on the untouched ABL warm state and after the
   BCR pulse + init table** — so our reset is not the culprit: the PHY's
   clock domain (PLL + digital) never comes up.
6. **The reference-clock gate is not the problem.** Through the SCM SIO
   service TrustZone reads `0x1fb1118` as `1` (enabled) and the write
   completes — yet the echo stays dead. The gate was on the whole time.
7. **The TCSR region is unreachable from the CPU.** Every access outside
   the ADSP hwmutex window stalls (no abort, no panic — a NoC route with
   no answering target). The vendor DT's `tcsrcc@1f40000` window overlaps
   `tcsr_mutex_block@1f40000` (0x20000) and an undecoded hole before the
   real `tcsr: syscon@1fc0000` (0x30000); the stock kernel may never write
   it either.
8. **Remaining suspect: the PHY's rpmh rails.** XBL/ABL hold proxy votes
   for L2B/L4B through their boot; the stock kernel's rpmh driver takes
   over and the regulator framework re-votes immediately. Our XNU never
   votes, so if the handoff drops the proxies the rails fall and every
   analog block (PLL, APB bridge logic) dies while the NOC-backed wrapper
   file stays readable. This also explains why the historical full-speed
   connects were the **repeater's** line side, not the PHY's.

Note: the senemos mainline port (`Project-UkeUEFI/uke-linux/senemos-uke-kernel`)
documents a "clock handoff" design (`CLK_HOLD_STATE`) assuming ABL leaves
the clocks on — but its own build report says `hardware_tested: false`, so
it is a design assumption, not evidence.

## Experiment ladder

All images flashed to both slots, base `vendor_boot_uke_full.img`,
69,005,312 B.

| Tag | Kernel | Change | Result |
| --- | --- | --- | --- |
| usbdal1..4b | — | warm handoff, reset ladder, DALEPENA drop, DEP_BASE/GUSB2PHYCFG base fixes | `DEP0 cmd=1 timeout`; softreset variants all stick |
| usbdal5..9 | `5a36792` | 19.2 MHz table era, DEPSTARTCFG-first, event machinery off | SETEPCONFIG hangs in the warm state |
| usbbr1 | `05a3bb0` | `DSBLCLKGTNG` unconditional + `native-usb-bcr=1` block reset (vendor order, qscratch restore, PHY replay) | block reset works, SETEPCONFIG still hangs on the fresh core |
| usbbr2 | `e74f760` | vendor command order, GRXTHRCFG bit26, MPS 512, post-BCR CSFTRST | CSFTRST **STUCK** on the fresh core, then the domain freezes (false `ep0 done`) |
| usbbr3 | `9ae353a` | 38.4 MHz PLL table, CSFTRST removed | poison gone (GEVNTADR holds), hang moves to cmd=2 |
| usbbr4 | (usbbr3+) | PHY state dump + APB probe v1 (bit0 poll) | table verified; `apb[00] TIMEOUT` |
| usbbr5 | `ecd834c` | clkref write at the corrected 0x1fb1118 | **store stalls the machine** — reverted to opt-in |
| usbbr6 | (usbbr5+) | APB probe v2 (bit3 poll, read RDDATA, 9 regs) | all reads `0000` — ambiguous (zero space) |
| usbbr7 | (usbbr6+) | APB write-read **echo** + TCSR read probes + hwparams | echo `no-echo`; `tcsr read 0x1fcb118` **stalls** |
| usbbr8 | (usbbr7+) | echo on the **pure warm** state, then after pulse+table | `no-echo` both — our reset is not the culprit |
| usbbr9 | (usbbr8+) | SCM SIO read/write of the clkref + re-echo | SCM works; clkref already `1`; echo still `no-echo` |

## What is already in the driver (pays off once the PHY is clocked)

- `GCTL.DSBLCLKGTNG=1` (the stock DT's `snps,disable-clk-gating`).
- `native-usb-bcr=1` block reset in the vendor `dwc3_msm_link_clk_reset`
  order, with QSCRATCH UTMI routing restore, `GUSB2PHYCFG`/`GUSB3PIPECTL`
  replay (suspend bits cleared) and a GSNPSID revive check.
- Vendor command order: `DEPSTARTCFG(0)` → `SETTRANSFRESOURCE` for every
  hardware endpoint (phys 0,1,3,5,6) → `SETEPCONFIG` pairs at MPS 512;
  `GRXTHRCFG.PKTCNTSEL` (bit 26) cleared; all endpoint commands run with
  `DEVTEN=0` (the vendor enables events last).
- The 38.4 MHz eUSB2 init table (`kUkeUsbPhyInit38_4Mhz`), applied with
  per-write readback, including the DT `param-override-seq <0x00 0x58>`.
- The APB echo test and `scmSmc()` SCM SIO wrapper (`native-usb-scm=1`).

## Boot-arg reference

Baked in `board.toml`: the UFS/sheng set + `native-usb-probe=0
native-usb-serial=1 native-usb-serial-dump=1`. Per-image experiment args
(passed with `--kernel-arg`): `native-usb-phy=1`, `native-usb-bcr=1`,
`native-usb-scm=1`, `sheng-build-tag=<name>`. Opt-in and off by default:
`native-usb-tcsr=1` (the clkref write — stalls, keep off),
`native-usb-softreset=1` (the CSFTRST ladder), `native-usb-fs=1`,
`native-usb-probe=1|2` (read-only dumps). Keep the final `unixnu.toml`
under 1024 bytes — a longer args line is silently dropped by the loader.

The USB payload also needs the UFS bring-up arguments the CPU baseline
uses, or the kernel panics with `Failed to mount root device` before the
driver is any use: `native-ufs-root=1 native-ufs-allow-unverified=1
sheng-data-persist=2 sheng-log=1 sheng-smp=0`. `sheng-log=1` keeps the
pram ramoops ring recording, which is the only way to read the log back
when the serial path itself is what fails.

## Build and flash (local Linux, minutes)

```sh
cd /home/allenwdk/UniXNU/UniXNU-linux
source scripts/linux-build-env.sh
export UNIXNU_HOST_COMPAT_INCLUDE=$HOME/.local/xnu-linux/include/compat
./unixnu defconfig uke-usb_defconfig -D ../UniXNU-uke-usbserial
./unixnu build kernel  -D ../UniXNU-uke-usbserial
# trust-bundle links live at the repo root (a defconfig run wipes
# out/uke/tree/refer):
ln -sfn ~/下载/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw refer
ln -sfn /home/allenwdk/UniXNU/UniXNU-uke/.cache/firmware/target-volumes/static-trustcache.bin \
        .cache/firmware/target-volumes/static-trustcache.bin
python3 scripts/build-original-system-trust.py   # prints bundle 7416ca4f…
./unixnu build payload -D ../UniXNU-uke-usbserial \
    --kernel out/uke/kernel/kernel.macho \
    --trust-cache out/uke/tree/build/original-static-system-trust/original-static-system.bundle \
    --data-ramdisk ../uke-port/data/data-seed-200/owner-data-200.dbr \
    --unique-chip-id 0x7252a8b07f8cb8f9 \
    --kernel-arg native-usb-phy=1 --kernel-arg native-usb-bcr=1 \
    --kernel-arg native-usb-scm=1 --kernel-arg sheng-build-tag=usbbrN
python3 ../UniXNU-uke-usbserial/scripts/patch-vendor-boot.py \
    --base ../uke-port/data/vendor_boot_uke_full.img \
    --payload out/uke/payload/payload.img \
    --output ../uke-port/data/vendor_boot_uke_usbbrN.img
# verify before flashing (toml must stay < 1024 bytes):
mtype -i out/uke/payload/payload.img ::/EFI/UniXNU/unixnu.toml | wc -c
fastboot flash vendor_boot_a ../uke-port/data/vendor_boot_uke_usbbrN.img
fastboot flash vendor_boot_b ../uke-port/data/vendor_boot_uke_usbbrN.img
```

Images so far: usbdal1 `ecc5bed5…` … usbdal9, usbbr1 `f13d8b84…`,
usbbr2 `c4a49381…`, usbbr3 `d2761e3c…`, usbbr4 `c6b8525c…`,
usbbr5 `75fa3b47…`, usbbr6 `e69cd445…`, usbbr7 `dc6c8c09…`,
usbbr8 `db4c4ecd…`, **usbbr9 `64328c88…` (current)**.

## Reading a boot

The panel carries the whole story; photograph after the boot settles.
Key lines and what they mean:

- `warm: apb echo (pure warm)` — the untouched-ABL bridge test;
  `ECHO` = the PHY is clocked, `no-echo` = it is not.
- `warm: scm read ret=… val=…` — TrustZone's view of the clkref word.
- `warm: block reset done gdscr=… bcr=…` / `warm: replay … id=33313130` —
  the block reset succeeded when GDSCR has `PWR_ON` and the id reads back.
- `serial: ep0 done gevt=…` — the endpoint command engine ran (only
  believable after a `GEVNTADR` readback that is non-zero).
- `DEP%u cmd=%x timeout readback=…` — the engine is stuck again; the cmd
  number says which command.
- The host side (`journalctl -k` on the attached PC) is the line-side
  truth: `new full-speed USB device` + `error -71` is the repeater pulling
  D+ without a working PHY; `new high-speed USB device` + `cdc_acm` is the
  goal.

A `serial: end …` tail line carries the full register state to any photo
after later boot output scrolls the earlier lines away. No panic
auto-restart: after a hang, enter fastboot manually (long power press +
volume down).

## Next steps (ranked)

1. **Ground-truth the rails: read the PMIC L2B/L4B status through the SCM
   SIO service.** The SCM io_read SMC (already working, usbbr9) can walk
   the apps SPMI PMIC arbiter; PMXR2230's LDO status registers then say
   definitively whether the rails are on. If off, the vote path is broken
   — the leading suspects become the missing SLEEP/WAKE-state votes
   (Linux's rpmh-regulator and clk-rpmh always program sleep+wake+active,
   we only send ACTIVE) or the DRV/channel choice; send the triple if so.
   If on, the missing clock is not the rails and the hunt moves to the
   UTMI clock routing into the core.
2. **Do not re-enable either reset** (`native-usb-softreset`,
   `native-usb-gctlreset`): usbbr11/usbbr16 proved a stuck reset poisons
   the whole device domain (GEVNTADR no longer latches, every DEPCMD
   "completes" as a dropped write, `serial: end` shows adr/dalepena/depcmd
   all zero) and destroys every downstream diagnostic.
3. **The repeater is NOT a USB-stack prerequisite on this board**: the
   stock DT's USB nodes reference no repeater anywhere — the only consumer
   of `wcd939x-i2c@e` is the sound card. The vendor cold path for uke is
   exactly rails → clocks (xo.lvl + TCSR gate) → BCR → register table,
   all of which this driver now replays.
4. **K70 endpoint order** (SETEPCONFIG before SETTRANSFRESOURCE per
   endpoint, bulk endpoints configured on SET_CONFIGURATION with
   `DEPSTARTCFG|xfer_rsc_idx=2`) is worth adopting wholesale once any
   command at all survives — with the domain un-poisoned every command
   after DEPSTARTCFG still stalls with CmdAct (`DEP0 cmd=2 readback
   00000402`, usbbr12..15).
5. **Slot-retry gotcha**: XNU never marks the boot successful, so several
   flash/reboot cycles exhaust ABL's slot retries and the device drops to
   fastboot ~5 s after `fastboot reboot`; run `fastboot set_active a`
   after flashing to reset the counter.

Historical note: the first revision's `0x1f4b118` "clkref" was an
arithmetic slip (`0x1f40000 + 0xb1118` was mis-summed); that address is
the ADSP `ipc_hwmutex` window — writes are harmless, reads hang the
tablet. The eUSB2 table shipped with the 19.2 MHz PLL constants until
usbbr3; the reference clock is 38.4 MHz on this board.
