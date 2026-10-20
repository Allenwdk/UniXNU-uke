# UniXNU on Xiaomi Pad 7 (uke) — 进度文档

> **Last updated:** 2026-10-21
>
> **当前一句话状态：** 阶段 1–5 已到达；XNU 已进入（绿条）。`SecurityLock ... LockCapsuleInterface` 是 **UEFI 输出**，不是 XNU 挂点；绿条后 XNU 在等根设备。当前阶段是 **阶段 6：原生 UFS/存储 + root**，不是 SEP。
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
  - 所以下一步是 **UFS/存储/root**，不是继续追 `AppleSEPManager`/SEP。

---

## 2. 阶段进度（对照 unixnu.com/porting/bring-up）

| 阶段 | 状态 | 证据 / 说明 |
|---|---|---|
| 1. UEFI | ✅ 完成 | UEFI 菜单/图形输出；3200×2136 framebuffer；3 MiB `SILICIUM_UEFI.fd`；stock ABL 能启动。 |
| 2. UniXNU | ✅ 完成 | 菜单、配置解析、条目/driver/kit 计数；底部状态行曾显示 `mem dt=1 ir=0 0xB2D51000-0xB7FFF41C`。 |
| 3. 负载 | ✅ 完成 | `unixnu.toml`、`xnu` 条目、80 MiB payload、ADT 都被识别；initrd 对齐问题修复后出现 `uke first boot: source 4K XNU to console`。 |
| 4. DarwinBoot | ✅ 完成 | `Exiting boot services`、entry pa、window 0x10000000、video base 0xE3940000、CurrentEL 1、SCTLR/ID_AA64 打印；绿条出现。 |
| 5. 内核入口 | ✅ 到达 | 绿条后 XNU 已运行；`SecurityLock ... LockCapsuleInterface` 是 UEFI 输出，不是内核挂点。XNU 现在等根设备。 |
| 6. 存储与根卷 | ⏳ **当前** | `NativeUfs` 尚未链接；`native-ufs-root=1` 尚未启用；`userdata`/root UUID/OS+SystemOS 尚未接通。 |
| 7. 用户态 | ❌ 未开始 | launchd、Data 卷、SpringBoard/设置界面、显示服务均未到。 |

---

## 3. 已完成的工作

### 3.1 仓库与板级配置

- 设备仓库：`Allenwdk/UniXNU-uke`；UEFI：`uefi-uke`；核心：`UniXNU` upstream `main`。
- `board.toml`：
  - IPSW：iPad7,1 iPadOS 17.7.11 (21H461)，kernelcache pin `f18b4df...`。
  - `[userdata]`：`first_lba = 0x2d3000`、`last_lba = 0x3b147fa`、`lba_bytes = 4096`。
  - `[payload]`：`volume = "UKEESP"`、`size_mib = 80`、`dram_size = 0x80000000`、`panic_screen = false`。
  - `[payload.adt]`：`owned_memory_size = 0x10000000`（256 MiB）、`chip_id = 207`、`root_match_volume_uuid = 04769D26-2CA0-4825-B443-E2E5F88300F2`。
  - `[[bootimg.dtb_node]]`：splash `0xe3940000 + 0x2b00000`；`/chosen` initrd `0x1000000..0x6000000`（ABL 运行时会用实际地址覆盖）。
- `configs/`：
  - `uke_defconfig`：首控制台（corecrypto、ArmDTPlatform、ArmCpu、timer、PAN、panic region、legacy display）。
  - `uke-full_defconfig`：sheng 派生 full 配置（UFS/AES/Virtio/RamBlock/VerifiedRoot/owner-root/PMGR/BOOT_PROGRESS/Adreno）。
  - `uke-full-minus-pmgr_defconfig`：只启用 PMGR + legacy display，用于验证 PMGR 不是 SEP 阻塞点。
  - `bootstrap_defconfig`：bootstrap/compile records + prelink candidate。
- 设备数据：UFS `0x1d84000`、compatible `qcom,ufshc`、stream ID `0x60`；触摸 NT36532；面板 O82 3200×2136；splash `0xe3940000`。

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

