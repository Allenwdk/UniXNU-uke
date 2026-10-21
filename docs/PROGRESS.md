# UniXNU on Xiaomi Pad 7 (uke) — 进度文档

> **Last updated:** 2026-10-22
>
> **当前一句话状态：** AMFI launch-constraint 已解决；watchdog policy 只部分生效，后续日志仍出现 watchdogd SIGTRAP（`namespace 2 subcode 0x5`）。系统可进入 userland 并运行 `CommCenter`/`logd`/`coresymbolicationd`，但屏幕停在 Apple logo；主阻塞仍是显示/GPU 的 default FB surface（`IOMFB: default_fb_surface: No Default FB Surface found`，`display type is 0`）。下一步是 NativeMobileDisplay / Adreno 732（按用户要求暂不启动）。
>
> **维护规则：** 每次 CI run 或实机测试后更新本文件。见文末「维护约定」。

---

## 1. 目标与当前结论

- 设备：Xiaomi Pad 7 / POCO Pad X1，代号 `uke`，SoC SM7675，12 GB RAM / 256 GB UFS。
- 目标：用 UniXNU（UEFI + DarwinBoot + 4 KiB XNU + iPadOS userland）在这台设备上启动。
- 当前结论：
  - UEFI、UniXNU 菜单、payload、DarwinBoot、XNU 入口都已跑通。
  - 绿条 = 内核入口代码已执行；之后 XNU 在等根设备。
  - sheng 在同一画面也是等根设备，补上原生 UFS/存储驱动后继续（参考 `blu-sharky/sheng-ipados@6530e65`，仓库当前不可公开访问）。
  - **full 内核已完成编译里程碑**：`uke-full_defconfig` 链接了 NativeUfs、NativeAES、mockfs、PMGR、pthread 和 legacy display，并成功 prelink；下一步是实机存储/root，而不是继续追 SEP。

---

## 2. 阶段进度（对照 unixnu.com/porting/bring-up）

| 阶段 | 状态 | 证据 / 说明 |
|---|---|---|
| 1. UEFI | ✅ 完成 | UEFI 菜单/图形输出；3200×2136 framebuffer；3 MiB `SILICIUM_UEFI.fd`；stock ABL 能启动。 |
| 2. UniXNU | ✅ 完成 | 菜单、配置解析、条目/driver/kit 计数；底部状态行曾显示 `mem dt=1 ir=0 0xB2D51000-0xB7FFF41C`。 |
| 3. 负载 | ✅ 完成 | `unixnu.toml`、`xnu` 条目、80 MiB payload、ADT 都被识别；initrd 对齐问题修复后出现 `uke first boot: source 4K XNU to console`。 |
| 4. DarwinBoot | ✅ 完成 | `Exiting boot services`、entry pa、window 0x10000000、video base 0xE3940000、CurrentEL 1、SCTLR/ID_AA64 打印；绿条出现。 |
| 5. 内核入口 | ✅ 到达 | 绿条后 XNU 已运行；`SecurityLock ... LockCapsuleInterface` 是 UEFI 输出，不是内核挂点。XNU 现在等根设备。 |
| 6. 存储与根卷 | 🟡 编译完成，待实机 | `uke-full_defconfig` 已链接 `NativeUfs` + `NativeAES` + mockfs + PMGR + pthread + legacy display，CI run `39495271779` 绿色；UFS 块类已绑定到固定 prelink candidate。`userdata`/root UUID/APFS 实机路径待设备。 |
| 7. 用户态 | ❌ 未开始 | launchd、Data 卷、SpringBoard/设置界面、显示服务均未到。 |

---

## 3. 已完成的工作

### 3.1 仓库与板级配置

- 设备仓库：`Allenwdk/UniXNU-uke`；UEFI：`uefi-uke`；核心：`Allenwdk/UniXNU` 的 `uke` 分支（已同步 upstream `main`）。
- `board.toml`：
  - IPSW：iPad7,1 iPadOS 17.7.11 (21H461)，kernelcache pin `f18b4df...`。
  - `[userdata]`：`first_lba = 0x2d3000`、`last_lba = 0x3b147fa`、`lba_bytes = 4096`。
  - `[payload]`：`volume = "UKEESP"`、`size_mib = 80`、`dram_size = 0x80000000`、`panic_screen = false`。
  - `[payload.adt]`：`owned_memory_size = 0x10000000`（256 MiB）、`chip_id = 207`、`root_match_volume_uuid = 04769D26-2CA0-4825-B443-E2E5F88300F2`。
  - `[[bootimg.dtb_node]]`：splash `0xe3940000 + 0x2b00000`；`/chosen` initrd `0x1000000..0x6000000`（ABL 运行时会用实际地址覆盖）。
  - `[kernel] archives`：full 构建的 13 个 archive builder；只有 `CONFIG_XNU_BUILD_ARCHIVES=y` 的配置会执行。
  - `[gpu]`：Adreno 732 的 chip id、寄存器窗口、SID、GMEM/GMU 参数和固件候选名（来自实机 DT 和 mainline catalog）。
- `configs/`：
  - `uke_defconfig`：首控制台（corecrypto、ArmDTPlatform、ArmCpu、timer、PAN、panic region、legacy display）。
  - `uke-full_defconfig`：**可复现的 stage-6 full 配置**（corecrypto、pthread、UFS bring-up、AES recorded bindings、storage headers、mockfs、PMGR、legacy display）。
  - `uke-full-plus_defconfig`：sheng parity；保留 VirtIO/RamBlock、verified root、owner-root、BOOT_PROGRESS，等待 live audit/策略包。
  - `uke-full-minus-pmgr_defconfig`：只启用 PMGR + legacy display，用于验证 PMGR 不是 SEP 阻塞点。
  - `bootstrap_defconfig`：bootstrap/compile records + prelink candidate；不执行 board archive list。
- 设备数据：UFS `0x1d84000`、compatible `qcom,ufshc`、stream ID `0x60`；触摸 NT36532；面板 O82 3200×2136；splash `0xe3940000`；GPU `0x43030b00` / `Adreno732`。

### 3.2 UEFI 固件（uefi-uke）

- `Platforms/Xiaomi/ukePkg`：DSC/FDF/DEC、`MemoryMapLib`（72 descriptors）、`ConfigurationMapLib`、`SimpleFbDxe`、UniXNU/DarwinBoot。
- `UkeBootMemoryDxe`：
  - 从 HOB DT 的 `/memory` 添加 DRAM；`ADD_LIMIT` 从 1 GiB 提升到 **2 GiB**。
  - `PublishInitrd`：把 initrd 发布成块设备；补了 GCD 映射。
  - **initrd 512 字节对齐修复**：ABL 给的结束地址 `0xB7FFF41C` 不是 512 对齐，原代码直接拒绝；现在向下取整到扇区边界再发布。
  - 通过 configuration table 向 UniXNU 菜单发布 `mem dt/ir/start/end` 状态（最小状态通道）。
- 3 MiB FD 保持；`UKE_VENDOR_HW=0`（Mu 驱动 + SimpleFb + UniXNU + DarwinBoot，无 vendor UEFI 驱动）。

### 3.3 boot image / vendor_boot / payload

- boot image：Android v4、**无 ramdisk**（`--ramdisk none`），约 1.5 MiB；DTB 内加 splash 和 `/chosen`。
- vendor_boot：`vendor_boot_uke_full.img`（85,782,528 B），vendor ramdisk 为 80 MiB payload，DTB 为完整 stock DTB。
- payload：`UKEESP` FAT 卷，包含 `kernel.macho` + `kernel.macho.adt` + `unixnu.toml`；菜单 `timeout = 5`。
- 正常启动路径：`fastboot flash boot` + `fastboot flash vendor_boot` + `set_active a` + `reboot`。uke ABL 的 `fastboot boot` 会报 `Load Error`，不可作为测试路径。

