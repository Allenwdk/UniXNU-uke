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


## 2026-11-03：GPU main 首次实机 panic 与单 user-route 候选

用户报告 `Unhandled implementation specific error`，BT：
`0xfffffff00af15438 0xdd8749300af137e4 0xfffffff00b604da0 0x2587a700b6181d8 0xb59831b00b60b17c 0x2297eab00b60c378`。
用已刷完整内核75bfbf86…解析：`write32+0x4c` 的 `str w2,[x8,x1]`，调用者为
`NativeAdrenoSmmuContext::attach+0x78 -> NativeAdrenoGpuContext::bringUp+0x2d0 -> NativeAdrenoGpuContextBringup+0x2ec`。
顶部0xfffffff00af15438是panic调用返回地址，恰好下一函数边界；不能命名为handle_user_abort执行。
确定为GPU stream attach期间MMIO写异常；缺x1/x2/x8与ESR/FAR，不能声称已确认SMR1或某个SID的具体写入。

前版把sheng的SID0/1都租给GPU；uke原始DT只有非安全user SID0、独立安全SID2及GMU SID5。
这是板级输入缺口。候选只附着user SID0，缺失第二route用UINT16_MAX表示；
保留完整上游每进程VM、mode2双ring抢占、调度、内存/Metal路径。**此修正尚未实机证明消除panic**。
此前文档“非安全0/1 fetch routes”不适用于uke，现由此节更正。

### 本次recovery回读

直接fastboot reboot recovery未提供可用shell；按既有流程临时刷原厂boot/vendor_boot四份、A槽并重启recovery，均OKAY。
ADB重启服务后识别TWRP b8a81f12；只读userdata最后32MiB，未主机写userdata。
raw SHA `9b41ad355ef3fa4cec3bf12e8a181f287d38417d3503ea6343764d1e3c1050dd`，原始/同tag解码：
`reference/logs/ukelog/ukelog-20261103-192013-gpu-main-panic.bin`，`reference/logs/boot/boot-20261103-192013-gpu-main-panic.log`。
2048槽CRC有效，但**新boot仅slot0..5/seq0..5六条**；slot6起seq2054等为旧环，不能取最高seq当本次启动。
本次约52.004s日志窗口打开、53.47s用户态boot、末行57.760404s；没有持久保存GPU attach寄存器或panic。
旧环的约5166s、HID overload和旧报告不作为本次GPU证据。审计 `uke-port/fixes/2026-11-03-ios27-gpu-smmu-user/readback.json`。

### 新候选

release `uke-port/releases/2026-11-03-ipados27-gpu-smmu-user/`；预期CRC **374A60FF5A4F1D52**，未刷。
boot复用已验证压缩loader镜像，vendor_boot 44830720 B。
boot SHA `2e46cba11922b6f2ea4018cdf5b75a564d8215fc75a8e17547bd90a5763c1328`；vendor_boot SHA `676c49b546854cc39a8d762ddba6fde652c0a0f921fe163728f2351fb8a01125`；
完整最终内核 SHA `853a19937d80a5859d0bc0601ed0dcaf08c99c48e5b55692261540fda7cbe1df`。
正式kernel/prelink/payload及一次压缩往返通过，missing2/stubbed24保持；addon/trust/Data seed不变。
当前设备为**原厂boot/vendor_boot四分区+TWRP**，留在recovery；并非在运行旧GPU版，也未刷此候选。
下一步当次授权后刷候选四分区，检查能否越过stream attach，再验GPU/桌面；若同点再异常需精确故障偏移。

## 2026-11-03 19:26:55：SID0路由修正候选已刷入

用户当次“刷吧”授权；8SHA通过，recovery b8a81f12经adb reboot bootloader，核product uke/is-userspace no及四个0x6000000分区。
boot_a/vendor_boot_a/boot_b/vendor_boot_b均OKAY，set_active a及current-slot a回读成功，rebootOKAY，结束2026-11-03T19:27:02.659507+08:00。
候选 `uke-port/releases/2026-11-03-ipados27-gpu-smmu-user/`，预期屏幕CRC **374A60FF5A4F1D52**。
boot SHA `2e46cba11922b6f2ea4018cdf5b75a564d8215fc75a8e17547bd90a5763c1328`；vendor_boot SHA `676c49b546854cc39a8d762ddba6fde652c0a0f921fe163728f2351fb8a01125`。
证据同release的`flash-result-20261103-192655.json`及匹配flash log。未主机写userdata；运行后的Data/addon/cache写入遵循原持久机制。
设备已退出TWRP并重启候选；实际越过stream attach、GPU呈现和桌面待用户验证，不能把刷写成功当panic已修。
新panic拍首行/BT，重点比较是否仍在SMMU write32/attach；无panic测试上划/触摸。