- GitHub Actions `build-kernel.yml`：
  - `kernel` job：bootstrap → corecrypto → ArmDTPlatform/ArmCpu → legacy display → first-console kernel。
  - `full-pmgr` job：bootstrap → corecrypto → legacy display → `libShengPlatformPower.a` → `uke-full-minus-pmgr` 内核。
  - 已修复的 CI 问题：
    - corecrypto 源码目录不是 unixnu managed refer module，`tree.assemble` 在 macOS 上不能 unlink 目录 → 最终构建前 `rm -rf refer/corecrypto-2023`。
    - PMGR audit 需要 `kernelcache.release.ipad7.decompressed` → 从 `kernelcache.decompressed` staging 到 `.cache/firmware/kernelcache.macho/`。
    - `BOOT_PROGRESS` 强制要求 UFS/owner-root/Virtio/source-corecrypto → PMGR-only 阶段先关闭。
    - `SOURCE_PTHREAD` 需要未构建的 `libpthread_kernel.a` → PMGR-only 阶段先关闭。
    - Adreno 关闭时 `NativeLegacyDisplay` 引用 4 个 GPU fallback 符号 → 在 `package/LegacyDisplayStubs.c` 补 weak stubs。
    - workflow 触发路径加入 `package/**`。
- 成功 run：`39472864159`（full-pmgr）。
- 成功产物：`uke-kernel-full-pmgr/kernel.macho`，51,994,624 B，sha256 `d2b28ced43e055bde0f234fdc0db18d60fba64118a7dbbdd1cf15847522d37e6`。
- 已构建的 full-pmgr payload/boot：
  - `out/uke/payload/payload.img`（80 MiB，5 秒菜单，256 MiB owned memory）
  - `/home/allenwdk/uke-port/data/vendor_boot_uke_full.img`（85,782,528 B）
  - `out/uke/boot/boot.img`（v4 无 ramdisk，sha256 `faa65f6b6dcca065e126e9f23de94334f21300207250086d450727d73e9761c1`）

### 3.5 实机结果

- UniXNU 菜单出现；payload 被识别；`xnu` 条目出现。
- DarwinBoot 打印 `entry pa`、`args pa`、`window 0x10000000`、`video base 0xE3940000 3200x2136 row 12800 bytes 32 bpp`、`entry words`、`CurrentEL 1`、`SCTLR_EL1`、`TCR_EL1`、`ID_AA64MMFR0_EL1`、`ID_AA64PFR0_EL1`。
- 绿条出现；XNU 已进入。
- 之后画面静止，但结论已修正：这是 XNU 在等根设备，不是 `LockCapsuleInterface` 卡死。
- 每次测试后已用 stock `boot_a.img` + `vendor_boot_a.img` 恢复 Android；当前设备在 Android。

---

## 4. 还没做 / 当前阻塞

### 4.1 阶段 6：UFS / 存储 / root

- `NativeUfs` 尚未链接进内核。
- `native-ufs-root=1` 尚未加入 payload。
- `userdata` 镜像尚未构建/刷入；`root_match_volume_uuid`、OS/SystemOS 窗口尚未在 uke 上验证。
- `NativeAES`、`VerifiedRoot`/owner-root、APFS 这条 root 链尚未接通。
- UFS 控制器地址 `0x1d84000`、compatible `qcom,ufshc` 已有；但 uke 的 **GCC 时钟/复位/PHY/PMGR** 与 sheng (SM8550) 不同，需要逐项确认。

### 4.2 参考实现

- `blu-sharky/sheng-ipados@6530e65` 当前 GitHub 返回 **Repository not found**（可能私有/改名），无法直接读取 diff。
- 如果拿到该 commit 的 patch/关键文件，优先按它补 `NativeUfs`/存储/root；否则按 sheng 设备页的 `sheng-pmgr=2` + `native-ufs-root=1` 路径实现。

### 4.3 其他未做

- `panic_screen=true` 因 `Font.h` 路径问题暂时关闭；需要修复后开启，以便捕获真正的 panic。
- `boot-progress=1/2` 需要 full 配置（UFS/owner/Virtio/source-corecrypto），当前 PMGR-only 不能开。
- `sheng-platform-stage`、`sheng-aks=1`、`amfi_get_out_of_my_way=1`、`cs_debug`、`kextlog` 等 boot args 尚未系统测试；它们不是当前 root 阻塞的优先项。
- Adreno 732、触摸、Wi-Fi/蓝牙、USB、音频、传感器、电池、suspend/resume 均未做。
- `owned_memory_size` 目前 256 MiB；sheng 用 4.75 GiB。uke 的 DT reserved-memory 把最大连续块压到 326 MiB；如需更大窗口，要裁剪 reserved-memory，而不是只调大 `ADD_LIMIT`。
- `first-console` CI job 的最终状态需要在最新修改后单独确认；`full-pmgr` 已绿。