### 3.4 内核与 CI

- 仓库已同步：`Allenwdk/UniXNU` 的 `uke` 分支 = upstream `main` + uke 驱动改动；fork 的 `main` 也已快进到 upstream `main`。CI 使用 fork 的 `uke` 分支。
- GitHub Actions `build-kernel.yml`：
  - `kernel` job：bootstrap → first-console archives（corecrypto、ArmDTPlatform、ArmCpu、legacy display）→ `uke_defconfig`。
  - `full` job：缓存恢复 → bootstrap → **13 个 board archives** → `uke-full_defconfig` 链接 + prelink。
  - 缓存：`UniXNU/out` + `UniXNU-uke/.cache`（key 含 UniXNU commit 和 board/config/scripts 哈希），以及 Homebrew 下载缓存；`concurrency` 会取消旧 run。
  - 构建开关：`CONFIG_XNU_BUILD_ARCHIVES=y` 才执行 board archive list；`bootstrap_defconfig` 不执行，避免 archive builder 依赖尚未生成的 prelink candidate。
  - 依赖拉取：corecrypto 改为 unixnu feed（不再手动 clone）；`IOStorageFamily` 作为 device feed，并加入 unixnu 的 kernel REFERENCES。
  - UFS unverified 构建：设备 override 修 `timerTicks`/`native_ufs_read_ticks` 的守卫，并把 GCC UFS 窗口改成 uke 的 `0x177000`/`0x19e000`。
  - UFS 块类符号：新增 `build-ufs-original-bindings.py`，从固定 prelink candidate 推导 190-slot vtable、metaclass、构造/析构地址，补进 `libNativeUfs.a`。
  - PMGR audit 需要 `kernelcache.release.ipad7.decompressed` → staging 到 `.cache/firmware/kernelcache.macho/`。
  - Adreno 关闭时 `NativeLegacyDisplay` 的 4 个 GPU fallback 符号由 `package/LegacyDisplayStubs.c` 提供。
- 成功 run：`39495271779`（`kernel` 与 `full` 均绿；`39493263420` 是首次 full 绿，`39495271779` 是文档/GPU 准备后的复跑）。
- full 产物：`uke-kernel-full/kernel.macho`，**52,121,600 B**，sha256 `62eac24259d3beae2c84e795a662ca28c231bbc338d5736bf1ab93c517853f99`（每次链接的 LC_UUID 不同，sha 会变；以 artifact 内的 `.sha256` 为准。首次绿 run 的 sha 是 `e1119e44…`，大小相同）。
- archive 证据：`out/uke/logs/archive-01..13-*.log`；UFS binder 输出 `PASS: 11 NativeUfs bindings ... 176 kernel method bindings, 190 slots`；AES 使用 recorded bindings。
- 旧 full-pmgr 产物保留：51,994,624 B，sha256 `d2b28ced43e055bde0f234fdc0db18d60fba64118a7dbbdd1cf15847522d37e6`。

### 3.5 实机结果

- 2026-10-21：`native-ufs-root=1` + `native-ufs-allow-unverified=1` + `native-ufs-diagnostics=1`（不刷 userdata）启动，诊断页显示：
  - `UFS ROOT FAILURE: root manifest rejected`
  - `HC cap=d5c7031f ver=00000400 hcs=0000010f hce=00000001 db=00000000 is=00000000`
  - `DMA=acaf0000 UTRL=0 SMMU smr=80000060 s2cr=00000046 cbar=00010000 sctlr=00000000`
  - `CMD n=7 type=01 op=28 lun=00 ocs=00 rsp=21000000 scsi=0000 power=33`
  - `PROTOCOL reject=0 residual=0 sense=000000 SMMU slot=109 id1=60000047`
  - `QUEUE rsr=0 ahit=0 polls=11 dbSeen=1 isSeen=0 isBefore=400 contiguous=22000`
  - `GPT/read lba=2d3000 userdata=2d3000..3b147fa magic=17bc1e8b f86dfb83 manifest=1`
  - `WINDOWS found=0 prepared=0 published=0`
- 结论：UFS host、SMMU、队列、UIC、GPT 读取和 root 模式都正常；唯一的失败是当前 userdata 没有 `SHNGROOT`（manifest status 1）。诊断模式把软失败变成了诊断 panic；不带 `native-ufs-diagnostics=1` 时会软失败并继续等根设备。
- 之后按用户选择**恢复 Android**：刷回 KernelSU `boot_a_kernelsu.img` / `vendor_boot_a_kernelsu.img`，`sys.boot_completed=1`，`su` 正常，`/sdcard` 可访问；userdata 未刷入，Android 数据完好。
- 2026-10-21（用户确认后）：刷入 8.74 GiB `SHNGROOT` userdata（12 个 sparse chunk，40.9 秒），再刷 boot + root-mode payload。UFS 成功发布窗口并挂载 root，但 `bsd_init.c:1098` 报：
  - `rootvp not authenticated after mounting`
  - `TCR=0x00000022a51a251a`、`SCTLR=0x000000003454599d`、`pages=0xbaee`、`free=0xaed2`
  - 含义：root 已经挂载，失败的是 iOS 的 `FSIOC_KERNEL_ROOTAUTH` sealedness 检查（自定义 boot 链没有 Apple root seal），不是 UFS 读失败。
- 已加设备侧 XNU patch `patches/xnu/offline-owner-root-binding-audit.patch`：`native-ufs-allow-unverified=1` 时跳过该 panic；默认 owner-root 路径不变。补丁已在完整 xnu patch 序列上本地验证；CI run `39549662508` 正在构建新内核。
- 2026-10-21（后续）：root-auth 放行 + 原始 trust cache（`7416ca4f…`）后，launchd 成功 exec；随后报 `Library not loaded: /usr/lib/libSystem.B.dylib`，因为 dyld cache 在 SystemOS。
- 加 `OWNER_CACHE_MOUNT`/`OWNER_CACHE_STARTUP` 后 launchd 仍找不到 libSystem（env var 路径不够）；改为 **`OWNER_UNION_SYSTEMOS=y`**，把 SystemOS 以 `MNT_UNION` 覆盖到 `/`。union 内核（sha `f75d4a19…`）启动后 launchd 加载了 libSystem，新的失败是：
  - `mount[3] exited -- no exit reason available -- (signal 0, exit status 66 )`
  - 上游 `scripts/check-mount-phase-checkpoints.py` 实际证明：exit 66 的根因是 `/sbin/mount -P 1` 取不到 `boot-manifest-hash`，不是 Data 卷缺失；原始 stderr 同时有 `data volume missing, but not required in env: 0`，但致命错误是 `failed to get boot-manifest-hash`。
