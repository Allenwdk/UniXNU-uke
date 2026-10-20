# Adreno 732 (uke) port preparation

The native GPU stack in `drivers/NativeAdreno` and `kits/AdrenoFirmware` runs the
Adreno 740 on sheng. uke has an **Adreno 732** (`qcom,adreno-gpu-gen7-11-0`,
chip id `0x43030b00`), so the driver cannot be enabled until the A732 firmware
and power values are collected.

This page records the values that are already known and the exact remaining
inputs. `scripts/check-uke-gpu.py` verifies the board values against a device
DT dump and reports which firmware files are present.

## Known values

From `/soc/qcom,kgsl-3d0@3d00000`, `/soc/qcom,gmu@3d69000` and
`/soc/qcom,kgsl-iommu@3da0000` in the live uke DT:

| Item | uke / Adreno 732 |
| --- | --- |
| chip id | `0x43030b00` |
| model | `Adreno732` |
| compatible | `qcom,adreno-gpu-gen7-11-0` |
| GPU registers | `0x3d00000` + `0x40000` |
| GMU registers | `0x3d68000` + `0x37000` |
| GMU PDC | `0xb280000` + `0x10000` |
| SMMU registers | `0x3da0000` + `0x40000` |
| stream IDs | user `0x00`, secure `0x02`, GMU `0x05` |
| UBWC mode | `4` |
| GMU frequency table | `0xd1cef00:0x40`, `0x20c85580:0x80` |

The closest mainline catalog entry (newer than the pinned source) is the
`0x43030c00` sibling: family `ADRENO_7XX_GEN2`, `gen71500_sqe.fw` /
`gen71500_gmu.bin`, GMEM 1.5 MiB, GMU chip id `0x70f0000`, GMU CGC mode
`0x00020222`, preemption record 4192 KiB, and the `a740_hwcg` / `a730_protect`
tables. Those values are in `[gpu]` of `board.toml` as the starting point.

## Missing inputs

The vendor partition supplies the GPU firmware. The stock `vendor_boot`
ramdisk does not contain it, so it must be pulled from a booted Android:

```sh
adb shell 'ls -l /vendor/firmware /vendor/firmware_mnt/image' | grep -Ei 'a732|gen71500|sqe|gmu|zap'
adb pull /vendor/firmware/gen71500_sqe.fw   # or a732_sqe.fw
adb pull /vendor/firmware/gen71500_gmu.bin  # or a732_gmu.bin
adb pull /vendor/firmware/gen71500_zap.mbn  # or a732_zap.mdt
```

Also needed before the first GPU boot:

- the GPU CX/GX rail and clock votes (the A740 code uses sheng's PMGR values);
- the A732 GMU firmware header values (`gmu_chipid`, CGC mode, GMEM size);
- the SMMU context-bank assignment (the DT gives the stream IDs, not the bank);
- the ZAP shader region binding.

## Checking the current state

```sh
python3 scripts/check-uke-gpu.py \
    --dtb /path/to/uke-live.dtb \
    --firmware-dir /path/to/vendor/firmware
```

The check validates the board values against the DT and prints the firmware
hashes it finds. It never touches MMIO and never enables
`CONFIG_XNU_NATIVE_ADRENO`.

## Turning the driver on

Once the firmware is available:

1. Put the three files under the device `.cache/firmware/uke-gpu/` tree and add
   their sizes and sha256 values to a device data file.
2. Generalise `scripts/build-native-adreno-kits.py` to select the A732 pins and
   the `gen71500_*` names instead of the sheng `.cache/firmware/sheng-gpu`
   paths.
3. Add the three Adreno builders to `[kernel] archives` in `board.toml`
   (`build-arm-driver.py NativeAdreno`, `build-native-adreno-kits.py`,
   `build-native-adreno-accelerator.py`).
4. Set `CONFIG_XNU_NATIVE_ADRENO=y` in a new `uke-gpu_defconfig` and keep the
   full configuration unchanged until the GPU archive builds.
5. Boot and read the NativeAdreno log lines for GMU cold start, CP ring attach
   and the first scanout copy.
