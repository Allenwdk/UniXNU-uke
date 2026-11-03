# uke iPadOS 27 GPU candidate (2026-11-03)

This is a complete source/build candidate, not a hardware qualification. The
interactive CPU rollback remains `2026-11-03-ipados27-ca-surface-track`.
No flashing or device I/O was performed for this GPU candidate.

## Implementation

- Upstream main: Adreno740 6fa7bae, AdrenoMetal 243d63c, GopFramebuffer e47c1b8;
  compiler recipes/patches from UniXNU c1ecc5b and board runtime from sheng ba66a66.
- Real chip ID 0x43030b00/gen7_11_0, gen71100 firmware, 1.5 MiB GMEM and
  2196 KiB preemption records. CP preemption mode 2 and per-process VM remain upstream.
  The OEM source ed4ad0d1ad00 explicitly enables preemption; the old Linux candidate
  deliberately excluded it from qualification. That exclusion is not a hardware limit.
- GMU base 0x3d68000; the GMU counter byte offset is 0x16220. GPU ACTLR 0x32b
  comes from the OEM DT and the gen7_11_0 Linux entry. Secure SID2 is not a client route.
- Sm7675Platform preserves the upstream ShengGmu/ShengGpuSmmu interfaces;
  board OPP/ACD/bandwidth policy and private firmware stay with uke.
- BO allocation, indexed handles, exclusive batch maps, per-process tables,
  two-ring FIFO/preemption/recovery and imported owner lifetime follow upstream.
- The frontend retains the upstream translator/Turnip patches, resource reuse,
  QoS, single-sample default/optional MSAA, shared caches and resumable priming.
  The 293-entry priming list comes from the current 24A446 OS/SystemOS images.
- BGRA8 uses the upstream SYSMEM policy with eligible transient attachments in
  GMEM. The display keeps upstream fence completion/retry/cancellation and uke's
  qualified requested-state correction. IFPC and high-cost capture default off.
- The original 24A446 trust modules and existing Data seed remain unchanged.
  The addon contains the plugin/Turnip and publish/priming files, with no daemon jobs.

## Build inputs and commands

Run kernel/firmware/payload from the existing `UniXNU-build` tree. The device
`feeds.toml` resolves the canonical local GPU repositories under `UniXNU/drivers`.
`Sm7675Platform` is provided by the SM7675 SoC repository. The kernel uses its
existing Apple RELEASE/LTO reference and qualified Linux IIG/toolchain.

Linux user-space requirements: Rust 1.98.0 plus rust-src; LLVM 24 clang;
ld64 956.6 with libtapi 1600.0.11.8. LLD 23 cannot link the authenticated-pointer
relocations emitted for this build. The SDK currently available locally is 17.0;
the shared-event check declares the selector already implemented by the runtime.
The resulting target is arm64e-apple-ios27.0, not an arm64/iOS17 plugin.

The local xcrun adapters are in `.cache/ios-toolchain/bin`; target-clang uses
XNU_LINUX_CLANG, target-clang++ uses XNU_LINUX_CLANGXX, and ld invokes the
TAPI-enabled linker in `~/.local/share/unixnu-startup-lto/toolchain-gpu/bin`.
The translator and Mesa caches have been reconstructed from their source pins
and complete recorded patches; old caches are retained under `*-before-ios27-main`.

```sh
./unixnu build kernel
./unixnu build firmware
# From the resolved tree, with the Linux user-space environment above:
python3 scripts/build-turnip-darwin.py --target ios27
python3 scripts/build-adreno-metal-plugin.py --target ios27 --no-replay
ADRENO_PLUGIN_DIR=build/adreno-metal-plugin27 python3 scripts/make-gpu-addon.py \
  --publish --no-daemon --prime-list build/prime-list.txt OUTPUT/gpuaddon.bin
python3 scripts/trust-append-module.py ORIGINAL_24A446_BUNDLE OUTPUT/owner-trust.bundle PLUGIN_CDHASH TURNIP_CDHASH
# Back in UniXNU-build; the existing local config points at those two inputs:
./unixnu build payload
./unixnu bootimg --ramdisk none --output OUTPUT/boot.img
python3 ../UniXNU-uke-build/scripts/pack-uke-vendor-boot.py --core . \
  --stock ../uke-images/vendor_boot.img --payload OUTPUT/payload.uefi --output OUTPUT/vendor_boot.img
```

## Payload transport

The raw FAT is 160 MiB. `payload.compression="uefi"` uses EDK II
TianoCompress --uefi and performs one exact decompression round trip.
The uke UEFI decodes into reserved 0xA8000000..0xB2000000, refusing overlap
with the live ABL source or FDT. It still accepts the old raw FAT transport.
The compressed byte extent is checked before any sector-tail rounding.
DarwinBoot, the addon wire format and the GPU runtime do not change.

The vendor_boot packer uses the current upstream v4 implementation and retains
stock DTB, ramdisk-table policy, bootconfig and vbmeta/footer. Only ramdisk
size/offsets and the relocated footer metadata change; unused partition padding
is omitted. A new compressed vendor_boot requires its paired new boot/UEFI.

## Evidence and device acceptance

Build logs are under `uke-port/fixes/2026-11-03-ios27-gpu-main/`;
release hashes, source revisions, input hashes and rollback commands are under
`uke-port/releases/2026-11-03-ipados27-gpu-main/`.
Kernel/prelink, Turnip, Metal, UEFI and payload builds passed. No extra host/QEMU
suite was run. The record checker covers the 13 areas separately.

Hardware acceptance remains: native Metal discovery, real rendered pixels and
presentation, unlock/desktop interaction, concurrent clients/preemption, resource
teardown and warm-cache reuse after a reboot. A build or a successful flash does
not establish these results. GPU throughput/frame pacing has not been measured.

The existing UKELLOG1 writer is preserved. Optional GPU cache exports use the
upstream chunk transport over NativeUfs's existing bounded export region; the
completed descriptor is recorded as `UKE GPU EXPORT` in the kernel log.

## 2026-11-03 19:09:24–19:09:27：GPU main 已按当次授权刷入

用户“设备在rec 刷吧”授权本次GPU候选。8项SHA通过；recovery b8a81f12经adb reboot bootloader进入bootloader；product uke、is-userspace no、四分区均0x6000000。
boot_a/vendor_boot_a/boot_b/vendor_boot_b均OKAY，A槽回读为a，reboot OKAY；未写userdata分区。
boot `2e46cba11922b6f2ea4018cdf5b75a564d8215fc75a8e17547bd90a5763c1328`；vendor_boot `88fbb144595130c7b5290960376c01f66c2227c917c51c283fea39e8ba0b88e5`。
本次屏幕预期CRC **EC654707C10583A5**。fastboot对带AVB footer的紧凑vendor_boot自动发送98304KB，目标容量96MiB；日志完整保留。
证据：`uke-port/releases/2026-11-03-ipados27-gpu-main/flash-result-20261103-190924.json`及匹配flash log、bootloader-transition-20261103-190904.json。
实际启动、GPU渲染/呈现、触控上划、抢占和稳定性均待验；刷写成功不作为启动成功。已验收CPU版ca-surface-track保留回退。