- sheng 最新 commit `7562a77` 加了 `sheng-data-persist=2`：Data 盘由 userdata 末尾的读写窗口提供，重启后保留；核心 `NativeUfs`（persistent store window）和 `NativeRamBlock`（overlay）已实现。sheng 还开 `OWNER_DATA_MOUNT`/`OWNER_BOOT_MANIFEST` 并用 `--data-ramdisk` 提供 32 MiB APFS Data seed（`prepare-owner-data-fixture.py`，需要 macOS hdiutil）。
- 2026-10-21：首次带 Data seed 启动时 UniXNU 菜单返回 `Compromised Data`。原因：`[payload.adt] packed_data_ramdisk = true` 让 ADT 写 `PackedRAMDisk`，但 seed 是原始 APFS `.cdr`，DarwinBoot 的 `RamdiskPacked` 为 false，`DarwinBoot.c:536` 返回 `EFI_COMPROMISED_DATA`。
- 修复：用 DarwinBoot 自带的 `drivers/DarwinBoot/Test/packed_ramdisk.py` 把 32 MiB seed 打包成 `DBRDISK1`（995,408 B，sha `058109c4…`，逻辑 32 MiB，seed 身份仍是 `d92b2443…`）；payload 重建为 64 MiB，vendor_boot 69,005,312 B。新的 userland Data 内核 sha `a7317807…`，带 `OWNER_DATA_MOUNT`/`NATIVE_RAM_BLOCK`/`OWNER_UNION_SYSTEMOS`。
- 2026-10-21：packed Data seed 启动后不再有 `Compromised Data`，内核 Data 挂载没有触发 `offline scratch Data mount failed`，launchd 继续跑到 `mount[3]`。结合上游检查，确认剩余失败是 `/chosen` 缺少 `boot-manifest-hash`。
- 修复：`[payload.adt] boot_manifest_hash` 显式提供 48 字节 bring-up owner boot-set 身份（`SHA-384("UkeBringup.BootSet.v1\\0")`，不是 Apple personalization）；`make-uke-adt.py` 写入 `/chosen/boot-manifest-hash`，`payload.py` 从 board.toml 传参。新 payload sha `2da6f5ff…`，vendor_boot `vendor_boot_uke_data3.img` sha `0b79b7d8…`。
- 2026-10-21：`mount[3]` 修复刷入后仍然看不到内核日志，屏幕黑掉后重启。定位到两个独立问题：
  - DEVELOPMENT 内核的 `kprintf` 被 `disable_kprintf_output=true` 门控；`-v`/`debug=0x14e` 只放开串口输出，而 uke 没有 UART。已在 `[payload] kernel_args` 加 `-enable_kprintf_spam`。
  - 启用 kprintf 后，`initialize_screen()` 把 DarwinBoot 传来的 GOP **物理** framebuffer 地址当成虚拟地址（`v_baseAddr` 4 字节对齐、bit0 未置位），第一次 console 写入就 fault，表现为黑屏/重启。内核补丁 `patches/xnu/zzzzz-video-console-physical-fb.patch` 在 `newVideoVirt < VM_MIN_KERNEL_ADDRESS` 时也走 `ml_io_map_unmappable()`；`PE_state.video.v_baseAddr` 保持物理地址，不影响 NativeGOPPresenter/NativeLegacyDisplay 的匹配。
  - 映射修好后仍有第三道门：`PE_create_console()` 走 `kPEGraphicsMode`，`gc_graphics_boot=TRUE`；`vcattach()` 只在 `gc_graphics_boot == FALSE` 时启用文字 console，所以图形启动下 `vcputc` 仍然丢弃输出。补丁 `patches/xnu/zzzzzz-video-console-text.patch` 加 `-uketextconsole`：`vcattach()` 里调用 `initialize_screen(NULL, kPETextScreen)`，设置 `gc_desire_text`，随后的 `kPEAcquireScreen` 以 `gc_enable(TRUE)` 打开文字 console。
- 2026-10-21（实机）：三种补丁后**内核日志和 userland 都起来了**。第二次重启能看到 Apple logo，日志显示 launchd 之后的 `cfprefsd`、`accountsd`、`fseventsd`、`thermalmonitord`、`wirelessinsightsd`、`UserEventAgent`、`MobileGestaltHelper` 等进程。主要错误是 Data 卷太小：
  - `fs_tx_enter_internal:27549: disk1s1 cfprefsd: ENOSPC: ttype DELETE num_blocks 6010 free_blocks 5420 ...`
  - 32 MiB APFS Data seed 在 userland 启动后很快耗尽；`sheng-data-persist=2` 的持久窗口存在，但 APFS 容器本身只有 32 MiB，无法扩容。
  - 另外有 `IOMFB: default_fb_surface: No Default FB Surface found. Return error`（显示/GPU 下一阶段）、`invalid denylist` 和 `logd ... crossed memory high watermark (16 MB)`（日志洪水副作用），不是当前启动阻塞。
- 修复：`grow-data-seed.yml` 复用已有 `uke-data-seed` 32 MiB 产物，用 macOS `diskutil apfs resizeContainer` 把 APFS 容器扩到 **214 GiB**（userdata 241,617,055,744 B − root 9,382,658,048 B − 1 GiB log reserve − 4 MiB guard − 1 GiB margin），再打包为 `DBRDISK1`。结果：逻辑 229,780,750,336 B，encoded 1,085,568 B，encoded sha `9f3ffc70…`，seed 身份 `29c86166…`，卷 UUID 仍是 `292E72B9-…`。新 payload sha `ff367383…`，`vendor_boot_uke_data7.img` sha `17cd5c69…`；等设备充电后刷入验证 ENOSPC 是否消失。
- 2026-10-21：data7 首次启动在 UniXNU 菜单返回 `Bad Buffer Size`。根因不是 seed 格式，而是 **boot.img 里的 DarwinBoot 太旧**：UEFI FD 于 00:15 编译时 `PackedRamdisk.h` 只允许 `Logical <= SIZE_2GB`（2 GiB）；214 GiB 的 packed seed 被 `RamdiskLayout` 拒绝，DarwinBoot 返回 `EFI_BAD_BUFFER_SIZE`。当前 submodule 的 commit `85332b3` 已把上限改为 1 TiB；重新执行 `unixnu build firmware` 后 FD sha `7ba803a6…`，新 boot image `boot_uke_1tib.img` sha `522fe6d0…`（1,515,520 B，DTB 仍是 `57e52bb8…`）。下次刷 `boot_uke_1tib.img` + `vendor_boot_uke_data7.img`。
  - 固件 preflight 在本地 Linux 需要显式路径：`UNIXNU_UEFI=/home/allenwdk/uefi-uke UNIXNU_DTB=/home/allenwdk/UniXNU-uke/build/uke/uke.dtb`，并先 `mkdir -p out/uke/tree/build/uke`（check-uke.py 会写 `build/uke/check.json`）。
- 2026-10-21：1 TiB boot 后 `Bad Buffer Size` 消失，Data 挂载成功，`Early boot complete`、`launchd_cache_loader`、`driverkitd active`、`cfprefsd`/`accountsd`/`MobileGestaltHelper` 等都已启动，ENOSPC 不再出现。新的失败在 userland 显示阶段：
  - `shared_region: ... [[(launchd)]] check_np(...) vm_shared_region_start_address() failed`（两次）
  - `vm_swap_create_file failed @ 16 secs` / `Failed to open swap file 30`
  - `IOMobileFramebufferUserClient::s_map_display_type ... UserClient display type is 0`
  - 之后屏幕黑掉并重启；有时在跑码阶段就重启，存在不稳定性。