## 2026-11-03：图标缺失定位与IconServices候选

用户确认SID0版“不panic”，观察30–90帧；桌面/设置图标均不显示。按用户反馈记录本次交互和panic消失，不扩展为性能测量或完整GPU验收。

### 实机证据

- 本次环280条新记录seq0..279，其余1768槽与上次逐槽相同排除；末行609.450951s，未见新panic。
- 原始环`reference/logs/ukelog/ukelog-20261103-193902-gpu-icons.bin`，SHA `5f141a510bf6d5d2071b2a518c4948edda3189d089087dc56d7c36b6ff689246`；同tag boot解码已去旧槽。
- Data只读快照generation2、2887207已写页，传输11826360320B；seed身份588bd73f…不变。主机未写userdata。
- Data原始`/private/var/db/com.apple.xpc.launchd/disabled.plist`两标签
  `com.apple.iconservicesagent=true`、`com.apple.iconservices.iconservicesagent=true`，审计`disabled-observed.plist`。
- 最新内核实际链接的旧BootWatchdogPolicy，在sheng-lite=1且缺sheng-iconservices=1时加入这两条；
  当前999B前的参数串978B确实缺该开关。最新sheng main ba66a66明确已让图标服务保持启用。
- 同boot `93FF011F-553F-470A-AC00-37F23975F04F`、24A446的Preferences/DockFolderViewService/lsd报告
  记录“Failed to get the icon cache path”。这些是header is_simulated=1/bug_type308的os_fault，**不当作进程退出**。
- 被环目录反复列出的iconservicesagent-080240.0004.ips实际是21H461，排除为旧17线报告。

这是当前可直接修正的服务禁用缺口；GPU纹理/编译问题并未由这次日志证实。

### 修正与交付

仅在板级参数加已有`sheng-iconservices=1`，原policy每次boot用O_TRUNC重写禁用列表，
移除两标签后由原launchd启动真实服务。不手改Data、造缓存或禁用sandbox，不重编GPU。
候选`uke-port/releases/2026-11-03-ipados27-gpu-iconservices/`已打包**未刷**，参数999B（<1000）；
boot/完整最终kernel与SID0版逐字节保持，CRC仍 **374A60FF5A4F1D52**，因此CRC不能区分本次配置。
vendor_boot SHA **0eaefe1d35722ae1242ed2555a7ccd3a62fba7b65aee7c58822ea697ba35cc1c**，44830720B；
GPU addon、trust和原Data seed保持，payload正式构建和一次压缩往返通过。
当前设备是临时原厂boot/vendor_boot四分区+TWRP，停留recovery；本修正真实图标效果待用户后续授权刷入验证。

审计`uke-port/fixes/2026-11-03-ios27-gpu-icons/`含原policy、上游对照、同boot报告、原始disabled.plist及回读清单。
下一步验收桌面/设置图标；若仍缺，再查服务实际启动和Metal首个拒绝，不清空Data或重跑无关测试。

## 2026-11-03：小组件与系统应用占位符调查

用户补充：除设置以外应用显示需要手动下载且不能打开。当前24A446原OS镜像有52个完整staged_system_apps，
包括时钟/日历/天气/地图及WidgetKit扩展；当前已刷参数没有owner-staged-apps=1，发布路径处于关闭。
同boot93FF011F-553F-470A-AC00-37F23975F04F中chronod114、pkd178、WidgetRenderer_Default347实际已运行，
禁用列表没有widget/chrono/PlugInKit标签。小组件不是直接被lite禁用。
PlugInKit将Batteries、ScreenTime、GameCenter等内置appex拒绝为“不在所属应用内”；
WidgetRenderer反复返回WidgetRendererErrorDomain/1。单条iokit-open-user-client拒绝日志也不能单独证明GPU权限失败，
上游驱动有实际原检查结果采样后的服务/客户端grant，须区分记录和最终结果。
解析5个tracev3共3803221条，当前boot2495322条，旧boot1307899条分开。选定事件在
uke-port/fixes/2026-11-03-ios27-gpu-widgets/selected-widget-events.json；完整索引/只读Data在gpu-icons审计目录。

修正采用已经链接的上游owner-staged-apps=1：在Data覆盖OS的/private/var之前，
将原OS staged目录作为只读union发布到/Applications，原LaunchServices缓存刷新与bindfs_vget负责重新注册。
不下载、不复制伪造app、不清Data或小组件数据库，不改应用代码/信任链/原插件限制。
IconServices禁用条目按最新上游从lite策略移除，替代上一份只加sheng-iconservices=1的未刷候选；
其余lite列表保持。两处均是已证实的输入/策略缺口，仍不能宣称所有小组件错误会由此一起消失。
当前设备保持原厂四分区+TWRP；新组合候选构建中，未刷。

