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
  set plus `CONFIG_XNU_NATIVE_ADRENO=y`. `configs/uke-gpu-userland_defconfig`
  adds the owner/Data/union userland path for the actual device probe. The
  shipping full/userland configs stay off until a device boot qualifies the
  path.
- CI run `39685196474` (2026-10-22) built `uke-gpu_defconfig` green. The
  archive chain produced the A732 headers, the NativeAdreno service, 43 kit
  objects, the staged AGXAccelerator and the legacy display with
  `GpuPowerProbe.o`; the resulting `kernel.macho` is 52,416,512 bytes,
  sha256 `6883deac638d8e20479fc7559c692bd8e22720441d9e24f8d0582a8eadc43b67`.
  That run had no vendor firmware, so the weak empty images were linked, as
  intended.
- CI run `39718041346` built `uke-gpu-userland_defconfig` green as well. Its
  `kernel.macho` is 52,465,664 bytes, sha256
  `db984d4e4c22e3b2e14a4841f22d2b705b58145f8b3d05ce22d3eb3f8f221b6d`; the
  probe's four rows are mirrored to the kernel log (`UKE GPU1..4:`). Mode 1,
  3, 5 and 6 payloads were built against that kernel; mode 7/9 need the firmware
  cache seeded first.

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

## Device results so far

Three boots have been run on the uke tablet. The probe's four rows are the
evidence; the kernel is `be25aa09…` (run `39725958294`) unless noted.

| Mode | Result | Meaning |
| --- | --- | --- |
| 1 | `gcc=ok cc=ok sm=skip gp=skip io=0 pw00` | GCC and GPUCC read back; the SMMU and GPU windows are not readable yet because the CX rail is off. |
| 3 | `gcc=ok cc=ok sm=ok gp=skip io=0 pw11` | CX GDSC and the SMMU vote clocks are on; the SMMU answers `SM 00290406 4c017e18 40000016 v0 R00000000` (ID0 `0x00290406` is the sheng MMU-500; ID1 `0x4c017e18` gives 24 context banks, 1 S2CR, 32 pages) with no fault. |
| 4 | **passes (2026-10-23, `m4b`)** | GMU SMMU context reaches `Ready`: `GMUCTX step 1..7 running`, then `B s7 st0 k0/0 i5511 p5w13r1e1 r4w2v1` and `T00010000c900f000 S00000167 C00000001 R00000005`. The board rows are `gcc=ok cc=ok sm=ok gp=skip io=0 pw11`, `ahb=80000003>80000003 mem=80000003 dvm=82000001`, `cx=0022f001>f822f000 gx=0022f001 sv=00000001`, `SM 00290406 4c017e18 40000016 v0 R00000000`. Kernel `b0fdff6f…`, payload `e04250fc…`, `vendor_boot_uke_gpu_m4b.img` `46409c67…`, `sheng-gpu-power=4`, `sheng-smp=0`. |
| 4 | first attempt faulted inside `bzero` | With the reserved-pool `adopt` path, the device took an unaligned data abort at `_bzero+0x44` (`dc zva`): the default `IODeviceMemory` mapping of the pool is not cacheable. Fixed in NativeAdreno `59bcc5a` (write-combine map plus store zeroing). |
| 4 (before adopt) | stopped in GMU context step 1 | The context allocator could not get its memory. See below. |

Mode 3 was the first power milestone; mode 4 now completes the GMU's own SMMU
context on the reserved pool (step 7 `Ready`, status 0), which is the stage-4
gate. The next device steps are mode 5 (read-only GMU clocks and the AOP
command DB) and mode 6 (GMU clock bring-up); mode 7 needs the gen71100 firmware
in the CI cache.

### Mode 4: from the contiguous allocation to the reserved pool

The first mode-4 attempt (before the `adopt` path) asked the XNU VM for 16 MiB
contiguous and stopped at step 1 (`Allocate`) with `st=2bd`
(`kIOReturnNoMemory`) and the XNU VM printing:

```
vm_page_find_contiguous(num=4092,low=16777215): found 0 pages ... scanned 47503 pages ...
vm_page_find_contiguous(num=1027,low=16777215): found 0 pages ... scanned 47503 pages ...
```

4092 pages is the 16 MiB GMU icache (`0xffc000`); 1027 pages is the native4K
table pool. The allocator asks for the icache first and the table pool last
(`NativeAdrenoGmuMemory.cpp`), so the order the two lines appear in does not
matter: both requests are for *physically contiguous* memory, and both fail
after userland has wired ~34 000 pages. By then DRAM has no 16 MiB contiguous
run left, and XNU's scan also demands the run to start on a page aligned to the
mask's own granularity (64 GiB for a 36-bit mask, 1 TiB for a 40-bit one).

**The `contiguous` flag of `NativeAdrenoMemory::allocate()` cannot change this.**
XNU's `IOBufferMemoryDescriptor::initWithPhysicalMask()` derives an alignment
from the physical mask and, when the mask is non-zero, always takes the
`kmem_alloc_contig()` path (`iokit/Kernel/IOLib.cpp`):

```c
if (physicalMask && (alignment <= 1)) {
    alignment   = ((physicalMask ^ (-1ULL)) & (physicalMask - 1));
    highestMask = (physicalMask | alignment);
    alignment++;
    ...
}
...
kr = kmem_alloc_contig(kernel_map, &virt, size, alignMask, (ppnum_t)atop(maxPhys), ...);
```

The scattered-icache change in the NativeAdreno fork (`fbedbed`) therefore has
no effect: the mask alone forces the contiguous path. It is kept because the
GMU's SMMU translates every page anyway and a scattered icache is acceptable
to the mapping code, but it is not what unblocks mode 4.

### Mode 4 with the reserved pool: the mapping attribute

`NativeAdrenoMemory::adopt` wraps the board's `GMU Pool` with
`IODeviceMemory::withRange` and then mapped it with the default
`kIOMapAnywhere`. That range lies outside the kernel's DRAM, so the mapping is
not cacheable; the `bzero` that follows uses `DC ZVA`, and on that mapping the
instruction takes an alignment abort (`esr=0x96000061`, `far` inside the 16 MiB
icache). NativeAdreno `59bcc5a` maps the pool with
`kIOMapAnywhere|kIOMapWriteCombineCache` — the same attribute sheng's
`GpuPowerProbe` asks for the payload hand-off — and zeros it with ordinary
8-byte stores. Cache maintenance by VA is skipped: the pages are outside
`gPhysBase..gPhysBase+gPhysSize`, so `NativeAdrenoMaintainPhysicalPages`
already ignores them, and non-cacheable writes are visible to the SMMU.

## Where the memory has to come from

Upstream sheng does not allocate the GMU payloads at all. `ShengGmuMemory` has
no physical address (`package/ShengGmu.h`), and the payload handoff is:

```c
IODeviceMemory *m = IODeviceMemory::withRange(payloads[i].physical, payloads[i].bytes);
gPayloadMaps[i] = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere|kIOMapWriteCombineCache), OSNoRetain);
```

`IODeviceMemory::withRange()` takes memory that **already exists** at a
physical address, so sheng never goes through XNU's allocator for the GMU
icache. The uke `NativeAdrenoGmuContext` instead asks XNU for the memory, which
is what fails.

### uke's device tree is a reduced tree, sheng's is the real one

This is the reason the sheng route is not available verbatim on uke.

| | sheng | uke |
| --- | --- | --- |
| Device tree | the real mainline DTS (`scripts/fetch-sheng-dts.py` compiles `sm8550-xiaomi-sheng.dts`) or the on-device `vendor_boot` DTB | a hand-written reduced tree (`scripts/make-uke-adt.py`, 16 `node(...)` calls) |
| `reserved-memory` | present in full | **absent** |
| GPU / GMU / SMMU nodes | present, with `reg`, `reg-names`, `compatible` | **absent** |

DarwinBoot passes the ADT to the kernel, not the `vendor_boot` DTB
(`drivers/DarwinBoot/DarwinBootPkg/Driver/DarwinBoot.c`):