- 诊断：又做了一个关闭 `offline-legacy-gop`/`offline-mobile-graphics-client`/`offline-mobile-graphics-load` 的 payload（`vendor_boot_uke_diag.img`，sha `db047b2d…`），仍然黑屏重启，说明不是显示驱动单独把 console 弄黑，panic 文本没有被画出来/没来得及画。现在缺少可持久化的 panic 或串口输出；下一步要么实现 panic region/`sheng-log` 持久化，要么先补显示 default surface。
- 2026-10-21：实现持久 panic 日志的读取。UEFI 端直接读 `0xC4000000` 的保留区在 `Reserv` 和 `BsData` 两种映射下都触发 `ArmCpuDxe` data abort，因此放弃 UEFI 读取。改为内核侧：ADT 加 `pram`/`embedded-panic-log-size`，`check_for_panic_log` 在清空前把上一轮日志复制到静态缓冲，`PE_init_printf(TRUE)` 后 `pe_print_last_panic()` 用 `PREVIOUS XNU LOG:` 打印到 framebuffer console。新 boot `boot_uke_kpanicprint.img` sha `4cef631b…`（含内存映射预留、无 UEFI 读取）；内核补丁 `patches/xnu/zzzzzzz-last-panic-print.patch` 正在 CI 构建。
- 2026-10-21：`boot_uke_kpanicprint2.img` 去掉了新增的 UEFI 内存描述符，pram 改用已有的 `llcc_lpi_region@ff800000`（`0xFF800000`/`0x380000`），UEFI assert 消失。但第一次启动没有出现 `PREVIOUS XNU LOG`，说明这次重启没有走内核 panic 写入路径（更像用户态/watchdog 复位），reserved panic region 没有内容。
- 2026-10-21：采用 `sheng-log` 的 msgbuf 环形缓冲思路（先不做 UFS reader）：`patches/xnu/zzzzzzzz-uke-last-log-ring.patch` 在 `load_init_program` 里用 `sheng-log=1` 启动内核线程，每秒把 `msgbufp` 写进 pram 区并写 `FUNK` 头；下一次启动 `check_for_panic_log` 复制**最新** 2 KiB，`pe_print_last_panic()` 打印。`zzzzzzzzz-uke-last-log-priority.patch` 修正 BSD 侧看不到 `MACH_KERNEL_PRIVATE` 里 `kernel_thread_start_priority` 声明和 `BASEPRI_KERNEL` 的编译错误。等 CI 成功后重打 payload（加 `--kernel-arg sheng-log=1`）。上游 UFS 版 writer/reader 结构见 `UniXNU-sheng/package/BootLog.c` 和 `package/linux-log`。
- 2026-10-22：对原 iPad7,1 `IOMobileGraphicsFamily`（`__TEXT_EXEC` sha256 `2e044ee6…`）做指令级审计，确认 `default_fb_surface` 的根因**不是** `display type is 0`：
  - `IOMobileFramebufferUserClient::s_map_display_type` 只是把 userland 传入的 scalar 写进 user client 的 `+0x10c` 并打印，然后返回 0；它是信息，不是失败。
  - `s_default_fb_surface` 是 selector 3（2 个 scalar 输入、1 个 scalar 输出）→ helper `0xfffffff005777464` → `fb->slot174(w,h)` → `IOCommandGate::runAction` → `fb->slot333` → 返回 `IOSurfaceID`；随后 `slot242` 把该 surface 的 send-right 转成 Mach port name 作为 scalar 输出。
  - `slot333` 读 `IOMobileFramebufferLegacy + 0x1618`；整个 kernelcache 里该字段只有 `init`/`stop`/提交失败三处 `str xzr`，**没有任何非零写入**。原厂平台驱动（AppleCLCD/DisplayPipe 那层，未包含在 port 里）负责安装它。
  - 原厂 `start()`（slot 86）在 `startHardware`（slot 288，我们的实现）返回 true 后，若 `+0x1780` bit0 置位且宽高非零，会调用 `slot299` 创建临时 surface 到 `+0x1620`；`+0x1700` 是 mode 块（首字是 pixel format，`+0x1704` 是 plane 数，`+0x1708` 是每像素字节数，`+0x1748` 是 layout 标志）。
  - 新审计 `scripts/audit-mobile-graphics-default-surface.py` 绑定 slot174/242/299/333、user-client dispatch、`+0x1600/+0x1608/+0x1610/+0x1618/+0x1620/+0x17c0/+0x1780/+0x1700` 等字段；`build-native-legacy-display.py` 已在 legacy audit 之后接入。
  - `package/LegacyDisplayProbe.cpp` 增加 `offline-legacy-default-surface=1`：`start()` 前设置 `+0x1780` 并填 BGRA/1 plane/4 bpp 的 mode 块，`start()` 后把 `+0x1620` retain 并写入 `+0x1618`，日志 `UKE: display surface root=… temp=… flag=… dims=… mode=… sid=…`；同时打印 `+0x17c0`（PurpleGfxMem 缓存）和 `PE_i_can_has_debugger`。本轮内核已构建（见下一条）。

- 2026-10-22：CI run `39649725296` 的 `full` job 成功（`uke-userland_defconfig`，含新审计、探针和 `NativeHidInput` touch sink）：kernel `dcf0b7d9…`，payload `baed2c67…`，vendor_boot `vendor_boot_uke_surface.img` `e3cca9fe…`（69,005,312 B，vendor ramdisk sha 与 payload 一致）。boot 仍用 `boot_uke_kpanicprint2.img`（`5be4b635…`）。下一步：设备进 fastboot 后刷两个槽位，读取 `UKE: display surface root=… temp=… flag=… dims=… mode=… sid=…`。

- 2026-10-22：probe 内核实机启动后在 `layout_buffers`（`0xfffffff00578d110`）data abort：`surface+0x28` 的 memory region 为 0（没有 GPU 时原厂 lookup 找不到 `PurpleGfxMem`），`ldr x8,[x0]` 空指针。修复：`start()` 前把 `+0x17c0` 预置为 32 MiB `IOBufferMemoryDescriptor`；已确认其 vtable `+0xb0` 与 `IOMemoryDescriptor::getLength` 是同一槽位，`layout_buffers` 用它做分配大小检查。`layout_buffers` 把行宽对齐到 0x80，得到 12928×2136≈27.6 MB，所以 surface stride 不是 GOP 的 12800，默认 surface 不能直接别名 GOP。commit `32bd7fa`；CI run `39652515090` 的 full job 成功，v2 kernel `0d90a045…`、payload `d89a53ff…`、`vendor_boot_uke_surface2.img` `96c996ca…` 已刷入两个槽位（boot 仍 `5be4b635…`）。现在等屏幕上的 `UKE: default surface region=…`、`UKE: display surface …` 和后续 swap 日志。

- 2026-10-22：为拿到默认 surface 的原始状态，构建了 gated panic 诊断内核（commit `8069f96`，kernel `c95afc51…`、payload `eed9ee7b…`、`vendor_boot_uke_surface_panic.img` `3ce3a67b…`），在 `start()` 后直接 `panic("UKE display: st=… root=… reg=… temp=… surf=… sid=… flag=… dims=… fmt=…")`；已刷入两个槽位，等屏上 panic 文本。`surf=0` 是预期的（panic 在 adopt 之前），关键是 `temp`、`sid`、`reg`、`flag`。

- 2026-10-22：panic 诊断结果：`st=1 root=0x… reg=0x… temp=0x… sid=1 flag=1 dims=3200x2136 fmt=42475241/1/4`。`temp` 非空且 `sid=1` → 原厂 slot299 用我们预置的 32 MiB 系统内存 region 成功创建并注册了默认 surface；`surf=0` 只是 adopt 之前的状态。下一步改成第一个 `newUserClient` 时 panic（commit `d284317`，CI run `39659831112`），确认显示客户端是否打开、adopt 是否生效，以及后面 presentation/watchdog 的行为。该诊断内核 `f8637059…`、payload `f36fdc2b…`、`vendor_boot_uke_client_panic.img` `71dcd576…` 已刷入两个槽位。实机结果：**没有 panic**——复位发生在任何显示 user client 打开之前，所以默认 surface 不是这次复位的直接原因。随后用 v2 内核加 `wdt=-1` 生成 `vendor_boot_uke_wdt_off.img`（payload `0f224e9d…`，sha `a2501050…`）刷入两个槽位，测试内核 watchdog 是否是复位源。

- 2026-10-22（日志转储，pram）：`UkeLastLog.c` 把 4 MiB pram 分成两半：低 2 MiB 保留 `FUNK` 记录（XNU 下次启动 `PREVIOUS XNU LOG` 打印），高 2 MiB（物理 `0xC4200000`，`0x200000`）按 Linux ramoops `persistent_ram_buffer` 格式（`sig=0x43474244`、`start/size` 环形缓冲、无 ECC）写 msgbuf 增量；`sheng-log=1` 已加入 `board.toml` 默认参数。该区域已在 pram 的 UEFI/ADT 保留范围内；reader 映射 `0xC4200000` 即可在 `/sys/fs/pstore/console-ramoops-0` 暴露。未修改 Linux 仓库。commit `bff40a7`，CI run `39664511819` 构建成功；kernel `8e5f49b5…`，`_uke_ramoops_initialized`/`_uke_ramoops_last_write` 符号已确认。

