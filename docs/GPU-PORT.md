# Adreno 732 (uke) GPU port

uke has an **Adreno 732** (`qcom,adreno-gpu-gen7-11-0`, chip id `0x43030b00`)
on SM7675. The native sheng stack was written around the Adreno 740; this page
records the A732 port and separates what is built and checked on the host from
what still needs a device boot.

The work lives on three branches:

| Repository | Branch | Contents |
| --- | --- | --- |
| `Allenwdk/UniXNU-uke` | `uke-gpu` | board values, A732 input record, GMU/vote header builder, kit/firmware wiring, `uke-gpu_defconfig`, CI |
| `Allenwdk/UniXNU` | `uke-gpu` | pins the NativeAdreno fork and points CI at this branch |
| `Allenwdk/NativeAdreno` | `uke-a732` | A740/A732 profiles, reg-name window selection, uke compatible strings, SM7675 counter offset |

The device `uke-gpu` workflows check out core `uke-gpu`. Before merging this
track into `main`/`uke`, merge core `uke-gpu` into `uke` (or restore the
workflow `ref`) and keep the NativeAdreno fork pin, because the A732 profile is
not in `UniXNU/NativeAdreno`.

## Known values

From `/soc/qcom,kgsl-3d0@3d00000`, `/soc/qcom,gmu@3d69000` and
`/soc/qcom,kgsl-iommu@3da0000` in the live uke DT, plus the public A732 catalog
entry in `MCC45TR/senemos-uke-kernel-mainline` patch
`0036-uke-adreno732-render.patch`:

| Item | uke / Adreno 732 |
| --- | --- |
| chip id | `0x43030b00` |
| model | `Adreno732` |
| compatible | `qcom,adreno-gpu-gen7-11-0` |
| family | `ADRENO_7XX_GEN2` (A740 family) |
| GPU / GMU / SMMU registers | `0x3d00000+0x40000` / `0x3d68000+0x37000` / `0x3da0000+0x40000` |
| GMU PDC | `0xb280000+0x10000` |
| GPUCC / GCC | `0x3d90000+0xa000` (`qcom,cliffs-gpucc`) / `0x100000` (`qcom,cliffs-gcc`) |
| stream IDs | user `0x00`, secure `0x02`, GMU `0x05` |
| GMEM | 1.5 MiB (`0x180000`) |
| GMU chip id / CGC mode | `0x70b0000` / `0x00020202` |
| preemption record | 2196 KiB (`0x225000`) |
| UBWC | encoder/decoder 4.x, swizzle levels 2+3, highest bank bit 16, macrotile |
| BCM policy | SH0 buswidth 16, MC0 buswidth 4, ACV fixed perfmode BIT(2) |
| firmware | `gen71100_sqe.fw`, `gen71100_gmu.bin`, `gen71100_zap.mbn` |
| ZAP reservation | `0x9899a000+0x2000` |
| AOP command DB | `0x81c60000+0x20000` |

The live GPU power table gives 9 OPPs: 950, 900, 835, 736, 684, 633, 500, 353
and 255 MHz, with `initial-pwrlevel=8` (`min_pwrlevel=8`, `max_pwrlevel=0`).
The GMU table gives 220 MHz / 550 MHz. `scripts/prepare-uke-gmu.py --derive`
converts the `qcom,gpu-pwrlevels` entries and the DDR bus table into the OPP and
bandwidth arrays the HFI vote builder consumes.

Firmware pins (collected from the rooted device on 2026-10-21):

| File | Bytes | sha256 |
| --- | --- | --- |
| `gen71100_sqe.fw` | 66,036 | `b6df660d…` |
| `gen71100_gmu.bin` | 80,680 | `435c7bac…` |
| `gen71100_zap.mbn` | 12,088 | `12949928…` |

## Implemented without a device

- `NativeAdrenoProfile` selects the A740 or A732 facts from `qcom,chipid`.
  The A732 profile records GMEM, GMU chip id/CGC mode, the 2196 KiB preemption
  record, UBWC parameters, firmware names and the `0x68000` GMU/GPU delta.
  Unknown chip ids are refused instead of inheriting a neighbour's values.
- The GPU service selects its windows by `reg-names`:
  - uke: `kgsl_3d0_reg_memory`, `rscc`, `cx_misc` on the GPU node;
  - sheng: `gpu`, `cx_mem` on the GPU node and `rscc` on the GMU node.
  The five-window GMU adapter is assembled in the `ShengGmu*` order.
- `qcom,gen7-gmu` and `qcom,kgsl-smmu-v2` are accepted beside the SM8550
  compatible strings.