## 2026-11-03：预置应用/小组件组合候选已备未刷

release `uke-port/releases/2026-11-03-ipados27-gpu-widgets/`，CRC **7617327C1AD705CE**。
修正：已有owner-staged-apps=1发布52个原OS应用/扩展；旧lite禁用图标服务逻辑按最新上游移除。
上一份仅加sheng-iconservices=1的gpu-iconservices候选未刷，现由此版取代；不再需要该参数。
正式kernel/prelink/payload和一次压缩往返通过，missing2/stubbed24保持，参数998B。
boot保持原压缩loader，vendor_boot 44830720B，SHA `2bcddf06a514319bc8b23c0b766b17d9776c596a3d04e62bd726593ec49261fe`；
完整最终内核SHA `3a5952693fb60c8b2b059e58f45f35537d17b3195ff25b7df1f4f579f5941756`。GPU插件/驱动、trust和原Data seed保持。
当前设备留原厂四分区+TWRP；新候选**未刷，应用/小组件效果未验证**。

验收：应用不再显示下载占位、时钟/日历/天气等能直接打开；桌面和设置图标正常；再看对应小组件内容是否显示及更新。
天气等联网数据缺失与小组件完全不呈现分开判断。若仍有WidgetRendererErrorDomain/1或插件所属应用拒绝，
取新boot对照服务/应用注册和渲染；不跳过原PlugInKit限制、不清小组件数据库来假装修复。

## 2026-11-03 19:53:57：应用/小组件组合候选已刷

用户当次“刷吧”授权后，8SHA通过，TWRP b8a81f12经adb reboot bootloader，product uke/is-userspace no及四96MiB分区确认。
boot_a/vendor_boot_a/boot_b/vendor_boot_b均OKAY；A槽回读a，rebootOKAY，结束2026-11-03T19:54:04.730929+08:00。
release `uke-port/releases/2026-11-03-ipados27-gpu-widgets/`，屏幕预期CRC **7617327C1AD705CE**。
boot SHA `2e46cba11922b6f2ea4018cdf5b75a564d8215fc75a8e17547bd90a5763c1328`，vendor_boot SHA `2bcddf06a514319bc8b23c0b766b17d9776c596a3d04e62bd726593ec49261fe`。
证据：同release的`flash-result-20261103-195357.json`及匹配flash log。没有主机userdata写入；本次启动的原应用注册/禁用列表/缓存写入属于正常持久Data行为。
设备已退出recovery并重启候选；真实预置app打开、桌面/设置图标及小组件渲染待用户验证。
验收先看app是否不再需下载且能打开，再看图标和小组件；若panic拍首行/BT，若仍占位/空白保留新启动日志，不能用刷写成功称功能已修。

## 2026-11-03：组合候选失败复核——应用发布成功，数据容器类别设置失败

本次新环69条seq0..68，另1979槽与上一dump相同排除，末记录188.199309s；没有在这份新环看到内核panic。
用户见卡死，仅据此不能确认内核停机、GPU挂起或界面服务阻塞。
原始环/配对boot日志tag：20261103-195951-gpu-widgets-failure，证据在uke-port/fixes/2026-11-03-ios27-gpu-widgets-failure/。

实际/Applications已经枚举MobileTimer/MobileCal/Weather，说明owner-staged-apps发布生效；
实际disabled.plist也已无两条IconServices禁用项。前两个修正生效不等于应用注册完成或图标恢复。
iconservicesagent124/341/354多次真实退出并重拉，但未在本次Data找到同boot独立IPS；旧backboardd报告已排除。

同boot **59FF5F2A-3581-404A-AFD3-EC989C03DEC2** 的MobileInstallation日志有21条
“Failed to set data protection class … MIContainerManagerErrorDomain Code72 UNSUPPORTED”，涉及应用/WidgetKit容器。
VFS原F_SETPROTECTIONCLASS/F_GETPROTECTIONCLASS走VNOP_SETATTR/GETATTR；
已链接的上游owner-dataprotect=1兼容实现通过xattr在未加密可写卷保存类别，但当前未启用。
这是现在可直接接通的上游输入缺口，不声称它已经解释全部iconservices崩溃或卡死。

新候选按上游PE_builtin_boot_args提供owner-staged-apps=1 owner-dataprotect=1；
命令行仍可覆盖。保持原应用/信任链、Data seed、文件权限与sandbox，未启用加密或伪造硬件密钥。
不清用户Data/小组件数据库；仅让原容器服务所设类别能按已有上游实现回读。
当前设备为原厂四分区+TWRP；修正候选正式构建中，未刷。

## 2026-11-03：未加密Data类别元数据候选已备，未刷