- 2026-10-22（串口抓到的显示日志，分析）：隔壁串口分支抓到本内核的 `UKE` 行（串口前缀被交错成 `dt_foreach`/`UPE`/`MKE`，数值仍可信）：
  - `default surface region=0x… bytes=33554432`：32 MiB 系统内存 region 生效。
  - `display surface … flag=1 dims=3208x2136 mode=42475241/1/4 sid=1 mem=… desc=… debug=0`：默认 surface 创建成功；串口里的 3208 是 OCR 误读，实际 `PE_Video` 宽度是 **3200**（与 panic 探针的 `dims=3200x2136` 一致）；`installBootDimensions`/mode/stride 仍按 3200 计算，stride 12928。
  - `UKE client: type=0 pid=86 surf=0x… temp=0x… sid=1`：adopt 生效（`surf == temp`），显示 user client 确实打开了；之前“没有 panic”是屏幕/日志没抓到，不是客户端没到。
  - 原厂 `IODisplayFrameBufferLegacy::start` 打印 `provider was not AppleARMIODevice`，但它继续完成；不是致命错误。
  - `debug=0`（`PE_i_can_has_debugger()` 为假）说明 slot242 的 entitlement 路径可能失败；下一步应优先做 `surface_copy_port_name` 后备，并在 client/`s_default_fb_surface` 之后加探针。复位发生在 client 打开之后；`wdt=-1` 只把复位推后到 Apple logo 之后，内核 watchdog 只是一个因素。

- 2026-10-22（slot242 后备）：`NativeLegacyDisplay` 覆盖 slot242；原厂 slot242 返回 `kIOReturnNotPrivileged` 时直接调用原厂 `surface_copy_port_name`（`0xfffffff004baa90c`），复用 send-right/port 逻辑。`NativeMobileDisplay` fork 到 `Allenwdk/NativeMobileDisplay`（分支 `uke-display-slot242`，commit `b1c39b5`），core `.gitmodules` 指向 fork，core `uke` `e79c156`；builder 绑定两个 C 别名并把 242 加入 owned。CI run `39668984510` 成功；kernel `eae840e7…`、payload `6727e844…`、`vendor_boot_uke_slot242.img` `a31175fc…`，带 `wdt=-1`，已刷入两个槽位。预期日志：`UKE: default surface port fallback root=… surface=… status=…` 或 `UKE: default surface port status=…`。

- 2026-10-22（Data 可写层）：`native_ufs_store_open()` 现在在 `sheng-data-persist=2` 下最多等 5 s，等 `gStoreGeometry.valid && gHost.ready` 再开窗口。之前 `NativeRamBlock` 在 Data 挂载前只调用一次，UFS root layout 未就绪就直接返回 `no root layout`，overlay 不激活，`/private/var` 只读，`vm_swap_create_file` 报 EROFS（30），随后 `shared_region` 失败。新增 `NativeUfs: store open blocks=… first=… waited=…ms why=… ok=…` 日志。commit `abd5614`，CI run `39672640527` 构建成功；kernel `f59342f3…`、payload `c2111b98…`、`vendor_boot_uke_datawrite.img` `361b68f0…` 已刷入两个槽位（带 `wdt=-1`）。成功后 Data APFS 应可读写，launchd 才能越过 shared region，之后才到 slot242/显示；现在等 `NativeUfs: store open … ok=1` 和后续日志。

- 2026-10-22（三次启动结果）：`NativeUfs: store open … waited=0x2e why= ok=1` → Data 可写窗口已打开（46 ms）。第一次启动仍在 Data 挂载前遇到 `vm_swap_create_file`/`shared_region`（冷启动时序）；第二次启动越过 `Early boot complete`、`driverkitd`、`FileSet KC(s)`，并出现 `IOMobileFramebufferUserClient::s_map_display_type`（显示客户端已打开）；第三次启动复位为 `watchdog[112] … namespace 2 subcode 0x5`（用户态 watchdog）。`sheng-disable-watchdogd=1` 返回 17（EEXIST），策略没有更新 `disabled.plist`；已改为 `O_TRUNC` 重写并同时禁用 `com.apple.watchdogd`/`com.apple.watchdog`（commit `3e7da15`，CI run `39676889315`）。下一步：确认第二次启动的 `UKE: default surface port … status=…`，并让 watchdog 不再复位。

---

## 4. 还没做 / 当前阻塞

### 4.1 阶段 6：UFS / 存储 / root

- **编译已完成**：`NativeUfs`、`NativeAES`、mockfs、PMGR、pthread 和 legacy display 都已链接进 `uke-full_defconfig`，prelink 成功。
- **UFS 读路径已验证**：`native-ufs-probe=10` 在实机读到 GPT 和 userdata block0；`native-ufs-root=1` 走到 manifest 检查。
- **userdata 已刷入**：8.74 GiB `SHNGROOT` 镜像已写入 userdata（Android 数据已清空，用户确认）。root-mode 启动能发布窗口并挂载 OS APFS 卷。
- **root-auth 阻塞**：`bsd_init.c:1098` 的 `FSIOC_KERNEL_ROOTAUTH` 在自定义 boot 链上返回错误，触发 `rootvp not authenticated after mounting`。已加设备侧 patch：`native-ufs-allow-unverified=1` 时跳过该 panic；等 CI 新内核。
- **root/launchd 已到达**：root-auth 放行 + trust cache 后 launchd exec；union mount 后 libSystem 可加载。
- **userland 已启动**：root-auth 放行 + trust cache + union SystemOS + boot manifest hash + 文字 console 后，launchd 启动，`cfprefsd`/`accountsd`/`fseventsd`/`thermalmonitord`/`wirelessinsightsd`/`UserEventAgent`/`MobileGestaltHelper` 等进程都出现在日志里，Apple logo 也能显示。
- **当前阻塞：默认 FB surface**：Data 容量问题已由 214 GiB seed 解决，用户态已起来；但 `IOMobileFramebufferLegacy` 的默认 surface（实例 `+0x1618`）没有被安装。原厂 `start()` 在 `startHardware` 返回 true 后，只有 `+0x1780` bit0 置位时才会调用 slot299 创建临时 surface 到 `+0x1620`；原厂平台驱动（未包含在 port 里）负责把它安装到 `+0x1618`。`display type is 0` 只是 `s_map_display_type` 记录 userland 传入值，不是失败。修复与探针见上面的 2026-10-22 记录。
- UFS 块读取目前是 **unverified bring-up**：块类已绑定到原始 class，但每次读不做 policy 校验。`native-ufs-root=1` 在未认证构建里默认拒绝发布；新加的 `native-ufs-allow-unverified=1` 是明确的 bring-up 开关，只用于发布窗口，不改变读取认证状态。认证读取仍需要 verified-root policy bundle 和 live storage ABI audit。
- UFS GCC 窗口已按 uke/SM7675 改为 `0x177000`/`0x19e000`，实机读取通过。
- payload 已带 `native-ufs-root=1` + `native-ufs-allow-unverified=1` 的 root-mode 版本（`vendor_boot_uke_root_nodata.img`）；未刷 userdata 时会停在 manifest 检查。
- VerifiedRoot/owner-root、VirtIO/RamBlock、BOOT_PROGRESS 仍被 live audit/策略包门控，见 `uke-full-plus_defconfig`。

### 4.2 参考实现

