# uke iPadOS 27 GPU candidate (2026-11-03)

## 2026-11-03：当前27线GPU版本通过用户真机验收

用户原话：“这版本上述问题都解决了 写文档然后推送到main吧”。本次确认对应22:31:33刷入的 `2026-11-03-ipados27-gpu-coreimage-resources`，CRC **90B6BC701210D9DB**。
已解决此前报告的Apple标志无法进入系统、桌面/设置图标缺失和加载慢、小组件加载异常，以及设置中上划Dock/应用切换导致的界面服务重启。该版本成为当前27线GPU可交互基线；17线gpucomp49和27线CPU ca-surface-track保留作历史/应急回退。

- boot SHA `2e46cba11922b6f2ea4018cdf5b75a564d8215fc75a8e17547bd90a5763c1328`。
- vendor_boot SHA `cd41b67e983f2d7032d78831a1efb929931f417a1b598346101625e6ed37e2b6`，44859392B。
- 完整最终内核SHA `47202fe27876b4af9a9a21dfa8e715af02430888f3e47ed1702a71eb5b4ddda1`。
- 核心修复：Turnip chained fixups、原应用发布与Data类别元数据、CoreImage原nil时的标准原库加载重试；正常抢占/VM/缓存/故障恢复保持。失败的全局VFS source-union实验已撤回，不在验收源码中。
- GPU代码：Adreno740 `1858e770ef59f318d3381a6287db1a050d2055e6`，Metal `10a077a8b4de9b8333069089e808ab8ad2cba8bd`，GOP `5137bbe9abf8175835898144089b5bff2166684d`，SoC `5464caa`，UEFI `c2004da`。发布时新增文档/依赖指向提交，不改变已验收代码。

验收依据是用户真机反馈；本次未重新读取修复后的日志/计数器，不把上游能力配置等同压力验证。精确帧耗时、长时间/多客户端压力、跨冷启动缓存复用仍待单独量测。
证据：release内 `user-observation-success.json`、`manifest.json`、`flash-result-20261103-223133.json` 与匹配flash log。此前文档中“待验/未刷/无法进系统”均为历史阶段，以本节为当前结论。

用户要求发布当前测试通过的代码到main。有分叉的目标远端main先原样保存到新分支，再以当前版本覆盖；能快进的直接推送，不把旧分叉源码合回已验收树。发布结果另记本次publish-result.json。


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


## main发布的源码获取

本次源码依赖固定到用户仓可获取的版本：Adreno740（远端NativeAdreno）`22abfe4`、AdrenoMetal `854f99a`、GOP `f3e29a2`、SoC `7f857a8`；与验收二进制相比这些新增提交只更新README。原编译commit及镜像证据见 [版本记录](releases/2026-11-03-gpu-coreimage-resources.json)。全局VFS实验已撤回。设备feeds不再要求外部本地路径，工作区覆盖放在忽略的local.toml；SoC自带kits/Sm7675Platform自动发现。


## 2026-11-03：已验收GPU27版本发布main完成

用户确认“这版本上述问题都解决了”，当前GPU基线CRC **90B6BC701210D9DB**。本次7仓main全部推送且远端SHA回读一致；仅发布文档/依赖引用，运行时代码与验收镜像保持。