```c
CopyMem (TreePath, Request->KernelPath, Length);
StrCatS (TreePath, Length / sizeof (CHAR16) + 11, L".adt");
Status = ReadFile (Root, TreePath, SIZE_16MB, &Tree, &TreeSize);
...
Args->DeviceTree = Kernel.Base + TreeAddress - Address;
```

So the kernel sees the reduced tree, which has no `reserved-memory` and no GPU
node. That is also why the driver reads its register windows from the recorded
constants in `scripts/data/uke-gpu-inputs.json` instead of the DT: the DT has
no `kgsl_3d0` node to read. (`NativeAdrenoService.cpp`'s `providerChipId()` and
`windowNamed()` are written against a real DT and currently find nothing.)

**Both full trees are already on the device, and neither one reaches the
kernel.** The flashed `vendor_boot` carries the stock 1 885 893-byte DTB, and
the flashed `boot` carries a 475 556-byte one (`gzip(BootShim+FD) + tree`, see
`scripts/unixnu/bootimg.py`); both have the complete `reserved-memory`
subtree and the GPU nodes:

```
reserved_memory: reserved-memory { ... }
    reg-names = "kgsl_3d0_reg_memory", "rscc", "cx_dbgc", "cx_misc", "qdss_etr", "qdss_gfx", "qdss_tmc";
    compatible = "qcom,adreno-gpu-gen7-11-0", "qcom,kgsl-3d0";
    compatible = "qcom,qsmmu-v500", "qcom,adreno-smmu";
```

Those DTBs are for the **Android** boot chain: the stock ABL hands the
`vendor_boot` DTB to the Android kernel, and the `boot` DTB is the one the
Mu-Silicium bootloader appends for the Darwin chain. DarwinBoot does not read
either of them — it reads `kernel.macho.adt` from the payload volume and puts
**that** into `boot_args.DeviceTree`. So the full trees are available to be
used, but nothing currently connects them to the kernel.

### Options

| Option | What it does | Risk |
| --- | --- | --- |
| A. Add a 16 MiB `reserved-memory` node to the ADT | Declare one `no-map` region in `make-uke-adt.py` and let `NativeAdrenoGmuMemory` adopt an existing physical range instead of allocating it. Verifies that the GMU SMMU context can be built. | Needs the region's physical address to be free of the Android `reserved-memory` claims. |
| B. Publish the real DTB to the kernel instead of the reduced ADT | DarwinBoot would read a real `.dtb` sidecar (the stock `vendor_boot` DTB is already in the flashed image) instead of `.adt`, so the kernel gets `reserved-memory` and the GPU/GMU/SMMU nodes. Removes the need for the recorded window constants. | The tree is 1.8 MiB and carries Android-side memory reservations; XNU's handling of all of them is unverified. |
| C. Reserve the pool in the kernel early | Move `NativeAdrenoMemory::reservePool()` from context build time to `NativeAdrenoGpuService::start()`, before userland has fragmented memory. | Depends on DRAM still having a 16 MiB run that early; not verifiable without a device. |

Option A is the smallest step that reaches the actual milestone (a working GMU
SMMU context). Option B is the shape upstream sheng has: sheng's kernel gets
the real `vendor_boot` DTB, so its `IODeviceMemory::withRange()` can reach the
`reserved-memory` regions. On uke the same data is present in the flashed
images but the DarwinBoot -> ADT handoff drops it.

## Device test procedure

The GPU probe is compiled into the kernel; the payload only carries it and the
boot arguments. The first device session should be staged: read-only first, then
the power/clock steps, and only then the firmware/GMU/CP path.

### 0. What to keep

- Do not flash `userdata`: it holds the Unix root image. The GPU test only
  replaces `boot` (UEFI) and `vendor_boot` (payload).
- Keep the known-good `boot_uke_kpanicprint2.img` (`5be4b635…`) and the current
  userland vendor_boot as fallbacks. The uke ABL has no successful-boot marker,
  so `fastboot set_active a` before each test and check
  `fastboot getvar slot-unbootable:a`.