- `blu-sharky/sheng-ipados@6530e65` 当前 GitHub 返回 **Repository not found**（可能私有/改名），无法直接读取 diff。
- 如果拿到该 commit 的 patch/关键文件，优先按它补 `NativeUfs`/存储/root；否则按 sheng 设备页的 `sheng-pmgr=2` + `native-ufs-root=1` 路径实现。

### 4.3 其他未做

- `panic_screen=true` 已启用；`Font.h` 从 Mu-Silicium vendored 到 `scripts/Font.h`，Linux/CI 无 UEFI checkout 也能打补丁。panic 文本现在能在屏上稳定显示。
- `boot-progress=1/2` 仍需要 audited UFS/owner-root + VirtIO + source-corecrypto；当前 full 配置没有这些，所以保持关闭。
- `sheng-platform-stage`、`sheng-aks=1`、`amfi_get_out_of_my_way=1`、`cs_debug`、`kextlog` 等 boot args 尚未系统测试；它们不是当前 root 阻塞的优先项。
- GPU 已收集 A732 的真实 `gen71100_*` 固件和 9 档频率（`docs/GPU-PORT.md`）；驱动仍是 A740 常量，`CONFIG_XNU_NATIVE_ADRENO` 关闭。
- 触摸、Wi-Fi/蓝牙、USB、音频、传感器、电池、suspend/resume 均未做。
- `owned_memory_size` 目前 256 MiB；sheng 用 4.75 GiB。uke 的 DT reserved-memory 把最大连续块压到 326 MiB；如需更大窗口，要裁剪 reserved-memory，而不是只调大 `ADD_LIMIT`。
- `kernel` 与 `full` 两个 CI job 已在 run `39495271779` 同时绿；UFS 驱动改动后会触发新一轮 full 构建。

---

## 5. 下一步（按优先级）

1. **刷 userdata（需用户确认，会清空 Android 数据）**：`fastboot flash userdata out/uke/userdata/userdata-root.img`，再刷 boot + root-mode payload；验证 `SHNGROOT`、OS/SystemOS 窗口、APFS 卷和 `root_match_volume_uuid`。
2. **启动到 root/launchd**：观察 `NativeUfs: published ...` 日志、OS 卷挂载和 launchd；若失败，用 panic screen / `native-ufs-diagnostics=1` 定位。
3. **认证读取/owner-root**：OS/SystemOS DMG 已拉取并校验；生成 verified-root policy bundle，补 live storage ABI snapshot，再开 `VERIFIED_ROOT_PROBE`/`OWNER_ROOT_*`/`BOOT_PROGRESS`（`uke-full-plus_defconfig`）。
4. **GPU**：固件已收集（`gen71100_*`）；按 `docs/GPU-PORT.md` 改 `build-native-adreno-kits.py` 的 pin/路径，加 Adreno archives，再开 `CONFIG_XNU_NATIVE_ADRENO`。
5. **阶段 7**：launchd、Data 卷、SpringBoard/设置界面；之后再处理显示/触摸/GPU 运行时。
6. `panic_screen` 已启用；后续 panic 直接看屏。
7. 每次测试后恢复 Android（KernelSU boot/vendor_boot 备份在 `uke-port/data/*_kernelsu.img`），并更新本文档。

---

## 6. 关键产物与命令

### 6.1 产物

| 产物 | 路径 | 说明 |
|---|---|---|
| UEFI FD | `uefi-uke/Build/ukePkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd` | 3 MiB，含 initrd 对齐/2 GiB/状态通道修复 |
| **full 内核** | CI artifact `uke-kernel-full/kernel.macho` | **52,121,600 B，最新 sha256 `62eac242…`（每次链接会变，以 artifact `.sha256` 为准）** |
| full 内核（本地副本） | `/home/allenwdk/uke-port/ci/uke-kernel-full/kernel.macho` | 52,121,600 B，sha256 `62eac242…` |
| **root-mode 内核** | `/home/allenwdk/uke-port/ci/uke-kernel-full-allow/kernel.macho` | 带 `native-ufs-allow-unverified`，52,121,600 B，sha256 `f81e81b7…` |
| **userdata 镜像** | `/home/allenwdk/UniXNU/out/uke/userdata/userdata-root.img` | 8.74 GiB，sha256 `fd43ee70…`，已刷入 userdata |
| root-mode payload | `/home/allenwdk/uke-port/data/vendor_boot_uke_root_nodata.img` | 85,782,528 B，`native-ufs-root=1` + allow-unverified |
| trust-cache payload | `/home/allenwdk/uke-port/data/vendor_boot_uke_root_auth_trust.img` | + 原始 Static/System trust cache |
| userland union payload | `/home/allenwdk/uke-port/data/vendor_boot_uke_userland_union.img` | + `OWNER_CACHE_MOUNT/STARTUP/UNION_SYSTEMOS`，launchd 已启动 |
| 旧 full-pmgr 内核 | `/home/allenwdk/uke-port/ci/uke-kernel-full-pmgr/kernel.macho` | 51,994,624 B，sha256 `d2b28c...` |
| payload | `out/uke/payload/payload.img` | 80 MiB，5 秒菜单，256 MiB owned memory（旧 full-pmgr payload） |
| vendor_boot | `/home/allenwdk/uke-port/data/vendor_boot_uke_full.img` | 85,782,528 B，80 MiB payload + full DTB |
| boot image | `out/uke/boot/boot.img` | v4，无 ramdisk，sha256 `faa65f6b...` |
| stock boot/vendor | `/home/allenwdk/uke-port/data/boot_a.img`、`vendor_boot_a.img` | 恢复 Android 用 |

### 6.2 常用命令

```sh
# payload（unique chip id 由外部提供，不要提交）
cd /path/to/UniXNU
./unixnu build payload -D ../UniXNU-uke \
    --kernel /path/to/kernel.macho --unique-chip-id "$CHIP"

# 5 秒菜单（构建后修改 payload 内的 unixnu.toml）
# timeout = 5 / fallback_timeout = 5

# vendor_boot 的 vendor ramdisk 换成 payload（80 MiB）
# 详见仓库外的一次性脚本；header 不变，payload 从 0x1000 开始

# 无 ramdisk boot image
./unixnu bootimg -D ../UniXNU-uke --uefi ../uefi-uke \
    --dtb /path/to/vendor_boot.dtb --ramdisk none

# 正常启动
fastboot flash boot out/uke/boot/boot.img
fastboot flash vendor_boot /path/vendor_boot_uke_full.img
fastboot set_active a
fastboot reboot

# 恢复 Android
fastboot flash boot /path/to/boot_a.img
fastboot flash vendor_boot /path/to/vendor_boot_a.img
fastboot set_active a
fastboot reboot
```

### 6.3 CI

- `workflow_dispatch` 或 push 到 `main`（触发路径见 workflow，含 `drivers/**`、`kits/**`）。
- artifacts：`uke-kernel`（first-console）和 `uke-kernel-full`（stage-6 full）。
- 最近成功 run：`39495271779`（两个 job 同时绿；`39493263420` 为首次 full 绿）。
- 缓存：`UniXNU/out` + `UniXNU-uke/.cache` + Homebrew 下载；第二次运行会复用 bootstrap 对象，冷启动约 15–17 分钟，缓存命中后约 8–10 分钟。
- 日志：`out/uke/logs/kernel.log`、`prelink.log`、`archive-01..13-*.log`；artifact 内 `logs/`。

---

## 7. 已知坑 / 经验