release `uke-port/releases/2026-11-03-ipados27-gpu-data-class/`，预期CRC **72D32647A9779337**。
按上游PE_builtin_boot_args提供`owner-staged-apps=1 owner-dataprotect=1`，显式命令行仍可覆盖；
启用已经链接的VNOP_GETATTR/SETATTR xattr类别兼容实现，保留未加密卷事实，不改变文件权限、sandbox或信任链。
应用发布与IconServices保持，GPU driver/plugin、原Data seed和owner trust保持，未清用户Data/WidgetKit配置。
正式kernel/prelink/payload及一次压缩往返通过，missing2/stubbed24保持，命令行978B。
boot `2e46cba11922b6f2ea4018cdf5b75a564d8215fc75a8e17547bd90a5763c1328`，vendor_boot `a5d93b473f70685d629d031b0a6d93379e279c58a2b90a11a7fe400d32ea9557`，44830720B；
完整最终内核 `378a8c07deafad6cd49f13bf43a75b9b33ba8468e4b820a4f292635e255caf8c`。
原始新环SHA `89c60be540835638fd9729786cbd2ff05a68874f366dbbdd4af6388479aa2e08`，69新槽/1979旧槽按上次dump排除；同boot59FF5F2A…解析2167188条。
当前明确根因边界是容器类别设置Code72 UNSUPPORTED；**尚无这版图标/app/卡死修复效果证明**。
图标服务的同boot独立IPS未保存，不能拿旧backboardd或旧17线iconservices报告补充成新根因。

当前设备为原厂四分区+TWRP，留recovery；此候选未刷。验收先看原应用能否打开、图标是否出现，
再看小组件与持续交互是否卡住；若仍失败需区分容器Code72是否消失和图标服务的真实失败。
回退优先使用用户已确认“不panic”的SID0版（仍缺图标/应用），或已验收CPU版ca-surface-track。

## 2026-11-03 20:08:21：Data类别元数据候选已刷

用户当次“刷吧”授权；8SHA通过，recovery b8a81f12经adb reboot bootloader，核product uke/is-userspace no及四96MiB分区。
boot_a/vendor_boot_a/boot_b/vendor_boot_b均OKAY，A槽回读a，rebootOKAY，结束2026-11-03T20:08:28.691922+08:00。
release `uke-port/releases/2026-11-03-ipados27-gpu-data-class/`，预期CRC **72D32647A9779337**。
boot SHA `2e46cba11922b6f2ea4018cdf5b75a564d8215fc75a8e17547bd90a5763c1328`，vendor_boot SHA `a5d93b473f70685d629d031b0a6d93379e279c58a2b90a11a7fe400d32ea9557`。
证据同release的`flash-result-20261103-200821.json`及匹配flash log。没有主机userdata写入；原持久Data/app注册/类别xattr写入在正常启动期发生。
设备已退出TWRP并重启本候选；容器Code72是否消失、app打开/图标/小组件/卡死效果仍待实机。
验应用能否直接打开、图标是否出现、小组件及连续交互；失败保留新boot日志，新panic拍首行/BT。


## 2026-11-03：Data-class 实机失败，图标服务崩溃已定位，Turnip 装载修正待真机

用户反馈仍卡死、无桌面/设置图标，仅时钟三根指针；小组件只有一个相册出现。Data-class 不算功能通过。
本次只读回读 tag `20261103-201642-gpu-render-failure`，新环 SHA `44fc101768cfe9140f87a2a559253abf9734de47109289d91b0e24c034246aa4`；160 新槽、1888 未变旧槽排除，尾部 371.026379s，触控心跳仍到 361.797769s。
不能据用户“卡死”推断整个内核停止；当前环未见新 panic，界面卡死机制仍需下一轮定位。

同 boot `AAB5DAA8-B5C0-422B-8BF3-C8422E6B8082` 的统一日志解析 2,294,448 条，选取 15,671 条；
选取日志中未再找到前版的容器 Code72/Failed to set data protection class。此结果有解析范围限制，不称所有应用/容器均已验收。
同 boot 真正的 `iconservicesagent-1970-01-01-080323.0002.ips`：pid406，incident `982CEC80-48D8-44E0-BB5C-B914E34BE861`，uptime190，连续崩溃4次。
`com.apple.iconservices.cleanup` 532KiB 工作队列线程在 dyld `__chkstk_darwin_probe` 触发 `Thread stack size exceeded`/SIGBUS，FAR `0x16f22b1f0` 落入该线程4KiB guard。
栈从 IconServices → IconRendering → RenderBox → MTLCreateSystemDefaultDevice → NativeAdrenoMetal → metal2vulkan `Device::system_default_with` → dlopen Turnip → dyld `withChainStarts`；尚未进入图标 GPU 绘制。