- The GMU always-on counter offset is derived from the mapped bases: 0x14220
  on SM8550 and 0x16220 on SM7675.
- `scripts/prepare-uke-gmu.py` derives `scripts/data/uke-gpu-inputs.json` from
  the live DTB and the pinned mainline registers, then builds the three headers
  `ShengGmuSource.h`, `ShengGmuVotesSource.h` and
  `ShengGmuCommandDbSource.h`. CI runs only the second step; no firmware or
  device is needed.
- `build-native-adreno-kits.py` embeds the gen71100 files when they exist under
  `.cache/firmware/uke-gpu`, verifies every pin, and otherwise leaves the weak
  empty definitions so the GMU refuses at runtime instead of failing the link.
- `GpuPowerProbe.cpp` and `GpuPowerBoard.h` carry the Cliffs offsets
  (`CX_GDSCR 0x908c`, `GX_GDSCR 0x9034`, `AHB 0x90c8`, `CX_GMU 0x90e4`,
  GMU/HUB RCGs `0x9168`/`0x919c`, DDRSS GPU AXI `0x7115c`), the A732 ACTLR
  `0x32b`, the secure SID `2`, the ZAP window at `0x9899a000` and the A732
  device info (0x43030b00, 1.5 MiB GMEM). The probe is compiled only when
  `CONFIG_XNU_NATIVE_ADRENO=y`.
- `configs/uke-gpu_defconfig` is the buildable A732 configuration: the stage-6
  set plus `CONFIG_XNU_NATIVE_ADRENO=y`. The shipping full/userland configs stay
  off until a device boot qualifies the path.
- CI run `39685196474` (2026-10-22) built `uke-gpu_defconfig` green. The
  archive chain produced the A732 headers, the NativeAdreno service, 43 kit
  objects, the staged AGXAccelerator and the legacy display with
  `GpuPowerProbe.o`; the resulting `kernel.macho` is 52,416,512 bytes,
  sha256 `6883deac638d8e20479fc7559c692bd8e22720441d9e24f8d0582a8eadc43b67`.
  That run had no vendor firmware, so the weak empty images were linked, as
  intended.

Host checks (Linux, no Apple SDK):

```sh
python3 scripts/check-uke-gpu.py
python3 scripts/check-uke-gpu-host.py --kit ../UniXNU/drivers/NativeAdreno
python3 scripts/check-gpu-power-host.py
```

The first validates `board.toml` against the recorded DT facts and firmware
pins; the second compiles and runs `NativeAdrenoProfile`'s A740/A732 test and
checks the generated headers; the third runs the gated GPU power sequence over
a software register peer.

## Still needs a device

The port is build-ready, not hardware-qualified. The first device boot still has
to establish:

- the SMMU context bank, ASID and SMR slot assignment (the DT gives stream IDs
  only; the probe's bank 0/1 choices are not yet observed on uke hardware);
- the GPU CX/GX rail and clock votes for the Cliffs GPUCC, and whether the
  bootloader leaves the domains in the state the probe assumes;
- the AOP command DB version word and the actual rail/BCM payloads;
- the gen71100 GMU cold start (RSC/PDC writes, firmware load/readback, HFI
  handshake) and the CP ring/SQE start;
- ZAP authentication and the secure-mode exit;
- scanout from the GPU context into the uke panel, then
  NativeMetal/QuartzCore/Turnip.

The firmware files are private physical evidence and are not in Git. A CI build
without them links the weak empty firmware and the GMU cold start refuses, as
designed.

## Building and checking

The GPU configuration is built by the existing macOS kernel workflow:

```sh
gh workflow run build-kernel.yml -f defconfig=uke-gpu_defconfig
```

The full job restores `UniXNU-uke/.cache`; put the three files in
`.cache/firmware/uke-gpu/` before the run to embed them. Without them the job
still proves the A732 compile, link and header generation. The push-time
`ci.yml` runs the Linux host checks above.

On a device boot the probe still uses the existing gated arguments:

```
sheng-gpu-power=9
sheng-gpu-bringup=1
sheng-gpu-bank=<bank>
sheng-gpu-asid=<asid>
sheng-gpu-sid=<sid>
sheng-gpu-slot=<slot>
sheng-gpu-cmddb-version=<version>
```

Do not enable them in a shipping configuration before the first boot has shown
the power/clock prerequisites on; the staged modes exist so a fault stops at
the step that lacked its prerequisite.

## Checking the current state

```sh
python3 scripts/check-uke-gpu.py \
    --dtb /path/to/uke-live.dtb \
    --firmware-dir /path/to/vendor/firmware
```

It validates the board values against the DT and the pins, and never touches
MMIO.