- **`fastboot boot` 不可用**：uke ABL 对 stock/custom 都报 `Failed to load/authenticate boot image: Load Error`；必须用 `fastboot flash boot` + `flash vendor_boot` + `reboot`。
- **ABL 槽位**：UEFI 不标记成功，多次启动后 `slot-unbootable:a` 会变 yes；每次测试前 `fastboot set_active a`。
- **initrd 对齐**：ABL 的 `linux,initrd-end` 不是 512 对齐；`PublishInitrd` 需向下取整，不能拒绝。
- **`NO WINDOW`**：DarwinBoot 要 1 GiB 连续窗口；uke 最大连续块只有 326 MiB；`owned_memory_size` 必须降到 256 MiB，`ADD_LIMIT` 提到 2 GiB 以保留更大空闲块。
- **`LockCapsuleInterface` 是 UEFI 输出**：不要把它当成 XNU/SEP 挂点；绿条后 XNU 在等根设备。
- **`AppleSEPManager` 是误导**：预链接内核里有该 kext，但 sheng 同类画面最终是等根设备；不要先做 SEP 排除。
- **corecrypto**：现在是 unixnu feed（`refer/corecrypto-2023`，pinned `11fba65…`），`prepare` 会自动拉取；不再手动 clone，也不需要 `rm -rf`。
- **`IOStorageFamily`**：作为 device `feeds.toml` 的 refer source（pinned `e45fbe3…`），并加入 unixnu 的 kernel REFERENCES，`prepare` 会拉取并链接进 tree。
- **`CONFIG_XNU_BUILD_ARCHIVES`**：board archive list 只在配置打开时执行；bootstrap 和 first-console 不执行，避免依赖尚未生成的 prelink candidate。
- **UFS unverified 构建**：`build-arm-driver.py NativeUfs --unverified` 跳过 `NATIVE_VERIFIED_BLOCK`；设备 override 把 `timerTicks`/`native_ufs_read_ticks` 移出 verified 守卫，并把 GCC UFS 窗口改为 uke 的 `0x177000`/`0x19e000`。
- **UFS 块类绑定**：`build-ufs-original-bindings.py` 从固定 prelink candidate 推导 190-slot vtable、metaclass、构造/析构，生成 `libNativeUfs.a` 的 `original-bindings.o`；这是 bring-up 绑定，不是 live-qualified audit。
- **`native-ufs-allow-unverified=1`**：未认证构建里 `native-ufs-root=1` 默认拒绝发布；该参数是明确的 bring-up 开关，只发布 OS/SystemOS 窗口，读取仍不认证。发布线程在 unverified 模式下等待任意 media 后代，而不是签名 UUID。
- **UFS 实机探针**：`native-ufs-probe=10` 只读，panic 输出 `userdata first/last`、block0 magic、ext4 magic、manifest status；`probe=11` 检查每个窗口的 GPT。panic 会停在屏上，长按电源重启。
- **`packed_data_ramdisk=true` 要求 `DBRDISK1`**：ADT 写 `PackedRAMDisk` 时 DarwinBoot 要求 ramdisk 是打包格式；原始 APFS `.cdr` 会让 `RamdiskPacked=false`，在 `DarwinBoot.c:536` 返回 `EFI_COMPROMISED_DATA`。用 `drivers/DarwinBoot/Test/packed_ramdisk.py` 的 `pack()` 打包后再传给 `--data-ramdisk`。
- **Data seed UUID**：`OWNER_DATA_VOLUME_UUID` 必须等于 seed 的 APFS 卷 UUID（本次 `292E72B9-7594-431D-ADBC-2D354CC918DD`）；seed 由 `build-data-seed.yml` 在 macOS 上用 `prepare-owner-data-fixture.py` 生成。
- **vendor_boot 重排**：payload 尺寸变化时 DTB/table 偏移会移动；用 `scripts/patch-vendor-boot.py` 重排，并用 `unpack_bootimg` 核对 `vendor ramdisk total size`、`dtb size` 和 table entry。
- **panic screen**：`scripts/Font.h` 是 Mu-Silicium 的 BSD 字体表；`patch-xnu-entry-probe.py` 在找不到 UEFI checkout 时回退到 `scripts/`。
- **大文件解压要留足空间**：之前 8.2 GB DMG 在磁盘接近满时解压出内容错误（尺寸对、sha 不对）；清理后 `unzip` 解出的 sha 与 pin 完全一致。构建 userdata 前保持 >20 GB 空闲，并先校验 sha。
- **Linux userdata 构建**：`build-userdata-root-image.py` 用 `7z x -tDmg -so` 解 OS 的 UDIF 整盘流，SystemOS 是裸 APFS 容器直接用；生成后核对 `SHNGROOT` 头、窗口 sha 和 GPT/APFS magic。
- **`native-ufs-diagnostics=1`**：会把 manifest 拒绝等软失败变成诊断 panic；正常启动不要带它，或只看诊断。
- **CI 缓存 key**：包含 UniXNU commit + `board.toml`/`feeds.toml`/`configs/**`/`scripts/**` 哈希；改这些文件会换主 key，但 `restore-keys` 仍会复用旧 `out/` 树。`drivers/**` 不影响 bootstrap 对象，故不在 key 里。
- **`BOOT_PROGRESS`**：仍然要求 audited UFS/owner-root + VirtIO + source-corecrypto；full 配置保持关闭。
- **Adreno 关闭**：`NativeLegacyDisplay` 需要 4 个 GPU fallback weak stubs。
- **`panic_screen`**：`patch-xnu-entry-probe.py` 需要 `Font.h`；当前关闭，修好后再开。
- **`owned_memory_size`**：sheng 4.75 GiB，uke 256 MiB；不是越大越好，受 DT reserved-memory 碎片限制。
- **`root_match_volume_uuid`**：当前还是 sheng 的 `04769D26...`，做 root 前要确认 uke 的 Data 卷 UUID。
- **`unique_chip_id`**：平台种子，不在仓库中保存；构建时外部传入。
- **`sheng-ipados@6530e65`**：当前 404；需要用户提供 diff/关键文件。

---

## 8. 维护约定（必须保持更新）

每次 CI run 或实机结果后：

1. 更新顶部 **Last updated**。
2. 更新「当前一句话状态」和阶段表。
3. 在下面「工作日志」追加一行：日期、动作、结果、证据（run id / sha256 / 屏幕照片描述）。
4. 完成的「下一步」移到「已完成」；未完成的保留。
5. 更新「关键产物」中的路径和 sha256。
6. 不写 token、`unique_chip_id`、个人凭证；`root_match_volume_uuid` 可以写。
7. README 只保留一句话状态和本文档链接，详细状态以本文档为准。

### 工作日志