旧签名 Turnip 使用 `LC_DYLD_INFO_ONLY`，无 `LC_DYLD_CHAINED_FIXUPS`，bind_size=430272、mappedSize=17448960。
Apple dyld 的旧 arm64e 兼容分支在栈上临时分配 `bind_size + (mappedSize/4096)*2 + 512`，算得 **439304 字节**，与真实 IPS 的 x8=439304、x12=439312 精确一致。
源码对应 [MachOAnalyzer::withChainStarts](https://github.com/apple-oss-distributions/dyld/blob/main/common/MachOAnalyzer.cpp)，本次读取副本保存在审计目录。
这是直接证实的图标服务加载崩溃原因；不能据此保证已解释所有小组件空白或系统卡死。

修正：core `a8dd110` 对 ios27 Turnip 链接显式加 `-Wl,-fixup_chains`，Meson 已有构建目录也显式更新参数。
保留完整上游 Mesa/metal2vulkan 补丁、pin、PAC、渲染与缓存策略，不添加历史 copied.rs 或功能裁剪。
Turnip 与插件正式重建、4KiB 签名；插件 CDHash 保持 `a259eef2e91b3d968de46d0c01238ab560b883db`，Turnip 新 CDHash `4c99a9d1bbd46b7d9ff05f4a611c62067fd9afaf`。
最终 addon 内的签名库包含 `LC_DYLD_CHAINED_FIXUPS`，无旧 dyld info；4210 个签名页哈希与信任包匹配，原 IPSW trust 模块逐份保持。

release `uke-port/releases/2026-11-03-ipados27-gpu-turnip-chains/` **已构建未刷**；boot SHA `2e46cba11922b6f2ea4018cdf5b75a564d8215fc75a8e17547bd90a5763c1328`，vendor_boot SHA `896955d7570b660979befe12138e0832a39da1348cb54d526302d287a0e08502`，44859392B <96MiB。
最终内核与 Data-class 一致，SHA `378a8c07deafad6cd49f13bf43a75b9b33ba8468e4b820a4f292635e255caf8c`，CRC **72D32647A9779337** 也相同，必须按 vendor_boot/addon SHA 区分版本。
保留应用发布、Data 类别元数据兼容、原 Data seed；未清数据库或写设备 userdata。160MiB FAT 与标准UEFI压缩运输完成，一次压缩往返通过。
当前设备为原厂四分区+active a+TWRP（取证用），新候选未部署。
下一轮看桌面/设置完整图标、原应用打开、Photos 以外小组件以及持续交互；同 boot 再查图标服务是否还崩溃。卡死仍在则沿新现场继续定位，不提前称全部修复。
证据目录 `uke-port/fixes/2026-11-03-ios27-gpu-render-failure/`，含只读 Data 快照、真实 IPS、解析索引、装载格式对比和 artifact-checks.json。


## 2026-11-03 20:29:32：Turnip chained-fixups 修正已刷入

用户当次“刷吧”授权，8SHA通过；TWRP b8a81f12经adb reboot bootloader，核product uke/is-userspace no及四96MiB分区。
boot_a/vendor_boot_a/boot_b/vendor_boot_b均OKAY，A槽回读a，rebootOKAY；结束 `2026-11-03T20:29:40.232511+08:00`。
release `uke-port/releases/2026-11-03-ipados27-gpu-turnip-chains/`，vendor_boot SHA `896955d7570b660979befe12138e0832a39da1348cb54d526302d287a0e08502`。
内核CRC **72D32647A9779337** 与Data-class保持相同；此次通过新的vendor_boot/addon与Turnip CDHash识别版本。
证据同release的 `flash-result-20261103-202932.json` 与匹配flash log；未主机写userdata，未清Data。
设备已退出TWRP并重启修正版；完整图标、应用打开、小组件和持续交互仍待真机反馈，刷写成功不代表功能通过。
验收观察桌面/设置完整图标、相册以外小组件、应用直接打开及约3分钟交互；若panic拍首行/BT，若仍卡死保留本boot现场再取匹配日志。


## 2026-11-03：图标已出现但仍慢；Dock 操作后的界面服务重启已取证

用户在 `gpu-turnip-chains`（vendor_boot SHA `896955d7570b660979befe12138e0832a39da1348cb54d526302d287a0e08502`、CRC72D32647A9779337）确认图标能显示，但桌面/设置都需很久；设置中上划呼出 Dock 后界面服务重启。用户明确指 respring。此版只达到“图标能出现”的局部进展，不能标为稳定或性能通过。
设备经原厂boot/vendor_boot四分区+active a进入TWRP只读取证；**当前仍为原厂四分区+TWRP，新修复未构建/未刷**。未主机写userdata，保留原持久Data与缓存。

配对原始环tag `20261103-203855-gpu-dock-restart`，SHA `24e91c1ca25aed8e760eaf5a12eab173f42b5573da2c6d7db634bf576f1a1a36`，195新CRC有效槽，1853未变旧槽排除。该次日志尾438.909757s；400.583126s backboardd[62]以SIGTERM15退出，随后backboardd[551]启动、SpringBoard[30]因backboardd respawn联动结束。此处按界面服务重启调查。

同boot **027440E6-CA24-41A9-9981-7DD558908B91** 的统一日志已解析2,330,230条（另149,338条为上一boot，按UUID排除）；4个trace共2,479,568条。当前boot的两条QuartzCore日志明确报 `MTLCommandBuffer ... error = DeviceLost`，pid62，原始解析time89970854008/89970857711。解析time是带timesync转换的字段，不能直接拿它与内核uptime400s相减。
上游 `NativeBridgeMetalFrontend.mm` 在真实runtime Metal error11后、完成回调交付后，按现有策略延迟500ms `kill(getpid(),SIGTERM)` 结束backboardd，launchd/FrontBoard重建界面。日志与该路径吻合；**导致DeviceLost的GPU故障/超时/恢复细节未持久化，不能据此归结抢占、SMMU或某个shader**。不关闭上游恢复或改成吞错误。

当前boot两份独立iconservicesagent IPS：pid114/uptime220、pid501/uptime290，均SIGSEGV、FAR0x28，CI::MainProgramArgsQ-0/-2；指令为原CoreImage `ProgramNode::create_program_and_arguments` block+1136（image+0x1a82c）的 `str d0,[x8,#0x28]`，现场x8=0。其前序将虚函数返回的程序对象存入node+0x78，随后无NULL检查写耗时字段。已越过旧dyld装载栈溢出，当前是CoreImage程序创建失败后的空指针。
同boot图标服务反复记录6类原库加载失败：ci_stdlib、ci_filters、ci_raw及三个_stitchable变体；随后服务重拉。能解释图标长时间等待与反复请求，但尚不能证明所有延迟由此产生。
原OS中这些文件确实存在；用固定版本上游AIR容器读取器直接读取原文件，ci_stdlib为CI-kernel库（0个普通函数）、ci_stdlib_stitchable233函数、ci_filters_stitchable566函数，均可解析。**这仅排除这些原文件明显缺失/基础容器格式拒绝，不证明设备NSBundle定位、文件读取或运行时库创建成功**。CoreImage原internalLibraryWithName:device:先查NSBundle资源URL，存在时调用newLibraryWithURL:error:；本轮尚无该实际URL和底层拒绝文本，不盲加资源复制或路径伪装。

设置另有同boot pid453/uptime290 IPS：FrontBoard `0x8BADF00D`，后台scene-update超过10秒；主线程在SystemConfiguration的SCDynamicStoreNotifyWait/等待IOKit quiesce，经Preferences的EthernetWatcher调用。此报告独立于GPU DeviceLost；二者因果关系未证实，不直接把所有问题归到设置或Dock。

证据：`uke-port/fixes/2026-11-03-ios27-gpu-dock-restart/` 的analysis.json、current-reports-unique.json、原IPS、device-loss-events.json、kernel-selected.txt、只读Data快照、统一日志筛选、原CoreImage反汇编/符号与library-load-analysis.log。
下一步优先获得原库装载的实际URL/拒绝文本，并把GPU已有hang/recovery/fault记录与DeviceLost配对；保留mode2双ring、每进程VM、恢复与缓存策略，不用无证据的禁用开关掩盖症状。本轮没有新修复镜像，也没有进行宿主/QEMU渲染验证。


## 2026-11-03：上游复核与GPU资源视图修正版已备，未刷

用户要求继续修复图标慢、Dock respring与设置问题，并复核sheng。已重新fetch main：sheng ba66a66、core c1ecc5b、Adreno740 6fa7bae、Metal243d63c、SM8550 a9f45dc，与本轮底座相同；不存在更新的现成全套修复。
历史CoreImage内核函数/externFunctionNames、完整运行时补丁、sheng ce7aae1抢占寄存器fenced重试已在本地，不能将其再次引入当作新修复。

发现并修正代码层缺口：owner cryptex 的bindfs以源SystemOS union的上层vnode直接VNOP_LOOKUP/READDIR，却未保留源union下层。这样访问别名时会遗漏原OS中的CoreImage资源。新 `native-bindfs-source-union.patch` 仅让owner boot、源mount确为union的bind视图继承MNT_UNION；原VFS lookup/getdirentries/getattrlistbulk的lower遍历改从源union获取下层目录。普通bind语义、只读、MAC/目录访问检查和vnode引用管理保留。不复制原资源、不改CoreImage二进制。**本轮仍未回读失败调用的实际NSBundle URL，修正与实机全部加载失败的因果关系待验证**。

设备同步sheng当前lite列表（coreduet、性能统计、Siri/duet/modelcatalog）及无相机cameracaptured禁用输入，保留IconServices/应用发布/Data类别。未引入与本问题无关的上游调度诊断开关。故障变化时将驱动已有recovery/fault/client记录写入持久日志，最多16条相关CoreImage失败路径；高开销逐帧/像素捕获仍关。
GPU源码/用户态不变，保持mode2双ring、每进程VM、CP页表切换、缓存策略和DeviceLost后正常恢复。未通过关闭抢占、共享VM或吞错误规避；QSMMUv500的ACTLR仍按OEM原DT0x32b，没有套用ARM MMU500的寄存器假设。

release `uke-port/releases/2026-11-03-ipados27-gpu-resource-view/` **已构建未刷**。
core源码 `6a91677cbdc873cc5c2a63a33022b6e2262eee3a`，device源码 `d55d703ef1205e571c91d07314dd6ca68aeebd4d`；未push。
boot SHA `2e46cba11922b6f2ea4018cdf5b75a564d8215fc75a8e17547bd90a5763c1328`；vendor_boot SHA `b2e3db8199d774ee93300e2e6833898c3ed7713b887f8323e82bafad9e41687c`、44859392B；最终内核SHA `66aceaf373c928fd3ec128f6ce9895c050444eb393b5267b9a5d8401dc251671`，CRC **ED9AFCA7B8824CBE**。
正式kernel/prelink/payload通过，missing2/stubbed24精确集合不变；160MiB FAT、一次UEFI压缩往返和96MiB容量要求通过。用户态addon/trust复用已签名chained-fixups版本；未改Data seed或清缓存，未进行额外宿主/QEMU渲染验证。

当前设备仍为取证用原厂四分区+active a+TWRP；本轮未刷任何设备分区。
**图标延迟、Dock DeviceLost/respring与设置IOKit quiesce超时均未通过新候选真机验收**，不能称全部已修。下一步按当次授权刷成对四分区，验完整图标加载、设置上划Dock、应用切换与持续交互；失败则用新增的具体恢复/拒绝记录定位，保留原始boot现场。回退CPU基线ca-surface-track的成对命令见release README。
证据 `uke-port/fixes/2026-11-03-ios27-gpu-upstream-repair/`：upstream-pins.json、上游板级源码与diff、patch before/after、正式构建日志和artifact-checks.json。


## 2026-11-03 21:21:12：资源视图修正版已刷入

用户当次“刷吧 不用做过多检查”授权；正式构建打包完成后，仅确认boot/vendor_boot两镜像SHA、目标uke/is-userspace no及四96MiB容量。
TWRP b8a81f12经adb reboot bootloader；boot_a/vendor_boot_a/boot_b/vendor_boot_b均OKAY，A槽回读a，rebootOKAY，结束 `2026-11-03T21:21:19.535812+08:00`。
release `uke-port/releases/2026-11-03-ipados27-gpu-resource-view/`，屏幕预期CRC **ED9AFCA7B8824CBE**；vendor_boot SHA `b2e3db8199d774ee93300e2e6833898c3ed7713b887f8323e82bafad9e41687c`，44859392B。
最终源码core `6a91677cbdc873cc5c2a63a33022b6e2262eee3a`，device代码d55d703；此前同路径的构建中间文件没有刷入，最终包以本次manifest/SHA为准。
证据同release的 `flash-result-20261103-212112.json` 和匹配flash log；未主机写userdata，未清Data。没有增加宿主/QEMU测试轮次。
设备已退出recovery并重启修正版；真实图标加载、设置IOKit等待、Dock respring与持续交互效果仍待用户反馈，刷写成功只代表部署通过。
验桌面/设置图标加载速度，设置中上划Dock/切换应用，以及约3分钟连续交互；respring保留新boot日志，panic拍首行与BT。


## 2026-11-03：resource-view实机回归已撤回；继续交付定向CoreImage修复

用户反馈resource-view版CRC ED9AFCA7B8824CBE卡Apple标志、无法进入系统；该release已标withdrawn/do_not_flash，不能再作为可用候选。用户明确拒绝“仅回退能开机”作为任务结果，要求继续修bug。**没有刷回旧GPU镜像，设备分区仍是取证时恢复的原厂boot/vendor_boot四份**；本轮未刷新的实验候选。

已保存新环tag `20261103-212731-gpu-resource-regression`，SHA `42cbd5b2062f9aae499517bcccf7ceefab80c78dab773eb99b6c29f20615a6bb`，88新CRC有效槽/1960未变旧槽排除；新boot `DC81CB84-E8B0-450E-A446-C4E12B9C2BA2`统一日志解析2,503,065条。launchd在83.933158s完成Early boot，backboardd实际swap1..3接受；内核触控/看门狗到226.736525s仍推进。lsd99线程602最近约94% CPU、累计102.695926s（内核91.677806s），系统应用扫描264个/CoreServices22个；**这不是内核未启动，也不能仅凭高CPU证明具体死循环位置**。新boot未找到独立崩溃IPS；旧lsd模拟报告按boot排除。Data只读快照已保存，未主机写userdata。

撤回core VFS改动 `0a22d26`、组合后台策略/板级观测 `a89c607`。core的patch/profile/scripts与原chains底座保持，新增修复不再改变全局VFS语义或后台策略；“source-union缺口就是全部图标问题”的前述因果判断未获实机证实，现明确作失败假设。

新定向实现（Metal `10a077a8b4de9b8333069089e808ab8ad2cba8bd`）：保留原CoreImage `internalLibraryWithName:device:` 结果；仅iOS27、本前端设备、原nil、8个已知CI原库名时，从标准框架路径通过原 `newLibraryWithURL:error:`重试。保持原helper直接返回newLibrary结果的+1所有权；nil/真实错误继续传递，成功不伪造。不改CoreImage二进制、不复制资源。
内核 `1858e770ef59f318d3381a6287db1a050d2055e6` 只在真实恢复时落盘已有fence/restart/fault结果，原marker NUL结束时保留有界拒绝摘要；正常调度/抢占/VM/恢复路径不变。**这为Dock DeviceLost保留原因，不称底层GPU故障已修**。设置IOKit quiesce超时也仍未证明已解决。

release `uke-port/releases/2026-11-03-ipados27-gpu-coreimage-resources/` **完整构建未刷**，CRC **90B6BC701210D9DB**；vendor_boot `cd41b67e983f2d7032d78831a1efb929931f417a1b598346101625e6ed37e2b6`、44859392B；最终内核 `47202fe27876b4af9a9a21dfa8e715af02430888f3e47ed1702a71eb5b4ddda1`。core `e528959d7288fffef05ee84f3a3c588a433cafd5`，设备pin `c624e8f72433d0cba8e64fe9eed2ecbff1f7fbe6`。插件CDHash `a461aff051a2cd13bfedfd1264b7009d5c2b101e`，Turnip `4c99a9d1bbd46b7d9ff05f4a611c62067fd9afaf`；匹配owner trust与原Data seed保持。
正式kernel/ios27插件/payload通过，missing2/stubbed24精确集合不变，最终签名输入/chained fixups、160MiB FAT与一次UEFI压缩往返/镜像容量通过；未增加多轮宿主/QEMU渲染测试。
最新连接检查adb/fastboot均未枚举；最后受控状态为active a+原厂四分区+TWRP。新修复没有刷写授权/部署结果。下一步接回设备并按当次授权刷新版，验越过Apple、完整图标加载、设置上划Dock、切换应用与持续交互；再用真实CI recovered/refused和GPU恢复原因继续修，而非以“能开机”结题。
证据 `uke-port/fixes/2026-11-03-ios27-gpu-coreimage-resources/`（构建/打包），`uke-port/fixes/2026-11-03-ios27-gpu-resource-regression/`（原始环、Data、统一日志与analysis.json）。


## 2026-11-03 22:31:33：CoreImage定向原库修复版已刷

用户通过“刷入新修复版”明确授权本次候选（并非旧版回退）。设备recovery b8a81f12已接回，Data只读FUSE卸载后adb reboot bootloader；boot/vendor_boot两SHA、product uke/is-userspace no、四96MiB容量确认通过。
boot_a/vendor_boot_a/boot_b/vendor_boot_b均OKAY，A槽回读a，rebootOKAY，结束 `2026-11-03T22:31:41.284460+08:00`。
release `uke-port/releases/2026-11-03-ipados27-gpu-coreimage-resources/`，CRC **90B6BC701210D9DB**；vendor_boot SHA `cd41b67e983f2d7032d78831a1efb929931f417a1b598346101625e6ed37e2b6`，44859392B。
证据 `flash-result-20261103-223133.json` 与匹配flash log；未清Data，未主机写userdata。失败的resource-view仍标撤回，不再部署。
设备已重启新定向修复，实际是否越过Apple标志、图标加载是否加快、设置上划Dock/应用切换是否respring仍待真机反馈；不把部署结果当bug已修证明。
验收顺序：进桌面 → 桌面/设置完整图标首次加载 → 设置上划Dock与应用切换 → 约3分钟连续交互。失败保留本boot并回读NativeAdreno client/recovery；panic拍首行/BT。GPU DeviceLost底层原因与设置IOKit等待仍需实机结果。