- The unique chip id and the trust cache are build inputs, not repository data.

### Where the payload lives: uke and sheng differ

The two ports put the Darwin payload in different partitions, and the reason is
a size limit, not a preference.

| | sheng | uke |
| --- | --- | --- |
| Payload location | the `userdata` partition, as a FAT32 volume | the `vendor_boot` vendor ramdisk |
| `boot` partition | replaced, but carries only BootShim + UEFI FD (no kernel) | replaced with the UEFI image |
| `vendor_boot` | **not** replaced; keeps the stock DTB and supplies it to the kernel | **replaced**; the payload is the vendor ramdisk |
| Flash command | `fastboot flash userdata <payload>` | `fastboot flash boot` + `fastboot flash vendor_boot` |

sheng could not put the kernel in `boot`: `scripts/build-sheng-payload.py`
records the reason,

```
Kernel-in-boot.img is physically impossible: the stock ABL kernel window is
~78 MiB before its own ~31 MiB vendor ramdisk + ~2 MiB DTB staging, while
the kernel alone is 92 MiB (73 MiB LZMA). The v4 boot image carries only
BootShim + UEFI FD.
```

so sheng's `boot` image keeps the stock DTB source and its payload goes to
`userdata` (`fastboot flash userdata build/sheng/boot/sheng-userdata-payload.img`).
Its `vendor_boot` is left alone, which is why the sheng kernel gets the stock
`vendor_boot` DTB with ABL's runtime memory fixups
(`'dtb_source': 'on-device vendor_boot (ABL runtime fixups); none appended'`).

uke goes the other way. The uke ABL delivers the `vendor_boot` vendor ramdisk
and ignores the boot image ramdisk (`scripts/patch-vendor-boot.py`), so the
payload is built into a `vendor_boot` v4 image:

```sh
fastboot flash boot out/uke/boot/boot.img
fastboot flash vendor_boot /path/to/vendor_boot_uke_full.img
```

`patch-vendor-boot.py` takes the stock `vendor_boot` header, DTB, vendor ramdisk
table and bootconfig and only replaces the ramdisk content, so the **stock DTB
is preserved** in the flashed image even though uke does not use it. The uke
ABL also rejects `fastboot boot` ("Failed to load/authenticate boot image"), so
the flash path above is the only one that works.

The consequence for the GPU work is the one described above: uke's *kernel*
sees the reduced ADT, not the `vendor_boot` DTB that the flashed image still
carries.

### 1. Seed the private firmware (needed for modes 7 and above)

The three `gen71100_*` files are not in Git. Either:

- put them in `.cache/firmware/uke-gpu/` and build on a Mac locally, or
- copy them into the repository Actions cache once:

```sh
# in the device repository; .cache/firmware/uke-gpu holds the three files
tar -czf /tmp/uke-gpu-firmware.tar.gz -C .cache/firmware/uke-gpu \
    gen71100_sqe.fw gen71100_gmu.bin gen71100_zap.mbn
base64 -w0 /tmp/uke-gpu-firmware.tar.gz > /tmp/uke-gpu-firmware.b64
gh secret set UKE_GPU_FIRMWARE_B64 < /tmp/uke-gpu-firmware.b64
gh workflow run seed-gpu-firmware.yml -f key=v1
```

`build-kernel.yml` restores `uke-gpu-firmware-*` for the two GPU defconfigs and
the kit builder verifies every pin before embedding. Without the cache the
kernel still builds, but the GMU cold start refuses.

### 2. Build the test kernel

```sh
gh workflow run build-kernel.yml -f defconfig=uke-gpu-userland_defconfig
gh run list --workflow build-kernel.yml --limit 1
gh run download <run-id> -n uke-kernel-full -D ~/uke-port/ci/uke-kernel-gpu
```

`uke-gpu-userland_defconfig` is the userland/Data/union configuration plus
NativeAdreno; it reaches the display/GPU probe in the normal boot path. The
build also produces the A732 headers and the accelerator. `uke-gpu_defconfig`
is the smaller stage-6 build used for compile/link validation.