| 日期 | 动作 | 结果 / 证据 |
|---|---|---|
| 2026-10-20 | UEFI/ukePkg、MemoryMapLib/ConfigurationMapLib、3 MiB FD | UniXNU 菜单出现；`check-uke.py` 72 descriptors / 0 static errors |
| 2026-10-20 | GitHub Actions first-console kernel | bootstrap/corecrypto/ArmDTPlatform/ArmCpu/legacy display 成功 |
| 2026-10-20 | payload + vendor_boot + boot image | payload 被识别；`xnu` 条目出现；`fastboot boot` Load Error |
| 2026-10-20 | initrd/owned memory/`NO WINDOW` | initrd end 对齐修复；`owned_memory_size=0x10000000`；`ADD_LIMIT=2 GiB`；绿条出现 |
| 2026-10-20 | full-pmgr CI | run `39472864159` 成功；内核 `d2b28c...` |
| 2026-10-20 | 实机 full-pmgr | 绿条后静止；当时误判为 SEP，后修正为等根设备 |
| 2026-10-21 | 恢复 Android | stock boot/vendor 刷回；ADB 正常 |
| 2026-10-21 | 参考文档/sheng 页面 | 确认 `LockCapsule` 是 UEFI 输出；sheng 同类画面等根设备；`sheng-pmgr=2` 是 native UFS root 必要条件 |
| 2026-10-21 | 本文档建立 | 记录阶段 1–5 完成、阶段 6 当前、下一步 UFS/root |
| 2026-10-21 | 同步仓库/fork | `Allenwdk/UniXNU` `uke` 合入 upstream `main`（15 commits）并推送；fork `main` 快进；CI 改用 fork `uke` |
| 2026-10-21 | full 配置/archives | `CONFIG_XNU_BUILD_ARCHIVES`、13 个 archive builder、`IOStorageFamily` feed、AES recorded bindings |
| 2026-10-21 | UFS bring-up | 设备 override 修 tick 守卫 + GCC `0x177000`/`0x19e000`；`build-ufs-original-bindings.py` 生成块类绑定 |
| 2026-10-21 | full CI 成功 | run `39493263420` 两 job 绿；`kernel.macho` 52,121,600 B，sha256 `e1119e44…` |
| 2026-10-21 | 文档/GPU 准备后复跑 | run `39495271779` 两 job 绿；同尺寸内核 sha `62eac242…`（LC_UUID 变化） |
| 2026-10-21 | 实机 full + UFS probe | `native-ufs-probe=10` 读到 GPT/userdata/block0；`manifest status=1`（Android data） |
| 2026-10-21 | panic screen | `Font.h` vendored；`panic_screen=true`；panic 文本稳定显示 |
| 2026-10-21 | GPU 固件 | root 读取 `gen71100_sqe.fw`/`gen71100_gmu.bin`/`gen71100_zap.mbn` + 9 档频率，写入 `board.toml [gpu]` |
| 2026-10-21 | UFS bring-up 开关 | `native-ufs-allow-unverified=1` 允许未认证发布窗口；CI 重新构建 full 内核 |
| 2026-10-21 | CI 缓存 | `out/` + `.cache` + Homebrew 缓存；`concurrency` 取消旧 run；冷/热启动约 15–17 / 8–10 分钟 |
| 2026-10-21 | GPU 准备 | A732 板级数据写入 `board.toml [gpu]`；`check-uke-gpu.py` 对照实机 DTB 通过；`docs/GPU-PORT.md` 记录剩余固件/功率输入 |
| 2026-10-21 | UFS root 诊断 | `native-ufs-root=1` 走到 manifest 检查；host/SMMU/queue/GPT 正常；`manifest=1`（当前是 Android data） |
| 2026-10-21 | userdata 镜像 | 8.74 GiB，sha `fd43ee70…`；OS `e5634d04…` / SystemOS `b4908cde…` 与 verified-root 期望一致 |
| 2026-10-21 | 恢复 Android | KernelSU boot/vendor_boot 刷回；`sys.boot_completed=1`；userdata 未刷入，数据完好 |
| 2026-10-21 | 刷 userdata + root | 8.74 GiB sparse 写入 40.9s；UFS 发布窗口并挂载 OS APFS；`bsd_init` root-auth panic |
| 2026-10-21 | root-auth 放行 patch | `native-ufs-allow-unverified=1` 跳过 `FSIOC_KERNEL_ROOTAUTH` panic；补丁本地验证；CI run `39549662508` |
| 2026-10-21 | trust cache + launchd | 原始 `7416ca4f…` bundle 后 launchd exec；缺 libSystem（dyld cache 在 SystemOS） |
| 2026-10-21 | union SystemOS | `OWNER_UNION_SYSTEMOS=y` + UFS unverified 授权函数；launchd 加载 libSystem，`mount[3]` exit 66 |
| 2026-10-21 | Data seed / RAM block | macOS CI 生成 32 MiB APFS seed；`OWNER_DATA_MOUNT`/`NATIVE_RAM_BLOCK` + RAM block binder；raw seed 触发 `EFI_COMPROMISED_DATA` |
| 2026-10-21 | packed Data seed | `DBRDISK1` 995 KB；payload 64 MiB / vendor_boot 69,005,312 B；userland Data 内核 `a7317807…` |
| 2026-10-21 | boot manifest hash | 上游检查证明 `mount[3]` 66 是缺 `/chosen/boot-manifest-hash`；ADT 写入 48 字节 bring-up hash；payload `2da6f5ff…` / vendor_boot `0b79b7d8…` |
| 2026-10-21 | kernel logs | `-enable_kprintf_spam` 放开 DEVELOPMENT 的 kprintf；补 `zzzzz-video-console-physical-fb.patch` 把物理 framebuffer 交给 `ml_io_map_unmappable()`；待 CI 新内核 |
| 2026-10-21 | userland 日志 | `zzzzzz-video-console-text.patch` + `-uketextconsole` 后日志正常；launchd 及 `cfprefsd`/`accountsd`/`MobileGestaltHelper` 等启动，Apple logo 可见 |
| 2026-10-21 | Data ENOSPC | 32 MiB seed 太小；`grow-data-seed.yml` 复用旧产物扩到 214 GiB，encoded 1,085,568 B，seed `29c86166…`；payload `ff367383…` / vendor_boot `17cd5c69…` 待复测 |
| 2026-10-21 | CI 拆分/缓存 | `build-kernel.yml` 改为 dispatch-only；新增 `ci.yml` push 校验 + `check-xnu-patch-set.sh`；cache key 改为 exact source identity（v6，无 restore-keys） |
| 2026-10-21 | AMFI kill switch | payload 加 `amfi_get_out_of_my_way=1`；`AMFI: Launch Constraint Violation (enforcing)` 消失，launchd 系统服务继续启动 |
| 2026-10-21 | watchdog 根因 | `sheng-disable-watchdogd=1` 的调用被 `XNU_BOOT_PROGRESS` 编译掉；`watchdogd` SIGTRAP（namespace 2 subcode 0x5）复位 |
| 2026-10-22 | watchdog policy | 新 patch `zzzzzzzz-uke-watchdog-policy.patch` + `CONFIG_XNU_WATCHDOG_POLICY=y`；`BootWatchdogPolicy.c` 编入 `libUkeLastLog.a`；内核 `7ec6670e…` / payload `b503aef5…` / vendor_boot `c55c6eb5…` |
| 2026-10-22 | watchdog 首次生效 | 一次启动里 `watchdog` 变为 launchd SIGKILL；越过 `amfi: Exiting Early Boot`、launchd tasks、IOACPIFamily、IOHIDSystem；系统进入 userland |
| 2026-10-22 | watchdog 复现 | 后续日志再次出现 `pid 195 ... namespace 2 subcode 0x5`（SIGTRAP）与 `coredump (watchdogd, pid 105)`；策略未完全消除 watchdog，显示仍是主阻塞；`logd`/CommCenter watermark、`MobileGestalt` unnest 为非致命噪声 |
| 2026-10-22 | 当前阻塞 | 系统 alive（`CommCenter`/`logd`/`coresymbolicationd`/APFS tx），但 `IOMFB: default_fb_surface: No Default FB Surface found` + `display type is 0`，屏幕停在 Apple logo；下一步 NativeMobileDisplay / Adreno 732（暂不启动） |

---

## 9. 参考

- UniXNU porting：https://unixnu.com/porting/
- Bring-up order：https://unixnu.com/porting/bring-up
- Drivers overview：https://unixnu.com/drivers/overview
- Adreno 740 driver：https://unixnu.com/drivers/native-adreno
- Boot arguments：https://unixnu.com/xnu/boot-arguments
- Debugging and logs：https://unixnu.com/guide/debugging
- sheng device page：https://unixnu.com/devices/sheng
- sheng 官方设备仓库：https://github.com/UniXNU/UniXNU-sheng
- sheng-ipados 参考 commit：`blu-sharky/sheng-ipados@6530e65`（当前仓库不可访问）
- 本设备仓库：https://github.com/Allenwdk/UniXNU-uke
- 本设备核心 fork：https://github.com/Allenwdk/UniXNU（`uke` 分支）
- CI：`.github/workflows/build-kernel.yml`
- GPU 准备：`docs/GPU-PORT.md`