| 仓库 | 本次发布的提交 | 方式 |
| --- | --- | --- |
| [Adreno740](https://github.com/Allenwdk/NativeAdreno/tree/main) | `22abfe46098e568fd0a66e1e358f021f2c0e964f` | 旧main保存为`backup/main-before-uke-gpu27-20261103-230417`后覆盖 |
| [AdrenoMetal](https://github.com/Allenwdk/AdrenoMetal/tree/main) | `854f99a49e88febb52d4105baa88a4db4c27718b` | 旧main保存为`backup/main-before-uke-gpu27-20261103-230417`后覆盖 |
| [GopFramebuffer](https://github.com/Allenwdk/NativeMobileDisplay/tree/main) | `f3e29a2401cf1242b4e2eaf2b89ef2dd2fcacbaa` | 旧main保存为`backup/main-before-uke-gpu27-20261103-230417`后覆盖 |
| [SM7675](https://github.com/Allenwdk/UniXNU-sm7675-common/tree/main) | `7f857a825a3c1f0882a50eaa824249c714d9679d` | 快进 |
| [UEFI](https://github.com/Allenwdk/uefi-uke/tree/main) | `8a1153e6c54c508e613127af8c639334cdd8e51f` | 快进 |
| [UniXNU](https://github.com/Allenwdk/UniXNU/tree/main) | `899fdcbf80619d8a0d68d5646c7ab131304010b4` | 快进 |
| [Uke](https://github.com/Allenwdk/UniXNU-uke/tree/main) | `c7aefb985fabc3a6d7c13f63d1f458fa71768519` | 快进 |

有分叉的目标远端旧main均原样保存到 `backup/main-before-uke-gpu27-20261103-230417`，按用户要求再覆盖main，使用精确旧SHA的force-with-lease防止并发覆盖；没有把旧分叉源码合回测试树。其余4仓直接快进，未创建额外备份。旧main完整SHA见发布审计。

core `.gitmodules`/gitlinks及设备feeds现指向远端可获取的已验收依赖，保持canonical组件ID；NativeAdreno只是Adreno740的用户远端仓名。设备本地工作区路径迁入忽略的local.toml，clean clone不依赖它。SoC的Sm7675Platform由本身kits目录发现。
13区台账和resolver状态通过；本轮不重建/不刷机，不把文档pin提交当成新二进制。修复后长时间并发、精确帧耗时和跨冷启动缓存仍待独立量测。
证据 `uke-port/fixes/2026-11-03-gpu-main-publish/publish-result.json`、`publish.log`、`publication-checks.json`，验收证据release/user-observation-success.json。
本表记录首次发布快照；设备仓随后仅补写这份发布台账，仍按普通快进推送。

## 2026-11-04 帧时间离线研究

完整证据见工作区根 `docs/status/gpu-frame-time-study-2026-11-04.md` 与 `uke-port/fixes/2026-11-04-gpu-frame-study/`。本次分析现有线程采样版CRC B4EC9E2D7D77ABCE，未构建或刷机。重负载短片段commit→complete P50 35.045ms，commit→编码submit18.592ms、GPU job15.127ms；9个flip间隔均值26.959ms，不代表全程滑动fps。UUID匹配的BO调用栈把两次约190ms的64MiB分配归到后台keep_room；编码线程37.276ms draw内另有darwin_bo_map/IOKit等待，具体锁竞争仍待测。GMEM实际已使用。

DCVS硬编码仍为sheng8档/top8，本板实际9有效OPP，触摸boost8请求900MHz而未到950MHz；最终kernel偏移0x1cc5e8找到原表，不是只看源码。没有实际频率读回，不能宣称该差异解释数倍fps差。用户说明没有sheng性能日志；下一步接上游真实touch timestamp ring/导出，按触摸窗口统计flip，继续拆map/IOKit等待和诊断开销，再单独修正OPP表验证。原GPU功能验收范围保持，性能和约180s后的卡死未验收。

## 2026-11-04 DCVS 九档与触摸/IOKit 阶段候选

已按用户要求从实际 `gStartup.gpu` 获取全部九档至950MHz；保留原governor/带宽票/15字旧统计ABI，全档驻留另存，避免第九档覆盖boost计数。上游HID event6 timestamp ring及kernel.touch.bin已恢复；可选IOKit回调记录GPU client外部调用与CPU map的原锁前、锁后、映射体返回、完成阶段及原locking标志，不改锁/返回结果。worker240秒后导出，覆盖约180秒卡住段；回调/文件实际生成尚需实机。

release `uke-port/releases/2026-11-04-gpu-dcvs-touch-map/`（工作区根），CRC77F3FB3E03ED62D7，vendor_boot SHA e4a907656a0800a845958bbbbbd5be01bbe54d6303bc2120fe5001c9dcf84eb2，44879872B；未刷未验。正式kernel/payload与压缩往返、真实OPP ASan/UBSan检查、解码旧快照/阶段关联检查通过，porting13区通过；982B参数、addon/trust/Data seed与前采样版相同。完整实现、边界与复跑见根 `docs/status/gpu-dcvs-touch-map-2026-11-04.md`。先前“硬编码仍为sheng8档”仅描述研究对象旧采样版。

2026-11-04 01:29，用户当次“刷吧”授权后CRC77F3FB3E03ED62D7已刷四分区，全OKAY，A槽回读和reboot OKAY；刷前两SHA/uke/non-userspace/四96MiB核对通过，未写userdata。release内flash-result-20261104-012902.json及匹配log为证。前段“未刷”为构建阶段历史；真实启动/触摸窗口数据/level9/阻塞结果待验，保留现场到约五分钟后再取同boot日志。

## 2026-11-04 回读：level9 与 IOKit 锁长尾实证，touch writer 遗漏已补

用户本轮反馈未卡死。新194有效槽/1854旧槽排除，尾438.337s；level9请求/接受198次、投票失败0，属于软件投票/驻留而非实际时钟读回。已确证同GPU client外部调用读锁阻塞CPU map写锁：后台room::run/keep_room线程906分配64MiB、wire111.137ms；编码线程908 map等锁110.439ms、map体45µs，读锁外部调用结束约2µs后取得写锁。BO栈UUID匹配实际dylib/dSYM。后续修复围绕外层锁契约与预分配/map交错，不直接关闭默认锁。

本轮新日志仍有3次GPU恢复返回0，第二次dead2是抢占超时转单环；当前诊断快照止于243s、恢复晚于367s，不能解释恢复现场。采样导出260.844s报-9：旧#if0漏删，touch扫描已记630条但stream少5096B/1文件。原错误blob保留，15个完整文件已恢复；本次不能给真实触摸窗口FPS，前述“touch已接通”只成立到producer/scan，不成立于完整导出。

修正移除该writer guard，真实emitter ASan/UBSan正对照与旧代码负对照通过，正式kernel/payload与一次压缩往返、missing2/stubbed24精确集合通过；982B参数/addon/trust/Data seed不变。release工作区根 `uke-port/releases/2026-11-04-gpu-touch-export/`，CRC002CD36B815DE636，vendor_boot ac1e657eafccf0263f13c89eab45a73e9828be83c93090c95938321334001ca0；未刷/未实机验证。设备取证后已恢复同77版四分区/A槽/reboot，未写userdata。完整根文档 `docs/status/gpu-dcvs-touch-readback-2026-11-04.md`；证据fixes同名readback目录。

2026-11-04 02:05：用户当次“刷吧”授权后，002CD36B815DE636触摸writer修正版四分区全OKAY、A槽回读/reboot OKAY。两镜像SHA、目标uke/non-userspace与四96MiB容量通过，无活动Data FUSE、未写userdata。release的flash-result-20261104-020507.json及匹配log为证；当前已重启002C，真实启动/导出闭合/kernel.touch.bin待验。IOKit锁改造尚未实现，前段“未刷”为历史。

## 2026-11-04 真实 touch 导出与帧间隔实测

002C回读status0、70235329B长度/count闭合、16文件含2886条TCH1时间戳，writer实机验收完成。34个真实touch run首末报告跨度共23.777767s，内含935次驱动flip，约39.32次/s；连续更新的894对间隔P50/P90/P99=19.134/40.898/94.771ms，6段≥100ms。保留零更新run、触摸前idle与边界列表；不能只用快帧段42.90次/s代表全部交互。

用户确认桌面滑动和打开/切换应用均短暂停住。233.659974s第一条touch到238.746831s下一flip约5.087s，两次触摸85条无flip；backboardd临近末端才commit，其他GPU client在工作，需独立追提交/唤醒链。IOKit等锁仍达114.289ms，持锁外部调用中的64MiB wire114.621ms；编码线程map体42µs。361.579537s有一次dead2抢占超时恢复，晚于约260s快照。没有声称这些问题已修。

用户明确“取完日志不用恢复，在twrp等着刷新的版本就行”；当前原厂boot/vendor_boot四分区+A槽+TWRP，未恢复002C，未主机写userdata，本轮未构建新锁修复镜像。根 `docs/status/gpu-touch-window-readback-2026-11-04.md` 为最新接续；证据 `uke-port/fixes/2026-11-04-gpu-touch-export-readback/`。

## 2026-11-04：120Hz第一阶段锁范围修复候选

已实现可选native-XNU入口，把自有NativeAdreno非连续BoAlloc尚未发布的页面prepareSystem移到原IOKit映射排斥锁之前。原锁内复核live/限额/请求后采用，活动IPC计数覆盖全调用；close抢先完成则拒绝并回收。BO诊断栈读取亦移到锁外。map/unmap/close、原默认锁、ABI与实际错误保留，不关闭全局锁。旧内核无此适配时保留原路径，BO trace bit16标识preflight。

必要主机检查抽取实际XNU包装器与驱动prepare/finish，受控VM暂停时map/close能继续、采用/回收一次、关闭先行拒绝和IPC归零通过ASan/UBSan；旧包装器负对照失败。正式kernel/payload及一次压缩往返、missing2/stubbed24精确集合通过；982B参数/addon/trust/Data seed与002C相同。候选CRC ECB8BC9681CCBA53，vendor_boot ef5cc5bd89b35148214304bc1e317e16c7de13da3c88409db03c4dc8b9a2a77b/44883968B，已备未刷/未实机测收益。设备继续原厂四分区+A槽+TWRP；本轮无设备写入。

真实120Hz以整帧8.33ms预算验证，尚未达成；5秒无新提交与晚期抢占恢复未修。下一步用真实touch同场景对照map锁长尾，再查其他慢external方法/GPU多pass成本。完整根文档 `docs/status/gpu-prepare-outside-iokit-2026-11-04.md`，证据与源码SHA/原始备份在fixes同tag目录。

2026-11-04 02:41用户当次“刷吧”授权后，ECB8BC9681CCBA53候选四分区均OKAY、A槽回读/reboot OKAY；recovery经adb reboot bootloader，两镜像SHA/目标uke/non-userspace/四96MiB检查通过，无活动Data FUSE、未写userdata。release内flash-result-20261104-024155.json及匹配log为证。当前已重启新候选，实际启动/锁长尾/帧率待验；下次取完日志仍留TWRP，不自动恢复旧镜像。前段未刷为构建历史。


## 2026-11-04 ECB8 启动回归取证

用户报告很早卡开机，授权进rec。原厂四分区/A槽/recovery成功，只读日志127新CRC有效槽/1921旧槽排除，原环3c3938935b2415f8f572e08ab83cb74fe759361d3b087b07ae272d40cf744f56。81.374345s SystemOS卷F914C62A-AD00-47C6-8281-5746291C339A not ready，81.395446s root publisher停window1/published1；rootvnode未设置，相关线程至260.780783s仍导出status0。小blob33470B仅rings/skipped两文件，无用户态性能数据。002C对照52.110356s同卷ready后继续OS/launchd。直接致因未定，不将GPU代码变化与挂载失败强行等同。ECB8已withdrawn/do_not_flash；当前原厂四分区+A槽+TWRP，未恢复镜像/未主机写userdata。完整根docs/status/gpu-prepare-early-stall-2026-11-04.md和fixes同tag为接续；有新日志，暂不需要照片。


## 2026-11-04 03:01：保留BO优化，修正启用时机并刷入DACD

用户要求继续修复，未回滚锁外BO准备。上一版只凭弱符号存在即对早期IOUC进入新IPC pin/类型检查，作用范围过早过宽；新增ready初始0，GPU上下文/锁/服务发布后release置1，XNU和driver双重检查。root开始打印ready，失败时worker最多一次打印缓存存储状态/ready/entries。此范围缺陷已修，但ECB8日志无entries，不能宣称就是SystemOS超时全部因果。

002C与ECB8签名根策略相同、ADT仅random-seed不同，设备SystemOS前64KiB与原镜像相同（非全卷完整性证明）。实际包装器ready0无新入口/ready1保持map-close并发及资源回收的ASan/UBSan通过，ECB8未门控负对照早期none_entries断言失败。正式kernel/payload和一次UEFI往返、原missing2/stubbed24集合、982B参数/addon/trust/Data seed一致通过。

release2026-11-04-gpu-preflight-readiness，CRC DACD0576367904C3；vendor_boot41c70ca2324392df6730376a4f5cc84875b01fef6750522d580088e9f9dca635/44883968B，最终kernelc3dd5fa3e700633b59b91f2d9f3c1993902c84806f68b5e87c5c8055c1b318c9。03:01用户当次刷吧授权后四分区/A槽回读/reboot全OKAY，未写userdata，证据flash-result-20261104-030147.json及匹配log。当前重启DACD，实际SystemOS恢复/桌面/帧率待验；取完仍留TWRP、不自动恢复旧版。根docs/status/gpu-preflight-readiness-2026-11-04.md为接续。


## 2026-11-04 DACD回读与WaitFence第二阶段（03:20已刷）

用户确认DACD能进系统、感觉顺些但仍卡。165新CRC有效槽/1883旧槽排除，原环56d3ca95aa9740ce5af0a3289f2d7e7709831cff3eb13c69b06db77490d375a6；export0/72528708B/16文件闭合，blob5cc99811863bfa788a6a49f87568b582597fb45be8a82c140507783d9200b90a，2687条touch，latency357772无dropped/overwritten。root ready0、SystemOS52.089s ready、GPU64.162s启用；68条BO全部flags27，优化实际采用。真实touch run内更新46.54次/s，连续996间隔P50/P90/P99=12.916/29.477/83.163ms；保留零flip段，仍有6.09s首次touch至下flip，不能只用连续更新57.67次/s代表整体。与002C负载不同，不作受控提速归因。

编码map等锁P90/P99/max2.750/17.222/61.718ms。197.795548s实际持锁线程816栈与Turnip UUID01702158-8D70-3832-9E7E-D707F9508C44匹配，dw_wait_fence_raw→darwin_sync_wait→WaitSemaphores；同client外部调用结束后约2us map获锁。下一持锁源确认为fence等待，不只凭tunnel selector0猜。

保留BO/ready优化，扩展私有context：非零WaitFence锁前复用原校验/timeout/等待/retirement/recovery，原锁内复核live/请求并返回真实结果。成功关闭release发布、等待acquire读取；core访问仍在g.lock内，IPC pin覆盖全程，零poll原路径不变。rings新增wait-preflight真实统计。实际preflight/锁内消费者ASan/UBSan覆盖map/close并发、真实成功/timeout/error、关闭先行NotReady/IPC归零/BO回收；旧DACD负对照map被wait挡住断言失败。正式kernel/payload/原missing2-stubbed24集合/982B参数/addon-trust-Data seed与一次UEFI往返通过。

新版release2026-11-04-gpu-wait-outside-iokit CRC0A3FC5BED491D8D0，vendor_boota6dc1efc4cb350f2f48898ccab8291d3ddd1bb83f24ce94cc8d9bb082f12979b/44883968B，final381ee4ae912207255663e1a87ff8e7d3f6e241d970051eeb0392a850fc82ff48。03:20用户当次刷吧授权后四分区/A槽回读/reboot全OKAY，未写userdata；证据flash-result-20261104-032005.json与匹配log。实际wait版启动/收益待验。DACD晚期3次恢复323/330/357s（第二dead2）在快照之后，6s无提交链未解决；不称120Hz达成。根docs/status/gpu-preflight-readback-and-wait-2026-11-04.md为最新；后续取完仍留TWRP。

## 2026-11-04：0A3F无响应回读与context-init候选

用户四分多钟无响应/无panic，等一分钟再fastboot。新环101/1947旧排除，raw b2a344b4892eab4727d0c3f545d4f9d9a8f05ff1af3305b0a72f56acc7c654b1；新export80982537B/16文件结构闭合（ac31b580daec55158d163e8368b3d339eaba3cb3240738074c28ad01642c2f8d），没有status0完成行。wait-preflight4392次non-success0、6765map等锁P99=2µs；touch run46.89/s，GPU5093于254.318152s退休，touch至257.836275s，scanout至260s，最终卡死机制未证实。当前boot94FE82A6-8D7B-4A44-ACA8-68E57150B983；Settings362仍被IOKit quiesce的EthernetWatcher等待导致FrontBoard watchdog终止，不等同最终全屏无响应。

已证实IONew未执行NativeAdrenoPreparedAlloc默认成员初始化，method实际0，58大BO全部flags11、锁内重复准备页。两处显式赋值初始化，真实零分配ASan/UBSan通过、已刷旧实现负对照失败；不撤BO/WaitFence优化。新release2026-11-04-gpu-preflight-context-init CRC66774380BB075567，vendor_boot b1f37768d4e8aed5af376006e2571fd1556d0d92c2b547da42c5990f92288515，正式构建/982B参数/原missing2-stubbed24/addon-trust-Data seed/一次UEFI往返通过，未刷。设备stock-four/A/TWRP，host未写userdata/FUSE已卸载。根docs/status/gpu-wait-freeze-and-context-init-2026-11-04.md及两fixes目录为接续。需验flags27恢复和六分钟交互，卡死/120Hz未验收。

03:49:07当次授权部署更新：CRC66774380BB075567四分区/A槽回读/reboot全OKAY，2026-11-04T03:49:07.810017+08:00–2026-11-04T03:49:15.374002+08:00，两SHA/uke/nonuserspace/四96MiB核对通过，无FUSE/host userdata写。证据release/flash-result-20261104-034907.json与flash-20261104-034907.log。当前已重启新候选，真实启动/flags27/卡死与120Hz未验；后续取完仍留TWRP。

## 2026-11-04：6677实机与下一定点取证候选

6677本轮用户稍顺/仍卡/未遇卡死；原始日志到462.530s，实际build-id1c16edcf89f4f3b2与已刷源码重建SHA配对，206新槽/1842旧排除。61大BO全flags27，编码3528次map等锁P99=1µs/max0.298ms；原锁问题实机收敛。真实touch run44.19次/s，仍有GPU三次恢复/3.175s及1.974s触摸后无画面、设置436 IOKit-quiesce watchdog，120Hz未达成。

raw90a73c16631585fbd3da3cca1e574e8f88a2842f6317222277e5456029db73aa；导出status0/69570318B/16文件闭合，blob ea4ebbeccbe86d640e3582617b4bd7e4b30749917005472d2e3012b4dbcab0a5。Data完整只读12045987840B覆盖页，新boot5B0634AD…5份报告，统一日志本boot2494239条；六旧导出IPS排除。正常有效未重放GPU jobP50=8.847ms，整帧8.33ms预算仍未满足。三恢复3242/3249/3254，第三dead2；头两次与128+12/128+9拆分相关但因果未证实。另有248s无提交/服务繁忙与Settings436 watchdog主线程EthernetWatcher→IOKit quiesce。

本轮未改设备分区、未恢复旧镜像、未主机写userdata，当前recovery/A槽，FUSE卸载。上游fetch main6fa7bae，Submit/Preempt无差异。定点取证候选CRC360388DFEE6BAC40，仅诊断开启时前八次hangcheck已判挂起、reset/endFault前记录22CP寄存器+已有preempt/stall/IB/switch源；旧数据标prior，诊断读失败不改ring健康，原stall错误契约保留。正常帧/提交/超时/恢复不变。实际捕获ASanUBSan和正式kernel/payload/exactmissing2stubbed24/982Bargs/addon-trust-Data seed/一次UEFI往返通过；vendor_bootdb2b49501c8ccc6a300fdc4b93647f4a4f30bd62ea909a7490d826ad06b65856，final9772920bfc7b80c0906ce68d4bcffa90c659add6ee5204e1078ac9669a30f7a4。已构建未刷，不称新的性能修复。接续根 `docs/status/gpu-context-readback-and-recovery-2026-11-04.md` 与context-readback/recovery-capture两fixes目录。

## 2026-11-04 普通帧120Hz预算主线（纯离线）

按用户新优先级，主线转为秒级大卡顿以外的普通帧稳120Hz。排除三恢复加1s保护及245–249.794s秒级无flip后，811普通连续间隔P50/P90/P99=14.037/32.496/79.297ms，仅53个<=8.333ms。869eligible/866唯一关联command中64.7%的GPU job超8.333ms，CPU commit→GPU提交P50/P90=5.589/12.466ms。

三段普通时段：200.341–203.049s单Metal-pass交互CPU/GPU中位5.330/8.890ms；222.348–224.924s多pass12.295/25.550ms；259.084–259.515s轻GPU5.037/2.939ms但latch平均15.700ms。实例command2371/fence3572 GPU总25.567ms，三个全屏pass占17.137ms。所有关联普通command软件level9/950MHz，实际clock未核验。CPU/GPU分位数不能相加。

GPU整体P50/P90=9.175/25.227ms，18.2%CPU阶段超8.33ms。实例Metal16→Vulkan19pass，GPU25.567ms/passsum22.606ms；普通passsum无超过job。Mesa整数52ns坐标按625/624修正后17163/19641pass匹配；CPU/GPU偏移只作50us记录容差的近似。原cache UUID+exports核验的CPU热点为psynch_cvbroad/memmove/memset/posix_madvise；不能独占归因到某处广播或给固定CPU百分比。Encode样本priority54/runq低，不支持主要是优先级饥饿。配置为FD735 chip0x43030b00/3SP3CCU/GMEM1.5MiB，740为6SP6CCU/3MiB，无sheng同场景倍率结论。

下一主线为CPU按等待prefix合并batch完成唤醒与最小观测/实际clock验证，再按最重pass追GPU纹理访问、load/store、转换与GMEM成本，轻GPU段关联VSync deadline/新command/latch。保留多等待者进展及scheduled/completed语义。秒级恢复与Settings等待留支线，CRC360388DFEE6BAC40仍未刷。 本轮只做既有6677数据的离线分析：未改runtime、未构建新镜像、未操作设备；原SystemOS临时只读挂载已卸载。普通帧120Hz未达成。 接续根 `docs/status/gpu-normal-120hz-budget-2026-11-04.md` 与normal-120hz-study审计。

## 2026-11-04：普通帧性能第一版实际实现

实际完成Metal等待前缀满足才广播、工作队列空→非空才通知；按原厂DT/OEM寄存器和3/4/1核分组接入sheng上游mode4 CPU动态频控/触摸boost/帧下限；启用既有固定120Hz刷新范围。关闭可选周期CrashReporter扫描，保留touch/GPU/frames与持久日志、既有BO/WaitFence/ready优化。

新候选CRC **FBDD4ADE7898D2C4**，release `uke-port/releases/2026-11-04-gpu-normal-perf/`；vendor_boot722db44c58422d3e144ff330d3f1ec641d83b1a6c9707f03759a56bb5cd2bc90。实际源码ASanUBSan/旧广播负对照、正式kernel/plugin/payload、missing2-stubbed24精确集合、最终签名页/trust/chained、原Data seed和一次UEFI往返通过；参数999B，Rust/Turnip逐字节保持。**已构建未刷，实机收益与稳定120Hz待验，本轮未操作设备。** CPU原代码来自sheng965460a，L3缺板级DT契约而未绑定；日志区分平均请求与当前实际state。debug334等于0x14e，仅为参数长度。保持原scheduled/completed/资源生命周期，无提前完成。详见根 `docs/status/gpu-normal-perf-implementation-2026-11-04.md`。

## 2026-11-04 11:56:07 FBDD当次授权刷写

用户当次“刷吧”授权后，CRC **FBDD4ADE7898D2C4** 四分区boot_a/vendor_boot_a/boot_b/vendor_boot_b全OKAY，set_active a/current-slot a/reboot OKAY。两镜像SHA、b8a81f12/uke/is-userspace no与四96MiB容量核对通过；从TWRP用adb reboot bootloader，无活动Data FUSE，未主机写userdata。 刷写区间 `2026-11-04T11:56:07.170581+08:00`–`2026-11-04T11:56:14.556234+08:00`，证据release内 `flash-result-20261104-115607.json` 与 `flash-20261104-115607.log`；vendor_boot SHA722db44c58422d3e144ff330d3f1ec641d83b1a6c9707f03759a56bb5cd2bc90。 设备已重启性能候选；真实启动、频控实际行为、普通帧收益和稳定120Hz待验。先确认CRC/桌面，重点测桌面/资源库连续滑动、设置列表和应用切换约六分钟，回读CPU请求/实际频率、phase4通知、普通CPU/GPU/flip/latch分位数。后续取完仍留TWRP，不自动恢复旧镜像。


CRC 024B5DB9A5698920 adds `sheng-diag=2`, using unstamped Turnip render passes and disabling the client sampler, detailed IOKit tracing and BO stack capture. Touch, flips/latches, whole-job GPU timing and pre-reset recovery evidence are retained. Flashed to both slots on 2026-11-04, with slot A selected and reboot accepted. Boot and performance await on-device verification. FBDD readback yielded 17 of 182 ordinary touch-window flip intervals within 8.333 ms.

024B5DB9A5698920 exported 11 files with 2,192 touch reports and no per-pass trace. Candidate 48B5BA69E9022695 skips repeated scanout preparation while a validated direct request waits for its GPU fence. The original checks run before flip. Flashed to both slots on 2026-11-04, with slot A selected and reboot accepted. Boot, presentation and performance await hardware validation.

AAA46F2B3BF99582 ran through a 434.497 s capture without an IRQ9 panic record. Candidate 5164F278FF7857C9 forwards the existing fence timestamp interrupt to the scanout work loop, keeping a timer fallback and the real fence and latch checks. Flashed to four partitions on 2026-11-04 with slot A selected and reboot accepted. Boot, IRQ wake behavior, and frame-time gains await hardware validation.

5164 hardware logged 3,494 fence IRQ notifications and zero effective IRQ retries. Candidate 12AE90686D7D2409 queues a coalesced kernel callout through the display gate. The fallback is 200 microseconds until a real IRQ retry runs, then 500 microseconds. Built and pending hardware validation.

Candidate A241A80DD44ADE63 enables touch-window GPU execution counters via sheng-diag=2 and sheng-gpu-trace=2, retaining the 12AE display callout. The decoder validates reset, preemption and vote epochs and reports global BR windows with contained jobs. Flashed to four partitions on 2026-11-04 with slot A selected and reboot accepted. Boot and counter capture await device validation.

D50B boot readback has 69 fresh slots and no completed GPU export. SystemOS UUID was not ready at 80.808s; GPU preflight remained disabled. Candidate 028EE52EBBBF73B0 keeps the VM pool and GPU/display sources, restores original delayed-work type/startup metadata, and records pool/thread state on root failure. Boot validation pending.

028E hardware reached SystemOS/OS readiness and launchd with 1274 valid execution-counter windows and 3153 IRQ retries. Plugin comparison package 2026-11-04-gpu-transient-sysmem-ab reuses kernel CRC028EE52EBBBF73B0, Metal CDHashe98d8accd3352a7613c193e9195b6fdbcfbe3c90, vendor_bootcbb4132a9587261bca755b402789454a2f041820dfcd135f359815a8c33d355d. It requests GMEM/SYSMEM in alternating commands for a narrow attachment class. Unflashed, pixel/performance/memory validation pending.

The e98d8acc plugin package boot stopped before GPU initialization. Snapshot shows manual postModLoad waiting for OSKext while an OSKext loader waits for preModLoad. Candidate A2EB4CE2CE4086F6 orders all board registrations with the existing OSKext lock. Original GPU comparison plugin is preserved. Boot qualification pending.

## 2026-11-05: guarded LLCC candidate

The 4648 ordinary-frame capture has GPU job P50/P90 11.518/15.308ms;
GPU frequency is about950MHz in busy counter windows. The board now supplies
its OEM cliffs7 GPU9/3200KiB and GPUHTW11/256KiB cache policy to the shared
`Sm7675Llcc.h` operations at GPU hw_init/reset. The CP is stopped there.
An already-configured matching cache is reused. Only inactive blank slices may
be initialized, and other clients' policy bits stay unchanged. Unknown/mixed
policy, readback or activation failure leaves the existing GPU route.
The default mode is2 in `UkeGpuSystemCache.h`/board.toml, 1 observes and0 skips
when explicitly overridden by sheng-gpu-llcc; the release keeps the original
999-byte boot arguments. Log lines report pre/post state and actual route.

Host MMIO-peer checks cover blank/existing/reset/read-only, every183 individual
I/O failure, timeout, route restoration and1000 unrelated policy masks under
ASan/UBSan. This is a performance candidate, not a hardware/120Hz claim.
Release and measured acceptance belong to root docs/status/current.md and
`docs/status/gpu-llcc-2026-11-05.md`. Deployment requires current authorization.

## 2026-11-05 LLCC readback and shader-wait candidate

CRC0394BE262DDF45AF has hardware LLCC result0/step5,banks4,init3/activate3,
GPU9/HTW11 active0->f and GBIF0/1 zero->2500/1294a529. Three restart readbacks
keep the configuration.197new slots/1851old,export605b46c1/8981091B/11files.
959 ordinary command-job matches: GPU10.647/16.316ms P50/90;singlepass10.503/
11.720ms,encode4.348/6.788ms.12 matched metadata groups10 lower,group median
change -1.812percent is directional only,not identical shader/content causal gain.
120Hz remains unmet;8recoveries observed in444s,with ordinary masks excluding
them. Coherency stress and pixel correctness are not independently qualified.

The next candidate preserves this cache path and userspace and extends the
existing32ms capture to31 BR counters,with ten SP wait/ALU/local-memory values
in event69. Legacy21-counter decode is retained. This is a low-overhead
diagnostic candidate for GPU-internal classification,not an FPS fix.
Root status documents carry artifact identities and the deployment boundary.

## 2026-11-05: shader counter pipeline correction

The1D70 trial retained163fresh slots/1885old and a complete11-file export.
1678 extra counter records are allzero despite valid selectors and activeBR.
Primary upstream a7xx_perfcntrs.json assigns SP24..35/PERFCTR2_SP toBV,notBR.
The earlier expansion selected the wrong pipeline; it cannot classify fragment
shader waits. Original21 counters and job timing remain valid;singlepass GPU
P50/P90=10.432/11.940ms.

The correction uses BR SP7..16/PERFCTR_SP and an explicit0x4252001f scope
marker. Decoder namespaces old31 records as bv_legacy, rejects BV/BR mixed
windows and marks flat extra counters invalid during shader activity. Host
tests now verify actual group/pipe membership as well as XML addresses.
Rendering/LLCC/frequency and userspace are preserved. New hardware results
remain pending; this is a diagnostic correction,not an FPS improvement.

## 2026-11-05 current GPU source checkpoint

The local checkpoint corresponds to deployed diagnostic CRC79EC212110E36D2A.
LLCC activation/routing has hardware readback. BR counter activity and120Hz
remain pending the next capture. Component source commits are recorded below.

- UniXNU/drivers/Adreno740 `e29c04e6449d701aad352c52f02a21da05ca8803`
- UniXNU/drivers/AdrenoMetal `d3355fc28999cea1ad2a49321857e91099f3256d`
- UniXNU/drivers/GopFramebuffer `b688eb27996f6bd5f4b075255046b25b0cec0fbd`
- UniXNU-sm7675-common `b21db76e3251d3910bd6c4902751c8c9caf7079e`
- UniXNU-build/drivers/ArmCpu `bc72a4f41d7822873cc48ffdf36a485d675fbce3`


## 2026-11-05 普通帧 GMEM 非 fetch ROAA 候选

待授权候选 CRC **C352017CCC10D54B**，release `uke-port/releases/2026-11-05-gpu-gmem-nofetch-roaa/`。
默认仅 A732 的 GMEM 非 input-attachment 读取 shader 省去颜色顺序 flush。真实 fetch、深度/模板和显式调试强制顺序保留。
AAA4 GPU P50 9.045ms，AA4A 单 pass 1266条 P50 10.330ms，927条超8.333ms，1264条带GMEM策略请求。
可消除flush占比尚未量到，像素、实际收益及120Hz待真机。正式kernel/Turnip/plugin/payload及定点ASan/UBSan检查通过，
vendor_boot `10861c74a87bc34edabc2f46d2293598d67499a8026b6b1b8e1d0ed1c396235a` /44896256B。
68,061个native符号与段节布局匹配v2，999B参数及Metal可执行相同，addon仅Turnip和manifest变化，原厂trust/Data种子门禁通过。
本轮沿用v2 addon配置，per-pass激活缺口继续待查，设备访问次数为零。工作区详细记录 `docs/status/gpu-gmem-nofetch-roaa-2026-11-05.md`。

## 2026-11-05 12:47 已授权刷入 C352017CCC10D54B

用户当次“刷吧”授权。2026-11-05T12:47:11.977026+08:00 至 2026-11-05T12:47:19.479486+08:00，SHA256SUMS 10项全部通过，
设备 b8a81f12 从 TWRP 经 `adb reboot bootloader` 进入 bootloader fastboot，product uke、is-userspace no、
四个分区容量均为 0x6000000。boot_a/vendor_boot_a/boot_b/vendor_boot_b 的 Sending/Writing 全部 OKAY，
set_active a、current-slot a 回读及 reboot 均 OKAY。主机未写 userdata。
候选已部署并重启，真实启动、画面正确性、普通GPU收益和120Hz待实机。
证据 `uke-port/releases/2026-11-05-gpu-gmem-nofetch-roaa/flash-result-20261105-124711.json` 与 `flash-20261105-124711.log`。

## 2026-11-05 C352图标/渲染回归已取证，修正版 2DFC638817B9A440 已备

C352已撤回并标DO_NOT_FLASH，活动ROAA优化已移除。100新槽/1,948旧槽，raw1943dfd4，文本到248.363s。
export逐字节属于旧v2，已排除。本轮Data帧记录2,877个command；没有新GPU ring，无法评价提速。
Data实际有ngpu/per-pass和tu_debug，先前“未安装”推断已更正。安装后的两个Mach-O前缀与C352一致，
但分别残留3,280,912/574,000B旧尾部，已补显式va_data_size设置及exact-size回归检查。
新模式4补通pass producer/reader/transport，180秒worker导出并打印addon安装计数。
修正版 **2DFC638817B9A440** 正式构建/包内门禁通过，vendor_boot `25e049f61ca5cf96ee6e7cf2191bf6766532f5734fa94a7ee63d467d96942887` /44,896,256B，待授权刷入。
设备取完留原厂四分区+A槽+TWRP，主机未写userdata。图标/像素恢复、采样链实机有效性和120Hz待验。
详见 [回读与修正](../../docs/status/gpu-roaa-repair-2026-11-05.md)。

## 2026-11-05 上游main同步完成，候选 6063137DD24BD4FA 已备

Core main18155b4和Metal maine82788d已合入当前构建线，Core `066efad2f2ed938f092529047ab3a88d750b53db`、Metal `29a85d88f40b8f4928b933b92fd5456c692c2767`、Adreno740 `176e0168e548846ae1fdb0f41e3eab9f2bcdaef3`。
Adreno740/GOP/ArmCpu/Smmu4K/XnuPrelink已含其上游main；新增sheng CP抢占线程回调与稳定等待栈保护。
模式4接入图标服务Metal记录，原ROAA、A732/LLCC、IOKit与addon精确长度修正保留。
正式kernel/插件/payload、定点检查与15项主线测试通过，vendor_boot `6f4a7f3782b8a7b8b6777b540957ad77b7ef767e52f62c04abdd4e35fa13283a` /44,900,352B。
候选 **6063137DD24BD4FA** 未刷，设备沿用原厂四分区+A槽+TWRP，本轮无设备操作，图标和120Hz待真机。
见 [同步、范围与证据](../../docs/status/upstream-main-sync-2026-11-05.md)。


## 2026-11-05 图标服务配置补齐，候选209862415C7884EC未刷

此前606只有图标Metal诊断，现已适配sheng 5d51e87的图标服务环境配置（FORCE_SOFTWARE_RENDERER=0、CI_ENABLE_METAL_GPU=0），并接入签名缓存与只读加载路径。Core源码0ee4f95、设备源码5ca9189；715服务仅目标环境变化，原装载/签名检查保留。正式kernel/payload及主机缓存/签名检查通过，vendor_boot SHA256 9db44e32a104cc6b225e06f97914307bc74b958e7cd567f2d9b4d02c634fdbef，45,301,760B。新候选未刷、未推送；私有libxpc消费、实际启动、图标恢复及120Hz待真机。本轮无设备操作，设备状态沿用原厂四分区+A槽+TWRP。


仅主机证据；签名逐页与缓存语义检查已过，Apple私有libxpc和真机接受未测。报告位于工作区docs/status/icon-service-config-2026-11-05.md。


## 2026-11-05 Sheng useful-change review (host only)

sheng main4601795筛选完成：缺少可隔离触摸算法3b44b1b、后台负载策略及新的连续输入/vsync/频率/SpringBoard诊断。隔离ASan/UBSan复跑上游新增用例：当前触摸算法在安静扫描释放断言失败，上游完整通过；仅主机行为证据。LAT1新38/39/41与Uke冲突，不能直接cherry-pick。没有合运行代码、新镜像或设备操作；当前2098未变。
详见工作区docs/status/sheng-useful-review-2026-11-05.md；touch-3b44b1b-review.patch仅apply-check未应用，现有2098镜像不变。