### 3. Build the payload and vendor_boot (Linux)

Use the same payload parameters as the current working userland image, with the
GPU mode appended. Keep the unique chip id in an environment variable or a
local file, never in the repository.

```sh
cd /home/allenwdk/UniXNU
./unixnu build payload -D ../UniXNU-uke --uefi /home/allenwdk/uefi-uke \
    --kernel ~/uke-port/ci/uke-kernel-gpu/kernel.macho \
    --unique-chip-id "$UKE_UNIQUE_CHIP_ID" \
    --trust-cache out/uke/tree/build/original-static-system-trust/original-static-system.bundle \
    --data-ramdisk /home/allenwdk/uke-port/data/data-seed-214/owner-data-214.dbr \
    --kernel-arg sheng-gpu-power=1
```

Then repack against the current working vendor_boot (a payload size change
moves the DTB and the boot-image table):

```sh
python3 /home/allenwdk/UniXNU-uke/scripts/patch-vendor-boot.py \
    --base /home/allenwdk/uke-port/data/vendor_boot_uke_data7.img \
    --payload /home/allenwdk/UniXNU/out/uke/payload/payload.img \
    --output /home/allenwdk/uke-port/data/vendor_boot_uke_gpu_m1.img
unpack_bootimg --boot_img /home/allenwdk/uke-port/data/vendor_boot_uke_gpu_m1.img
```

For each later mode, rebuild only the payload/vendor_boot with
`--kernel-arg sheng-gpu-power=<n>`; the kernel is unchanged.

### 4. Flash and observe

```sh
fastboot set_active a
fastboot flash boot /home/allenwdk/uke-port/data/boot_uke_kpanicprint2.img
fastboot flash vendor_boot /home/allenwdk/uke-port/data/vendor_boot_uke_gpu_m1.img
fastboot flash boot_b /home/allenwdk/uke-port/data/boot_uke_kpanicprint2.img
fastboot flash vendor_boot_b /home/allenwdk/uke-port/data/vendor_boot_uke_gpu_m1.img
fastboot reboot
```

The probe mirrors its four GPU rows into the kernel log, so the text console
(and the pram dump) shows lines such as:

```
UKE GPU1: GPU m1 gcc=ok cc=skip sm=skip gp=skip io=0 pw00
UKE GPU2: ...
```

The screen photo and those lines are the primary evidence. Run the modes in
this order, one boot each:

| `sheng-gpu-power` | What it does | Expected on success |
| --- | --- | --- |
| 1 | read GCC/GPUCC/SMMU/GPU; no writes | `gcc=ok`, GPUCC/SMMU/GPU `skip` or `ok` |
| 2 | plus the GCC GPU branch enable | `gcc=ok` and no reset |
| 3 | plus CX GDSC and SMMU vote clocks | `cc=ok`; SMMU IDs readable |
| 4 | builds the GMU SMMU context (bank 1/ASID 1/SMR slot 2, SID 5) | context step 6 `Ready` |
| 5 | reads GMU clocks and the AOP command DB | clock rows and command-DB status |
| 6 | GMU clock bring-up (PLL/RCG/branches) | step 7, `cx_gmu` running |
| 7 | GMU cold start (needs firmware) | firmware loaded/HFI ready |
| 8 | GX power and the HFI performance vote | GX on |
| 9 | GPU SMMU context + CP ring/init + scratch | CP fence completes |

Start at 1. Move to 3 only if the CX/GX state shows the domains off; if a mode
resets, stop and report the last `UKE GPU` line and the mode number. Do not jump
to 7/9 before 6 has shown the clocks and the command DB.

### 5. What to send back

- The four `UKE GPU1..4` lines (photo or console text) for each mode.
- The mode number, whether the device reset/hung, and the last line before it.
- Kernel, payload and vendor_boot sha256, plus `fastboot getvar
  slot-unbootable:a`.
- If the pram dump is readable, the same region from `0xC4200000`; it carries
  the `UKE GPU` lines even if the screen is lost.

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