---

## 5. 下一步（按优先级）

1. **拿到/确认 `sheng-ipados@6530e65` 的 UFS/存储实现**（diff 或关键文件）。
2. **构建 full 配置**：`NativeUfs` + `NativeAES` + `NativeVirtioBlock` + `NativeRamBlock` + `VerifiedRoot` + owner-root + `BOOT_PROGRESS`；补齐对应 archive/audit（`IOStorageFamily`、`IOAES`、verified-root 数据）。
3. **payload 加 `native-ufs-root=1`**，保留 `sheng-pmgr=2`；先验证 UFS probe/块设备，再挂 root。
4. **准备 userdata**：`build userdata`、`root_match_volume_uuid`、OS/SystemOS 窗口，确认 uke 的 Data 卷 UUID。
5. **阶段 7**：launchd、Data 卷、SpringBoard/设置界面；之后再处理显示/触摸/GPU。
6. 修 `Font.h` 并开启 `panic_screen`；在 full 配置可用后开 `boot-progress`。
7. 每次测试后恢复 Android，并更新本文档。

---

## 6. 关键产物与命令

### 6.1 产物

| 产物 | 路径 | 说明 |
|---|---|---|
| UEFI FD | `uefi-uke/Build/ukePkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd` | 3 MiB，含 initrd 对齐/2 GiB/状态通道修复 |
| full-pmgr 内核 | `/home/allenwdk/uke-port/ci/uke-kernel-full-pmgr/kernel.macho` | 51,994,624 B，sha256 `d2b28c...` |
| payload | `out/uke/payload/payload.img` | 80 MiB，5 秒菜单，256 MiB owned memory |
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

- `workflow_dispatch` 或 push 到 `main`（触发路径见 workflow）。
- `full-pmgr` artifact：`uke-kernel-full-pmgr`。
- 最近成功 run：`39472864159`。
- 日志：`out/uke/logs/kernel.log`、`prelink.log`；artifact 内 `logs/`。

---

## 7. 已知坑 / 经验

- **`fastboot boot` 不可用**：uke ABL 对 stock/custom 都报 `Failed to load/authenticate boot image: Load Error`；必须用 `fastboot flash boot` + `flash vendor_boot` + `reboot`。
- **ABL 槽位**：UEFI 不标记成功，多次启动后 `slot-unbootable:a` 会变 yes；每次测试前 `fastboot set_active a`。
- **initrd 对齐**：ABL 的 `linux,initrd-end` 不是 512 对齐；`PublishInitrd` 需向下取整，不能拒绝。
- **`NO WINDOW`**：DarwinBoot 要 1 GiB 连续窗口；uke 最大连续块只有 326 MiB；`owned_memory_size` 必须降到 256 MiB，`ADD_LIMIT` 提到 2 GiB 以保留更大空闲块。
- **`LockCapsuleInterface` 是 UEFI 输出**：不要把它当成 XNU/SEP 挂点；绿条后 XNU 在等根设备。
- **`AppleSEPManager` 是误导**：预链接内核里有该 kext，但 sheng 同类画面最终是等根设备；不要先做 SEP 排除。
- **corecrypto**：源码目录不是 unixnu refer module，最终 tree.assemble 前要删掉；archive 在 `build/corecrypto-2023`。
- **`BOOT_PROGRESS`**：要求 UFS/owner/Virtio/source-corecrypto；PMGR-only 不能开。
- **`SOURCE_PTHREAD`**：需要 `libpthread_kernel.a`；PMGR-only 先关。
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

---

## 9. 参考

- UniXNU porting：https://unixnu.com/porting/
- Bring-up order：https://unixnu.com/porting/bring-up
- Boot arguments：https://unixnu.com/xnu/boot-arguments
- Debugging and logs：https://unixnu.com/guide/debugging
- sheng device page：https://unixnu.com/devices/sheng
- sheng-ipados 参考 commit：`blu-sharky/sheng-ipados@6530e65`（当前仓库不可访问）
- 本设备仓库：https://github.com/Allenwdk/UniXNU-uke
- CI：`.github/workflows/build-kernel.yml`
