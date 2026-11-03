# UniXNU on Xiaomi Pad 7 (uke) — 进度文档

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


> **Last updated:** 2026-10-25
>
> **当前一句话状态：** **内存墙已拆 + 画面首次上屏**（2026-10-24）。UEFI 静态映射 0x8C0000000-0xB0000000 高区 DRAM（FD `e3ffdcbc…`）配合 `owned_memory_size=0x130000000`，XNU VM 实测 ~4.5 GiB、t=538s+ 无 jetsam；反汇编原厂 SwapEnd 链后打通呈现：合成器把整帧渲进 overlay surface（记录 layer 6、ID 如 41，原厂 4 层循环翻译不到），`presentBase` 在 sel 5 返回后用原厂 IOSurfaceRoot lookup（`0xfffffff0057a28c0`）解析该 ID 并拷贝到 GOP —— `gpu-framepresent2`（内核 `ea95a0dd…`）**首次在面板上显示系统 UI**（白底 + home indicator + "swipe up to upgrade"）。v3（内核 `f2598af9…`、vendor_boot `a4128390…`、归档 `releases/2026-10-24-gpu-framepresent-v3/`）去掉空事务时回落纯黑默认 surface 的路径（v2 几秒后黑屏的原因），上机判定中。**2026-10-25 触摸轨：驱动管线真机全通**（时钟/引脚/GENI/固件下载/帧循环/接触/坐标/HID 报告/事件驱动，`drv1 rpt7004 r0`、34 分钟长跑稳定），唯一判定项是真实 UI 下持续手势；有 UI 时 ~1 分钟触摸死锁已定为 `NativeHidInput 588c96f` 在触摸线程调 `handleReport`，上游 `5f3a37c` 改为 workloop `runAction` 并已 pin，touch5b 判定中。详见 `docs/TOUCH-PORT.md`。其他里程碑不变：异步/GPU 呈现提帧率（当前 CPU 拷贝 150-300ms/帧）、防自动变暗。
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
| 7. 用户态 | 🟡 大幅推进 | launchd/`Early boot complete`/`driverkitd`/Data 卷/display client 全部跑通；**GPU 9 级 bring-up + kernel Metal publish 已在真机验证**（见 3.1.2）；compositor 已在提交 swap。卡点：Legacy display 的 swap 没有 surface（`surface=0`，需 own-present），以及 VM 只有 `owned_memory_size` 的 256/320 MiB 导致 jetsam OOM（需 UEFI 高区映射）。 |

---

## 3. 已完成的工作

### 3.1 仓库与板级配置

- 设备仓库：`Allenwdk/UniXNU-uke`；UEFI：`uefi-uke`；核心：`Allenwdk/UniXNU` 的 `uke` 分支（已同步 upstream `main`）。
- GPU 适配分支：设备仓库 `uke-gpu`、核心 `Allenwdk/UniXNU` 的 `uke-gpu`、`Allenwdk/NativeAdreno` 的 `uke-a732`（core `.gitmodules` 指向 fork）。CI 的 `ci.yml`/`build-kernel.yml` 在这条分支上 checkout 核心 `uke-gpu`。
- `board.toml`：
  - IPSW：iPad7,1 iPadOS 17.7.11 (21H461)，kernelcache pin `f18b4df...`。
  - `[userdata]`：`first_lba = 0x2d3000`、`last_lba = 0x3b147fa`、`lba_bytes = 4096`。
  - `[payload]`：`volume = "UKEESP"`、`size_mib = 80`、`dram_size = 0x80000000`、`panic_screen = false`。
  - `[payload.adt]`：`owned_memory_size = 0x10000000`（256 MiB）、`chip_id = 207`、`root_match_volume_uuid = 04769D26-2CA0-4825-B443-E2E5F88300F2`。
  - `[[bootimg.dtb_node]]`：splash `0xe3940000 + 0x2b00000`；`/chosen` initrd `0x1000000..0x6000000`（ABL 运行时会用实际地址覆盖）。
  - `[kernel] archives`：full 构建的 13 个 archive builder；只有 `CONFIG_XNU_BUILD_ARCHIVES=y` 的配置会执行。
  - `[gpu]`：Adreno 732 的 chip id、寄存器窗口、SID、GMEM/GMU 参数、A732 catalog 值（`gmu_chip_id=0x70b0000`、CGC `0x00020202`、preemption record `0x225000`）和固件 pins。
- `configs/`：
  - `uke_defconfig`：首控制台（corecrypto、ArmDTPlatform、ArmCpu、timer、PAN、panic region、legacy display）。
  - `uke-full_defconfig`：**可复现的 stage-6 full 配置**（corecrypto、pthread、UFS bring-up、AES recorded bindings、storage headers、mockfs、PMGR、legacy display）。
  - `uke-full-plus_defconfig`：sheng parity；保留 VirtIO/RamBlock、verified root、owner-root、BOOT_PROGRESS，等待 live audit/策略包。
  - `uke-full-minus-pmgr_defconfig`：只启用 PMGR + legacy display，用于验证 PMGR 不是 SEP 阻塞点。
  - `uke-gpu_defconfig`：stage-6 full + `CONFIG_XNU_NATIVE_ADRENO=y`，是 A732 的可构建配置；full/userland 仍默认关闭 GPU。
  - `bootstrap_defconfig`：bootstrap/compile records + prelink candidate；不执行 board archive list。
- 设备数据：UFS `0x1d84000`、compatible `qcom,ufshc`、stream ID `0x60`；触摸 NT36532；面板 O82 3200×2136；splash `0xe3940000`；GPU `0x43030b00` / `Adreno732`。

### 3.1.1 GPU 阶段 4：Adreno 732（无需真机部分）

- 分支：设备 `uke-gpu`、核心 `uke-gpu`、`NativeAdreno` fork `uke-a732`（commit `dae5c2f`）。
- 驱动：`NativeAdrenoProfile` 按 `qcom,chipid` 选择 A740/A732；A732 记录 1.5 MiB GMEM、GMU chip id `0x70b0000`、CGC `0x00020202`、2196 KiB preemption record、UBWC 4.x/16/3、`gen71100` 固件名和 `0x68000` GMU/GPU delta。未知 chip id 直接拒绝。
- 窗口：GPU 服务按 `reg-names` 选窗。uke 用 `kgsl_3d0_reg_memory`/`rscc`/`cx_misc`；sheng 用 `gpu`/`cx_mem` 和 GMU 节点的 `rscc`。GMU 五窗口按 `ShengGmu*` 顺序组装。
- 兼容串：GMU 接受 `qcom,gen7-gmu`，SMMU 接受 `qcom,kgsl-smmu-v2`。
- 计数器：GMU always-on counter 由实际映射基址推导；SM8550 `0x14220`，SM7675 `0x16220`。
- 头文件：`scripts/prepare-uke-gmu.py --derive` 从 live DTB + pinned mainline 生成 `scripts/data/uke-gpu-inputs.json`（DT 窗口/OPP/带宽/stream/command DB/ZAP + A732 catalog + 19 条 RSC/PDC 写入），build 模式生成 `ShengGmuSource.h`、`ShengGmuVotesSource.h`、`ShengGmuCommandDbSource.h`。固件字节不入 Git。
- 构建：`board.toml` archive list 增加 `prepare-uke-gmu.py`、`build-arm-driver.py NativeAdreno`、`build-native-adreno-kits.py`；后两者在 `NATIVE_ADRENO=0` 时跳过。`build-native-adreno-kits.py` 只从 `.cache/firmware/uke-gpu` 嵌入 gen71100 并核对 pins，缺文件时保留 weak 空定义，GMU 在运行时拒绝而不是链接失败。
- 探针：`GpuPowerProbe.cpp`/`GpuPowerBoard.h` 已换成 Cliffs 偏移（CX/GX GDSCR `0x908c`/`0x9034`、AHB `0x90c8`、CX_GMU `0x90e4`、RCGR `0x9168`/`0x919c`、DDRSS AXI `0x7115c`）、A732 ACTLR `0x32b`、secure SID `2`、ZAP `0x9899a000` 和 A732 device info；在 `CONFIG_XNU_NATIVE_ADRENO=y` 时编入 legacy display archive。
- 配置：`uke-gpu_defconfig` 可构建；full/userland 保持 GPU off。
- host 校验（Linux）：`check-uke-gpu.py`、`check-uke-gpu-host.py`、`check-gpu-power-host.py`，已接入 `ci.yml`。
- 仍需真机：SMMU bank/ASID/slot、GPU CX/GX rail/clock votes、AOP command DB 版本、GMU cold start、CP ring/SQE、ZAP 认证、scanout、NativeMetal/Turnip。详见 `docs/GPU-PORT.md`。



### 3.1.2 GPU 阶段 4：真机结果（2026-10-24）

**9 级探针**（`sheng-gpu-power=N`，`sheng-smp=0`，`vendor_boot_uke_gpu_*`）：

| Mode | 结果 | 关键证据 |
|---|---|---|
| 4 | ✅ GMU SMMU context Ready | `B s7 st0 k0/0 p5w13r1e1 r4w2v1`，SMR valid `R80000005` |
| 5 | ✅ 只读时钟 + command DB | `K p00000100 … g0000 h0000 b111013 v0`，`D v1 m0c0330db p0 s1 x0` |
| 6 | ✅ GMU 时钟 bring-up | `C s7 f0 l2 u2/1 pe0000005 o1 h505 g303`，post `l44440016` |
| 7 | ✅ GMU cold start + HFI | `G1/0/0/0>G0/1/0/1`，`P0 g8 kc6 e0 t0 n4 1ms`，`S0 ph7 h1 f100 H1 r11 … p18` |
| 8 | ✅ GX 上电 + HFI perf vote | `X v0/0 o3 x3>0 cf8222800 R1 H8`，心跳开始 |
| 9 | ✅ GPU context + CP ring + publish | `Q s7 st0 k0/0 p5e1 r4v1 n5`，`C10ok`，`S zap/irq`，`P published`，`M … k0 s5/0 r1` |

镜像/内核（按时间）：`vendor_boot_uke_gpu_m4b.img` `46409c67…`（kernel `b0fdff6f…`）→ `m7b` `b7751002…`（`c6f7bbfe…`）→ `m9l` `386f9c20…`（本地 `f39883cd…`）→ `m9g` `2e0fbbde…`（`d7fce63b…`）→ desktop 系列 `f5beb87e…`。

**本阶段修掉的坑**
1. GMU pool 的 `adopt` 映射必须用 `kIOMapWriteCombineCache` + 普通 store 清零（默认映射上 `bzero` 的 `DC ZVA` 对齐中止，`59bcc5a`）。
2. 共用 `poll()` 的 10 ms 窗口太短：对齐 Linux 的 100 ms，`poll_budget` 200→1000（`b5ae88d`）。
3. `ShengGmuBuildStartup` 的 `fail=5`：派生带宽表缺 level-0 的 `0`，补回 7 项（`68caad2`）。
4. GPU context 的 1027 页连续表池在 userland wired 后抢不到：预留池 `+0x1600000`（`0xC9600000`）起 8 MiB 给 GPU context，tables+buffers 全部 adopt；adopt 成功后必须跳过 VM 回退（`adopted_pool` 标志，`3ec59c2`/`f1a2aee`）。
5. GPU context 的第二条 secure SID 2 流在非安全 VM 里写 `SMR[1]` 会同步外部中止：改为 `sid1==0xffff` 只挂 user SID 0（`e000d06`）。

**本阶段跑通但仍有问题**
- **Desktop 尝试**（`sheng-gpu-power=9 sheng-gpu-metal=1 sheng-fb-gpu=1 offline-mobile-graphics-client=1 sheng-gpu-pool=0`）：系统活到 500+ 秒，compositor 持续 `sel 4/5` swap，`M` 行 `n`（name reads）增长；但 swap 被 `surface=0` 拒绝（原厂在 SwapEnd 内填 surface 并释放请求），屏幕仍只有启动日志。swap 记录 1200 字节：`+0x00/+0x08/+0x18/+0x20` 是 4 个每帧变化的大 32 位值（疑似 IOSurface ID），`+0xa0/+0x100` 出现 `0xc80/0x500`（3200×1280）尺寸。内核里 `IOSurfaceLookup` 不在 kernelcache 符号表，own-present 需要另找 surface 解析路径。
- **内存**：`UKE MEM` 显示 wire≈47.5k + active/inactive≈5k 页 ≈ 200 MB。XNU 的 `arm_vm_init` 用 `boot_args->memSize`（DarwinBoot window）作为 `gPhysSize`，即 `owned_memory_size`；ADT 的 `dram-size` 只描述物理 DRAM，不决定 VM。`owned_memory_size=0x180000000` 时 loader 直接 `Out of Resources`（`UkeBootMemoryDxe` `ADD_BELOW=SIZE_4GB`，高区不在固件 MMU/GCD）。低区最大连续 run ≈326 MiB，故 320 MiB（`0x14000000`）是当前上限。根治需要 uefi-uke 把高区 `0x980000000-0xB00000000` 映射进 GCD/MMU。
- **本地 Linux 工具链**：独立 worktree（core `linux-host` + device 5a110b2/`gpu-m7-local`）可完整编 `uke-gpu-userland_defconfig`；修了 `out/` 预建、`UNIXNU_HOST_COMPAT_INCLUDE`、`compression_tool` shim 三个环境缺口，增量重编约 3-4 分钟。

**分支/CI 恢复**：并行的 Linux 移植提交（device `5a110b2` 的 `patches/xnu/linux-*`）会破坏 macOS CI（hostcompat / Xcode 16.4 warning），已 revert（`24ab7fd`）并在 `linux-host`/`linux-werror` 保留；CI 回到 `macos-latest`；固件改走三个 40 KB secret chunk + path 对齐的 Actions cache（`f61f3c3`）。

### 3.1.3 内存墙拆除 + 画面上屏（2026-10-24 深夜）

**内存**：照 sheng 的方式在 `ukePkg/Library/MemoryMapLib` 静态声明高区 DRAM（`0x8C0000000+0xC0000000` 与 `0x980000000+0x180000000`，相邻，RunEnd 合并成 9 GiB run；uefi-uke `1e2d12d`），board.toml `owned_memory_size=0x130000000`/`dram_base=0x8C0000000`/`dram_size=0x240000000`（device `a562371`）。实机：`UKE MEM` ~111 万 4K 页（≈4.5 GiB），free ~645 MiB，t=538s+ 无 jetsam（320 MiB 窗口时代 ~500s 必死）。FD `e3ffdcbc…`，boot.img `f197abc8…`。

**原厂 SwapEnd 链反汇编**（合并内核 = payload 里的 `kernel.macho`，`__PRELINK_TEXT` @ `0xfffffff00427c000`、`__PLK_TEXT_EXEC` fileoff `0x190A000`；地址在这个构建里固定）：
- sel 5 的实现 `0x577722c` 先检查 `structureInputSize == 1288`，不等则 abort（vtable[217]）返回 `0xe00002bc`——swap_submit 根本不跑。GPU 合成器发 1200 字节记录（CPU 渲染器是 1288）。
- `swap_submit`（vtable[178]，`0x578f224`）：gate 动作按 `record+0x98` 的 ID 在 `fb+5496` 链表找 request；掩码从 record 拷贝（`+0x14c`→request+792 updated、`+0x150`|→796 disabled、`+0x154`→800）；**层循环只跑 layer<4**，surface ID 在 `record+0x9c+4*layer`，等于默认 surface ID 时直接用 `fb+0x1618`，否则 `fb+0x1600`（IOSurfaceRoot）的 lookup（`0x57a28c0(root, id, task)`，task 来自 `0x57a2e84`）→ 存 `request+0xa40+8*layer`。
- 实机记录：updated=`0x56`（layers 1,2,4,6）、disabled=`0xff000000`；layers 1/2/4 的 ID=0，**layer 6 的 ID=41 才是整帧**——原厂循环够不到，request 的 surface 槽全空 → `accepted=0 reason=2 surface=0`。

**呈现方案**（NativeMobileDisplay `33fe049` + device `ad87339`）：`wrappedExternal` 在 sel 5 前把 <1288 的记录补零到 1288（`offline-legacy-swap-pad`，默认开）、捕获记录里第一个有 ID 的 enabled 层；原始调用成功返回后 `presentBase(frameSurfaceId)` 用**原厂 lookup stub**（绑定 `0xfffffff0057a28c0`/`0xfffffff0057a2e84`，字节校验）解析 overlay surface，校验几何后同步拷贝到 GOP（合成器自己的线程上，surface 不会被并发释放）。`offline-legacy-noconsole=1` 防止控制台重绘盖掉画面。默认 surface（`fb+0x1618`）实测纯黑（`nonblack=0`）——v2 曾回落到它导致首帧后几秒黑屏，v3 起空事务不再呈现、保留上一帧。

**实机结果**：`gpu-framepresent2`（内核 `ea95a0dd…`）**首次在面板显示系统 UI**——白底、home indicator、"swipe up to upgrade"。v3（内核 `f2598af9…`，vendor_boot `a4128390…`，`releases/2026-10-24-gpu-framepresent-v3/`）修复空事务刷黑，判定中。当前帧率 3-7 fps（3200×2136 CPU 拷贝 150-300ms/帧）。

**流程教训**（并行会话踩坑）：共享 `UniXNU-linux/out` 的并行构建（USB 串口测试）会把 defconfig/软链/kernel.macho 换掉，产出错误内核的 payload——两版废镜像由此而来（设备上表现为无任何 UKE 行 + SpringBoard corpse 循环）。**规矩：本地镜像刷机前必须 `mcopy -i <vendor_boot>@@4096` 抽出内核，grep 标记字符串并核对尺寸（GPU 轨 52,133,888 B / USB 配置 51,478,528 B）；重建前先重跑 `defconfig` 复位软链；每轮镜像归档到 `uke-port/releases/<date>-<tag>/`。**



### 3.1.4 触摸驱动按上游 sheng 对齐 uke 板级（2026-10-24）

背景：`package/TouchProbe.cpp` 早已是 sheng 全量分阶段探针，但 `TouchBoard.h` 是 sheng（SM8550）板级常量的**逐字拷贝**，在 uke 上会摸错引脚/时钟。本次按上游架构重写板级层并把 sheng 后续实机修复回移。

**uke 硬件事实**（`uke-port/data/uke-live.dts` + 小米内核 `gcc-cliffs.c`/`pinctrl-cliffs.c`）：
- 触摸 = `qupv3_se4_spi: spi@a90000`（SE 0xa90000，wrapper `qcom,qupv3_0_geni_se@ac0000`/0x2000，apps SMMU sid 仍 0xa3，SMMU reg 0x15000000/0x100000）上的 NT36532，SPI mode 0、19.2 MHz——**地址与 sheng 相同，但这是 QUP wrap 0，不是 sheng 的 wrap 1**。
- GCC：五个 wrap0 分支同在 vote 0x52008、位 18/19/20/21/26（与 sheng wrap1 巧合相同）；halt 改为 m_ahb 0x23000 / s_ahb 0x23004 / core 0x23008 / core_2x 0x23018 / s4 0x183bc；GPLL0 vote **0x52020**（sheng 0x52018），且是 fixed Lucid OLE PLL（常开，无 mode 锁位可轮询）。
- **关键结构差**：`gcc_qupv3_wrap0_s4_clk_src`（cmd_rcgr **0x183c8**）是普通 freq_tbl RCG（`clk_rcg2_ops`），不是 sheng 的 DFS——探针必须自己编程 RCG。方案：直接编 freq_tbl 的 `F(19200000, P_BI_TCXO, 1, 0, 0)` 条目（CFG=src 0/hid 1/M/N/D 清零 + CMD_UPDATE 自清位轮询 + 回读解码校验 19.2 MHz），SE 源时钟即 TCXO 直通，零 GPLL0 依赖；`GENI_CLK_SEL` 写 0。parent_map_0 与 sheng 同（TCXO 0 / GPLL0 600M 1 / even 300M 6）。
- TLMM 基址 **0x0f100000**（= DT 节点 pinctrl@f000000 + pinctrl-cliffs.c REG_BASE 0x100000，**每 GPIO 块起点**；初版误写 0x0f000000，实机读未背线区域挂总线导致无 panic 复位，见下；ctl=+0x1000*gpio、mux_bit 2、drv_bit 6、gpio 功能槽 0）；SPI 引脚 **MISO 16 / MOSI 17 / CLK 18 / CS 19**（功能 `qup0_se4_l0..l3`，各自 pingroup 的槽 1）、驱动 6 mA；IRQ 引脚 **GPIO 54**（`novatek,irq-gpio`）。触摸矩阵 60×40（mp-criteria X 0x3c/Y 0x28），与 sheng NT36532E 相同，`NVT_SHORT_AXIS_MAX=20320` ⇒ 沿用 sheng 的 Y 镜像 20319，待实机判定。
- TLMM 摘要中断 SPI 208（`pinctrl@f000000 interrupts=<0 0xd0 4>`）⇒ INTID 240，与 sheng 相同；`armcpu_spi_attach` 在核心仓 `drivers/ArmCpu/ArmCpu.cpp` 已有定义。

**回移的上游实机修复**（sheng 仓在 uke 快照之后的演化）：≤50 µs 自旋 `ioDelayUs`（int-182：轮询 1 µs 的 IODelay 每次两个上下文切换，触摸线程曾是最忙内核线程）；**中断驱动帧循环**（int-189：GPIO54 上升沿 TLMM 引脚中断唤醒读帧，无人触摸时零唤醒；`sheng-touch-poll=1` 回到轮询诊断，attach 失败自动回退 10 ms 轮询）；`thread_set_thread_name("sheng-touch")`；`native_touch_set_sink` 改原子 store/load。

**验证**：`TouchBoardTests.c` 重写（TLMM 地址/引脚编码、RCG encode/decode、SMMU bypass fixture）本机 PASS；`NvtTouchCore`/`GeniSpi` 主机测试 PASS（本机 gcc 需 `-Wno-misleading-indentation` 模拟 CI clang）。**阶段 4（固件下载+帧处理）需要 `.cache/firmware/novatek/novatek_nt36532_o82_fw_csot.bin`**（实际来自设备 ODM 镜像 `odm/firmware/`，o82 双变体 csot/tm；早期默认名 n81a 不存在，已修正；缺文件时探针按设计停在下载前）。上机分阶段：`sheng-touch=1`（只读快照）→ `2`（投票+引脚+引擎 ID）→ `3`（RCG+GENI init+chip id）→ `4`（固件+帧循环）。

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

### 4.1a 阶段 4：Adreno 732 GPU

- **已完成（无需真机）**：A740/A732 profile、uke `reg-names` 窗口选择、`qcom,gen7-gmu`/`qcom,kgsl-smmu-v2` 匹配、SM7675 GMU 计数器偏移、`gen71100` 固件/OPP/BCM/command DB/RSC 头文件生成、`uke-gpu_defconfig`、Cliffs 电源探针偏移、Linux host 校验。分支见 3.1。
- **仍缺（必须真机）**：SMMU bank/ASID/slot 的实机分配、GPU CX/GX rail/clock votes、AOP command DB 版本字、GMU cold start（RSC/PDC、firmware load/readback、HFI）、CP ring/SQE、ZAP auth/secure mode、GPU scanout、NativeMetal/Turnip。
- 固件 `gen71100_*` 不在 Git；CI 无固件时链接 weak 空定义，GMU 在运行时拒绝。首次实机启动按 `sheng-gpu-power=1..9` 分级观察，不要直接开最高级。

### 4.2 参考实现

- `blu-sharky/sheng-ipados@6530e65` 当前 GitHub 返回 **Repository not found**（可能私有/改名），无法直接读取 diff。
- 如果拿到该 commit 的 patch/关键文件，优先按它补 `NativeUfs`/存储/root；否则按 sheng 设备页的 `sheng-pmgr=2` + `native-ufs-root=1` 路径实现。

### 4.3 其他未做

- `panic_screen=true` 已启用；`Font.h` 从 Mu-Silicium vendored 到 `scripts/Font.h`，Linux/CI 无 UEFI checkout 也能打补丁。panic 文本现在能在屏上稳定显示。
- `boot-progress=1/2` 仍需要 audited UFS/owner-root + VirtIO + source-corecrypto；当前 full 配置没有这些，所以保持关闭。
- `sheng-platform-stage`、`sheng-aks=1`、`amfi_get_out_of_my_way=1`、`cs_debug`、`kextlog` 等 boot args 尚未系统测试；它们不是当前 root 阻塞的优先项。
- GPU 已收集 A732 的真实 `gen71100_*` 固件和 9 档频率（`docs/GPU-PORT.md`）；驱动仍是 A740 常量，`CONFIG_XNU_NATIVE_ADRENO` 关闭。
- 触摸已完整移植并真机验证驱动管线（见 §3.1.4 与 `docs/TOUCH-PORT.md`）；Wi-Fi/蓝牙、USB、音频、传感器、电池、suspend/resume 均未做。
- `owned_memory_size` 目前 256 MiB；sheng 用 4.75 GiB。uke 的 DT reserved-memory 把最大连续块压到 326 MiB；如需更大窗口，要裁剪 reserved-memory，而不是只调大 `ADD_LIMIT`。
- `kernel` 与 `full` 两个 CI job 已在 run `39495271779` 同时绿；UFS 驱动改动后会触发新一轮 full 构建。

---

## 5. 下一步（按优先级）

1. **读 pram 定位 Apple logo 后的重启**：重启进 UniXNU 菜单拍 `last panic:` 行，或 Linux 读 ramoops 高半区（`0xC4200000` 附近，sig `0x43474244`）拿完整 msgbuf 尾部；判定 display 路径 panic 还是 watchdog 复位。
2. **显示收尾（slot242 / default surface）**：`s_map_display_type … display type is 0` 后 display client 已开、default surface 已收养（sid=1）；若 pram 指向 display，走 slot242/`s_default_fb_surface` 探针或 `surface_copy_port_name` 回退收尾，目标 WindowServer/SpringBoard。
3. **GMU mode 4 实机验证**：GMU 内存已回到上游形状（`GMU Pool` 0xC8000000 预留 + `withRange` adopt）；下轮 `sheng-gpu-power=4` 验证 SMMU context 能否建立（GPU-PORT.md 选项 A 的真机验证点，该地址此前只验证过 4 MiB 可写）。
4. **SpringBoard/设置界面**：text_fixups 已生效（`ext-fixup … applied`），启动约束注意 `AMFI: Launch Constraint Violation (not enforcing)` 是否转 enforcing。
5. **认证读取/owner-root**：OS/SystemOS DMG 已拉取并校验；生成 verified-root policy bundle，补 live storage ABI snapshot，再开 `VERIFIED_ROOT_PROBE`/`OWNER_ROOT_*`/`BOOT_PROGRESS`（`uke-full-plus_defconfig`）。
6. `panic_screen` 已启用；无 panic 文本的复位看 pram。每次测试后更新本文档（KernelSU 备份在 `uke-port/data/*_kernelsu.img`）。

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
| 2026-10-22 | 当前阻塞 | 系统 alive（`CommCenter`/`logd`/`coresymbolicationd`/APFS tx），但 `IOMFB: default_fb_surface: No Default FB Surface found` + `display type is 0`，屏幕停在 Apple logo；下一步 NativeMobileDisplay / Adreno 732 |
| 2026-10-22 | GPU 分支 | 新建设备 `uke-gpu`、核心 `uke-gpu`、`NativeAdreno` fork `uke-a732`（`57ae482`）；core `.gitmodules` 指向 fork |
| 2026-10-22 | A732 profile | `NativeAdrenoProfile` 记录 A732 0x43030b00 / 1.5 MiB / gmu_chipid 0x70b0000 / CGC 0x00020202 / preempt 0x225000 / delta 0x68000；未知 chip id 拒绝 |
| 2026-10-22 | uke 窗口 | GPU 服务按 `reg-names` 选 `kgsl_3d0_reg_memory`/`rscc`/`cx_misc`，GMU 五窗口按 ShengGmu 顺序；GMU/SMMU 兼容串接受 `qcom,gen7-gmu`/`qcom,kgsl-smmu-v2` |
| 2026-10-22 | GMU 输入 | `prepare-uke-gmu.py --derive` 从 live DTB + pinned mainline 生成 `uke-gpu-inputs.json`（10 GPU OPPs、6 带宽、19 RSC writes、command DB/ZAP）和三个头文件 |
| 2026-10-22 | GPU 构建 | `uke-gpu_defconfig`、archive list、固件 pin 嵌入/weak 回退、`GpuPowerProbe` 编入 legacy display（仅 NATIVE_ADRENO=1） |
| 2026-10-22 | host 校验 | `check-uke-gpu.py`、`check-uke-gpu-host.py`、`check-gpu-power-host.py` 通过；`ci.yml` 已接入；实机未启动 |
| 2026-10-22 | GPU 构建绿 | run `39685196474` 的 `kernel` + `full` 两 job 成功；A732 头文件、NativeAdreno（-Werror）、43 kit objects、staged AGXAccelerator、legacy display + `GpuPowerProbe.o` 全部生成；`kernel.macho` 52,416,512 B，sha256 `6883deac…`；CI 无固件，链接 weak 空镜像（预期） |
| 2026-10-22 | GPU userland 构建绿 | run `39718041346` 两 job 成功；`uke-gpu-userland_defconfig` 内核 52,465,664 B，sha256 `db984d4e…`；修复 owner-cache 补丁与探针的 `native_gpu_addon_*` 重复定义（探针改为 weak），探针行同步到 `UKE GPU1..4:` 内核日志；本地准备 m1/m3/m5/m6 vendor_boot 测试镜像 |
| 2026-10-22 | 实机 mode 1 | `gcc=ok cc=ok sm=skip gp=skip io=0 pw00`；GCC/GPUCC 可读，CX 未上电故 SMMU/GPU 窗不可读 |
| 2026-10-22 | 实机 mode 3 | `gcc=ok cc=ok sm=ok gp=skip io=0 pw11`；CX GDSC + SMMU vote 时钟已开，SMMU 回 `SM 00290406 4c017e18 40000016 v0 R00000000`（ID0 是 sheng MMU-500），无 fault —— 迄今最关键的一步 |
| 2026-10-22 | 实机 mode 4 失败 | 停在 GMU context step 1（`st=2bd` kIOReturnNoMemory）：`vm_page_find_contiguous(num=4092)` 与 `(num=1027)` 均找不到物理连续页（scanned 47503，wired ~34485） |
| 2026-10-22 | 分散 icache 无效 | fork `fbedbed` 的 icache `contiguous=false` 改动无效：XNU `IOBufferMemoryDescriptor::initWithPhysicalMask()` 只要 `physicalMask` 非零就强制走 `kmem_alloc_contig()`，标志位没有作用空间 |
| 2026-10-22 | sheng 连续内存机制 | sheng 不自己分配 GMU 载荷：`ShengGmuMemory` 只有 cpu/bytes/iova，物理地址由 `IODeviceMemory::withRange()` 直接取已存在的物理区间；sheng 的 DT 是完整真实树（`fetch-sheng-dts.py` 编译主线 DTS），带完整 `reserved-memory`，故可 `withRange` 到预留区 |
| 2026-10-22 | uke DT 是精简树 | `make-uke-adt.py` 手写 16 个节点，无 `reserved-memory`、无 GPU/GMU/SMMU 节点；DarwinBoot 只读 `kernel.macho.adt` 交给内核（`Args->DeviceTree`），完整 DTB（vendor_boot 1.8 MB、boot 475 KB，都含 kgsl/reserved-memory）都在设备上但没接进内核 —— 这也是驱动改用 `uke-gpu-inputs.json` 常量取窗口的原因 |
| 2026-10-22 | 分区方案对比 | sheng 把 payload 放 `userdata`（boot 只能装 BootShim+FD：窗口 78 MiB < 内核 92 MiB），不动 `vendor_boot`，内核直接吃真机 DTB；uke 反过来：payload 放 `vendor_boot` 的 vendor ramdisk（`patch-vendor-boot.py` 原样保留 stock DTB），`boot` 装 UEFI |
| 2026-10-22 | 文档 | `docs/GPU-PORT.md` 新增 "Device results so far"、mode 4 连续分配分析、DT/ADT 对比与 A/B/C 三条路线 |
| 2026-10-22 | sheng 对比分析 | 只读对比 `UniXNU-sheng` 与 uke 路线：结构同源，分歧在 bring-up 权宜（unverified 存储、6 个 xnu 补丁、pram 日志、旧版 display 基线）；vendor_boot 交付/UFS 窗口/内存图是设备强制，保留 |
| 2026-10-22 | board.toml 补齐 | 摘上游 3 提交（`1bcc447`/`e02d1c9`/`12ecfd1`）进 core：`native-text-fixup.patch` + `payload.py` text_fixups 管线；`make-uke-adt.py` 加 `--persistent-data`/`--text-fixup`；board 补 `trust_cache_sha256`、4 条 21H461 text_fixups、`persistent_data=true`、`native-ufs-root`/`allow-unverified`/`sheng-data-persist=2`/`sheng-lite`/`sheng-aks` |
| 2026-10-22 | display 重基线 | `LegacyDisplayProbe.cpp` 以 sheng 版为基线（tail-queue swap 修复 int-132、GPU hand-off presenter、client interpose），保留 uke 增量（default-surface 播种/收养、slot242 fallback、client-panic）；builder 同步合并 |
| 2026-10-22 | watchdog 完整版 | `BootWatchdogPolicy.c` 采用 sheng 全量（`lite_jobs` 作业隔离、`sheng_boot_setup_reset`、分块写入/原位重写），保留 uke 的 `com.apple.watchdog` 禁用键 |
| 2026-10-22 | GPU 内存上游形状 | revert fork `fbedbed`（scattered-icache 实为 no-op）；新增 `NativeAdrenoMemory::adopt`（`IODeviceMemory::withRange` 接管既有物理页，同 sheng GpuPowerProbe hand-off）+ `GmuAllocator::adopt`（预留池切分，IOVA 同 allocate）；预留区三处对齐：UEFI MemoryMapLib `GMU Pool` 0xC8000000/32 MiB（2–4 GiB 空洞）、ADT `gmu-memory` 节点、board.toml `[gpu]` `gmu_pool_*` → `ShengGmuSource.h` |
| 2026-10-22 | 提交推送 | 4 仓库推送：NativeAdreno fork `1f24cda`、core `a853953`（含 pin）、device 3 提交（`d1282e9`/`ee5d7f1`/`8d9a465`）、uefi-uke `9d08712`；补丁集重放 61 个全 PASS（含 `native-text-fixup.patch`） |
| 2026-10-22 | CI 内核 | run `39790766052`（`uke-gpu-userland_defconfig`）成功；full 内核 `kernel.macho` 52,486,144 B，sha256 `3195da38…`；符号验证 `NativeAdrenoMemory::adopt`/`GmuAllocator::adopt`/`sheng_boot_watchdog_policy`/`native_legacy_swap_stage`/`default_surface_*` 全在（注：`uke-kernel` 产物是无归档版，刷机用 `uke-kernel-full`） |
| 2026-10-22 | 实机轮 1 | panic `offline scratch Data mount failed: 2 @kern_exec.c:7488`（ENOENT）：打 payload 漏传 `--data-ramdisk`，payload 无 `scratch-data.img` —— 构建疏漏，非代码问题 |
| 2026-10-22 | 实机轮 2/3 | 带 214 GiB 种子重建后：`sheng bring-up watchdogd launch policy: 0`（watchdog policy 真机验证 ✓）、OwnerData 挂载成功（vol-uuid 匹配 ✓）、`UKE: display surface adopted … sid=1`（default-surface 收养 ✓）、`ext-fixup: BTAudioHALPlugin+0x38bec applied`（text_fixups 真机生效 ✓）、SystemOS/OS 卷 ready、`Early boot complete`、`driverkitd is active`、`s_map_display_type` display client 打开、**Apple logo 显示**、`apfs_trim_to_prev_fsize`（Data 可写）；shared_region 首启抖动后自愈 |
| 2026-10-22 | 当前卡点 | Apple logo 后无预警重启（尾部：`s_map_display_type` → `apfs_trim` ×3 → `ext-fixup BTAudioHALPlugin` → logo → 清屏 → 重启，无 panic 文本）；待读 pram（菜单 `last panic:` 或 ramoops 高半区 0xC4200000）定位是 display panic 还是 watchdog 复位 |
| 2026-10-23 | Gunyah vWDOG 定位 | stock vendor ramdisk 的 `gh_virt_wdt.ko` 用 `smc #0`：`x0=0x86000005,x1=2` 关闭返回 1（被拒），`x0=0x86000007,x1=0` pet 返回 0（成功）；EL3 的 `qcom_scm_sec_wdog_deactivate`（SVC1/CMD7）从 UEFI 走 SMC32/64 均返回 -2/-4，确认 secure watchdog 不是本机复位源 |
| 2026-10-23 | watchdog 复位解决 | UEFI `UkeBootMemoryDxe` 入口调用 Gunyah disable/pet，并把 bite time 设到 30000；XNU `BootProgress` 监视线程每 5 s pet `0x86000007`；实机不再出现 watchdog 复位，启动能停在 panic screen 而不是硬重启 |
| 2026-10-23 | 高内存 | DT 10 个 memory range：低 2 GB 实际只有 ~682 MB 可用；高区 `0x980000000-0xB00000000` 6 GiB 等合计 ~9.9 GiB 未使用。ADT 改为 `dram-base=0x980000000`、`dram-size=4 GiB`（`make-uke-adt.py --dram-base` + board `[payload] dram_base/dram_size` + core `payload.py` 透传）；XNU 接受非连续高 DRAM，`memorystatus_jetsam_thread: no victim!` OOM 消失 |
| 2026-10-23 | mobile graphics 隔离 | 关闭 `offline-mobile-graphics-client/load`：不复位，但 SpringBoard/liveactivitiesd 反复 `Too many corpses being created`；打开：走到 `s_map_display_type` → `request_power_change second_external 0 powerstate1 fcdisplaytype 0` 后无 panic 硬复位。下一阻塞是 A732 GPU 电源/GMU（mode 4），不是内存 |
| 2026-10-23 | 当前镜像/内核 | gunyah-pet 内核 `430e4d83…`（run `39829956845`，`uke-gpu-userland_defconfig`）；高内存 payload `vendor_boot_uke_himem4g.img` `21dbecaf…`（GPU pool 1024 MiB）；nomg `5d277d09…`；UEFI `e09219ff…`（Gunyah + DT memory range 打印） |
| 2026-10-23 | CPU 基线 | `main` 用 fork `NativeMobileDisplay` `274f1e4`（`setPowerState` 不触发原厂 `request_power_change`）+ QEMU 的 `LegacyDisplayProbe`/`ShengPlatformPower`/display ABI；`uke-cpu_defconfig` 关闭 `NATIVE_ADRENO`、`offline-mobile-graphics-client=0`；`BootWatchdogPolicy` 增加不依赖 boot-progress 的 Gunyah pet 线程 |
| 2026-10-23 | CPU display 收养补回 | QEMU 版 probe 整文件替换时丢了 default-surface 流程：重新加入 `+0x1780/+0x17c0` 播种（32 MiB 系统内存 region、BGRA/1 plane/4 bpp mode 块）和 `start()` 后把 `+0x1620` retain 写入 `+0x1618` 的收养，日志 `UKE: display surface …/adopted`；Gunyah pet 线程加启动日志与每分钟心跳（`pets=`），用于区分 watchdog 复位与其他复位 |
| 2026-10-23 | CPU display 构建 | CI run `39890292502` 绿色（`uke-cpu_defconfig`）；full 内核 `a3d1ab03…`（含 `display surface adopted`/`Gunyah watchdog pets` 字符串）；payload `e0c44d78…`（200 GiB 种子）；`vendor_boot_uke_cpuadopt.img` `3d5b8b3c…`（与 cpu200 相同 base、bootconfig 0）；实机到 `pets=12 last=0`（pet 线程存活、SMC 成功）后仍在 `s_map_display_type` + `kext-fixup BTAudioHALPlugin` 后复位；无 panic 文本 |
| 2026-10-23 | selector 抑制 + 诊断 | 把 GPU 分支验证过的 user-client selector 抑制搬到 `main`：selector 12（进 `request_power_change`）和 6（其后的 wait）直接返回成功，其余照旧转发；加 `UKEMEM: client selector N` 首调用日志、client 打开时打印 `+0x1618/+0x1620/sid`；Gunyah pet 改为每 1 s、每 10 s 打一次带 uptime 的心跳。CI run `39892387621` 绿色，内核 `535f21c1…`、payload `9abb3d5f…`、`vendor_boot_uke_cpusel.img` `778ddb46…`，已刷入待测 |
| 2026-10-23 | cpusel 实机 | default-surface 收养成功：`UKE: display client status=0 pid=56 surface=… temp=… sid=1`（两个 client：pid 56/88）；`s_map_display_type` 后 selector 8/15/8 返回，心跳到 `pets=80 t=96s` 后卡死重启、无 panic。判断为内核在 display 发现阶段卡住、pet 线程被一起冻结，Gunyah 30 s 后咬 |
| 2026-10-23 | 参考实现（OnePlus Ace 5 / ios-sweet） | IOMFB user-client selector 表：3 GetLayerDefaultSurface、4 SwapBegin、5 SwapEnd、6 SwapWait、7 GetID、8 GetDisplaySize、12 RequestPowerChange、15 SetDebugFlags、18 IsMainDisplay、28 GetDotPitch、72 notifications；backboardd 发现阶段调用 57/15/22/29/33/79/74/12/6/27/17/84/50。Ace 5 用自建 `AppleCLCD:IOMobileFramebuffer`+`SweetSurface`、10–30 Hz vsync 定时呈现；userdata 侧需要 SpringBoard `BKNoWatchdogs`/`FBLaunchWatchdogExceptions`，并内置 `IOWatchdog` stub 以免 watchdogd `_PanicOnCrash`（对应我们此前的 `watchdog namespace 2 subcode 0x5`） |
| 2026-10-23 | wdt5 诊断镜像 | Gunyah bite 改为 300000 ms（启动时打印 `watchdog bite=300000 status=`）；pet 线程不再打印，心跳移到独立线程（kprintf 卡住不会停 pet）；selector 0–95 前两次调用都打印 enter/exit。CI run `39894662028` 绿色，内核 `02fff104…`、payload `e1b25249…`、`vendor_boot_uke_cpuwdt5.img` `4095b182…` |
| 2026-10-23 | wdt5 实机 | `watchdog bite=300000 status=0`（Gunyah 已接受长 bite）；display 链路完整跑通：`sel 3` 返回 0（`default surface port status=0`）、`sel 4/5` 两轮 swap 成功、`sel 6/12` 被抑制，`sid=1`；57/22/29/33/79/74/27/84/50/55/60 返回 Unsupported/Error 属正常。三次启动：一次 `shared_region` 失败后快速重启（旧问题），两次到 `Early boot complete` 后内核日志完全停止（连 `pets=` 心跳也没有）再重启，无 panic |
| 2026-10-23 | 控制台静音 | 参照 ios-sweet `sweetfb_noconsole`：新增 `offline-legacy-noconsole=1`，首帧成功拷贝后设 `disableConsoleOutput=1`，停止向已被显示栈接管的 3200×2136 framebuffer 画字，msgbuf/pram 日志继续写入。CI run `39899959208` 绿色，内核 `2c59a9ba…`、payload `2b4b1f58…`、`vendor_boot_uke_cpumute.img` `ff9133fd…`；实机无 `screen console muted`，说明 CPU 拷贝路径根本没执行 |
| 2026-10-23 | 单 CPU 实机 | `sheng-smp=0`（payload `6be7c34a…`、`vendor_boot_uke_cpu1.img` `f42fc16c…`）系统稳定运行到 `t=877s` 未重启：SMP 是卡死/重启的触发条件；单 CPU 下系统持续启动 daemon、反复打开 display client（pid 500/501/520/535/615/639/649/677…）。t≈847 出现 `request_power_change second_external 0 powerstate1 fdisplaytype 0`：display client 数超过 interpose 上限 8 后，后来的 client 没被包装，selector 12 走回原厂实现 |
| 2026-10-23 | client 表扩容 + swap 追踪 | `kSeenClients` 8→128 并在表满时打印一次；新增 `UKE: swap N accepted/reason`、`UKE: transfer N status/bytes`、`UKE: retire N generation/presented` 首调用日志；`offline-legacy-noconsole` 默认关闭，保持屏幕日志可读。CI run `39903223181` |
| 2026-10-23 | 队列诊断 | 单 CPU + `swap-end` 打印：`status=0 current=0 head=0 state=c80/858/3/0/0/0/0/0/1/0` —— SwapEnd 成功但**请求没有进 framebuffer 队列**（+5472/+5480 全空），powered 位 `+5840`=1。因此 `validateSwap`/`kickSwapQueue`/拷贝都不可能有输入，问题在 user client 未提交，不在拷贝或电源状态 |
| 2026-10-23 | 12 归因修正 | 早期“selector 12 导致硬复位”只是时间相关性，未证实：抑制 6/12 后复位只是移到 selector 0；当前 SMP 复位中 12 一直是被抑制的；单 CPU 下 t≈847 的 `request_power_change` 出现后系统仍存活 30s+（真实原因是 interpose 上限 8 溢出）。保留 12 的抑制作为防御（原厂电源切换缺少平台驱动），但不再当作复位根因 |
| 2026-10-23 | selector 6 测试构建 | 只抑制 12、转发 6（SwapWait），并在 5/6 返回后 dump 队列与状态；驱动侧 pin `748d39f`（kickOnce 对未验证请求补 `validateSwap`，kick 入口打 stage 3）。device `495443e`、core `2330732`、CI run `39908310402` |
| 2026-10-23 | selector 6 实机 | SwapWait 立即返回 0（不阻塞、不提交），SwapEnd 后队列仍 `head=0`，多次 swap 后只有一次 `UKE: kick 1`（family 调用了 kick，但队列空无请求）；`state[2]` 随 SwapEnd 递增。结论：原厂 user client 完全没有把 swap 交给 framebuffer 队列，SwapWait 不是提交点；需要自己接管 selector 5 |
| 2026-10-23 | SwapEnd 结构诊断 | 在 selector 5 前打印 `structureInputSize`/delivery/前 0x120 字节，用于确定 iOS 17 的 swap 记录布局（surface ID/rect/mask），为 Ace 5 式 own-present 做准备。device `8b30cac`、CI run `39909862208` |
| 2026-10-23 | 原厂 swap 路径反汇编 | 在构建内核里定位到 IOMobileFramebufferUserClient dispatch 表（`0xfffffff005f354c0`）：sel 3/4/5/6/8 的 handler 与 check counts，SwapEnd 期望 structureInputSize=1288。SwapEnd → slot 178（`0xfffffff00578f224`）→ IOCommandGate action（`0xfffffff005790894`）按 `structInput+152` 的 ID 在 **framebuffer+5496** 的链表里查找请求（请求 +788=ID、+2960=next），命中后置 +1355 并调用请求对象 vtable[32]。我们的驱动读的是 +5480（另一条队列），所以永远找不到请求 |
| 2026-10-23 | active list 修复 | `kickOnce` 改为优先读 +5496（回退 +5480），dump 增加 `active=`；子模块 `8277751`、core `b65ccf2`、device `144838c`、CI run `39913325398` |
| 2026-10-23 | active list 实机 | SwapEnd 返回后 `active=0`，但**进入原厂前** `active=fffffff8e22f1000`：请求确实挂在 +5496。self-kick 触发 `UKE: kick`，但 `swap accepted=0 reason=2 surface=0` —— 请求对象 +2624 的 surface 在 SwapEnd 入口还是空，原厂要到 SwapEnd 内部才解析/填充。结构体 1288 字节、前 0x20 是 4 个 32 位大数值（疑似 surface ID/port），+0x98 是 ID，+0xb0/+0x118 是 3200×2136 两组矩形 |
| 2026-10-23 | 请求快照诊断 | device `42032c1`、CI run `39920278411`。实机：`req-before` vptr=`fffffff005f36f60`、vtable[4]=`OSObject::retain`、id=1、started=0、surf0=0；`req-after` 读已释放请求导致 data abort（`pc 0xfffffff007987afc`，翻译错误）。说明请求对象在 SwapEnd 内处理完即释放，surface 在入口仍未填充，无法事后接管 |
| 2026-10-23 | 参考 sheng（公开仓） | `UniXNU/UniXNU-sheng` 的 `OfflineLegacySwap.mm` 给出真实用户态序列：`IOMobileFramebufferSwapBegin` → `IOMobileFramebufferSwapSetLayer(display,layer,IOSurfaceRef,src,dst,0)` → `SwapEnd` → `SwapWaitWithTimeout`。sheng board 用 `sheng-fb-gpu=1`/`sheng-vsync=144`/`offline-mobile-graphics-client=1`，其合成走 GPU hand-off；builder 多绑 `_native_legacy_vsync_notify`（`0xfffffff005796720`）与 `_native_legacy_set_refresh_properties`（`0xfffffff00579ebb8`） |
| 2026-10-23 | QEMU 对照发现 | QEMU board **不设置** `offline-legacy-default-surface`，也不做 default-surface 收养，CPU 路径靠原厂 SwapEnd 自己处理；已移除会读已释放请求的 req-after 诊断；device `436c038`、CI run `39923727598` |
| 2026-10-23 | QEMU 对照实机 | 关闭收养 + `unit-scale=1`：display client 能打开但 `surface=0 temp=0 sid=0`，**完全没有 sel 4/5 swap**，随后 liveactivitiesd 反复 `Too many corpses`。结论：uke 上 default-surface 收养是 compositor 开始 swap 的前提，QEMU 的路径不能直接照搬；收养不是“无 surface”的干扰项 |
| 2026-10-23 | 渲染器结论（待确认） | 打开收养时 SwapEnd 有请求但 `surf0=0`，原厂在 SwapEnd 内处理并释放请求，Legacy 路径拿不到可接管的 surface；sheng 依赖 GPU hand-off（`sheng-fb-gpu=1`/`sheng-gpu-metal=1`），QEMU 的 GUI 依赖 QemuMetal/shared-window 加速器。iOS 17 的 render server 很可能需要 accelerator 才会产生真实 layer；纯 CPU 基线可能无法合成。建议回到 A732 GPU 阶段 4+ |
| 2026-10-23 | GPU 分支同步 | 设备 `uke-gpu` 合并 `main`（`9e16d0f`）：CPU display 基线/收养、`+5496` 队列诊断、128-client 表、Gunyah pet 线程全部带入；`LegacyDisplayProbe` 保留 GPU 分支的 `sheng_gpu_power_probe`/`sheng_cpufreq_probe`/`native_touch_probe` 调用。core `uke-gpu` 合并 `uke` 并 pin 显示 `8277751`（active swap list + on-demand validation）、payload `--persistent-data`；NativeAdreno fork 合并官方 `origin/main` 6 个提交（metal client、VM 4096 objects、import 生命周期、非相干 import、IFPC hold-off），A732 profile/ACES 保留 |
| 2026-10-23 | 实机 mode 4（第一次） | `vendor_boot_uke_gpu_m4.img`（`78b30275…`，kernel `eae8c77b…`，payload `b3edf46d…`，`sheng-gpu-power=4`，`sheng-smp=0`）刷入后 panic：`Unaligned kernel data abort`，pc `_bzero+0x44`（`dc zva`），`esr=0x96000061`，`far=0xfffffff879345040`，lr `NativeAdrenoMemory::adopt+0x1b0`。x21/x15=`0xc8000000`、x20=`0xffc000` 确认是 GMU pool 的 16 MiB icache。原因：`adopt` 用默认 `IODeviceMemory::map(kIOMapAnywhere)`，pool 在 XNU DRAM 之外、映射不可缓存，`bzero` 的 `dc zva` 在非 cacheable 映射上触发对齐异常 |
| 2026-10-23 | startup 修复实机确认 | `vendor_boot_uke_gpu_m6f.img`（`cbd8340a…`，kernel `0c80fde0…`，payload `c4d64dd0…`）刷入：`D v1 m0c0330db p0 s0 x0 G1/0/0/0` —— `s0`=`AdrenoHfiStartupSuccess`，不再打印 `UKE GMUDB: fail=…`；`C s7 f0 l2 u2/1 pe0000005 o1 h505 g303` 与 GMU context `B s7 st0 …` 保持不变。mode 7 冷启动现在只差 `gen71100_*` 固件嵌入（CI secret `UKE_GPU_FIRMWARE_B64` 仍为空） |
| 2026-10-23 | mode 6 诊断 + s1 定位 | `vendor_boot_uke_gpu_m6d.img`（`39996687…`，kernel `bcd6a88a…`）在 D 行加了 `F<n>` 并在控制台打印 `UKE GMUDB: fail=… gfx/cx/mx/sh0/mc0/acv=… cn0=…`：实机 `fail=5`（`AdrenoHfiBuildBandwidth`）、`chip=43030b00 gmxc=0 gfx=32 cx=32 mx=32 sh0=8 mc0=8 acv=8 cn0=50030 acd=0 ifpc=0`。根因：派生脚本把 0 kbps 的 level 0 过滤掉，而 `AdrenoHfiBuildBandwidth` 要求 `kbps[0]==0` 且严格递增；OPP 表却补了 0 项。修复：`prepare-uke-gmu.py` 带宽表前置 0（7 项）、`uke-gpu-inputs.json` 记录 0、host check 期望 7；CI `39954299547` 重建 |
| 2026-10-23 | m9g 复核 | `vendor_boot_uke_gpu_m9g.img`（`2e0fbbde…`，kernel `d7fce63b…`）刷入复核：`gpu pool tables 0 at c9600000`（固定 22 MiB 偏移）、`Q s7 st0 … n5`、`C10ok`、`M s5/0 r1`、心跳 `F0 G0 q4`，无复位。GPU 9 级 bring-up + kernel Metal publish 两次启动一致稳定。下一步：userland Metal plugin（NativeMetal bundle + addon/trust cache）与 scanout |
| 2026-10-23 | **m9f：adopt 成功 + Metal publish** | `vendor_boot_uke_gpu_m9f.img`（`0871f29a…`，kernel `48485e01…`）刷入：`gpu pool tables 0 at c9800000`（无 `alloc failed`），`Q s7 st0 k0/0 p5e1 r4v1 n5`、`C6sent`/`C10ok`、`S zap`/`S irq`、`P start`/`P published`。`M a0.ffffffff/0.ffffffff k0 s5/0 r1 o0/0/0 n0 d` —— **kernel 侧 Metal accelerator publish 成功**（stage 5、status 0、已注册；`k0`=IOGPUFamily 可用；`a0.ffffffff`=addon 未挂载、`K0`=插件无进度标记、`o0/0/0`=还没有 userland open）。心跳稳定 `r3b/3b F0 G0 q3`。另把 GPU 池基址改为固定 22 MiB 偏移（`+0x1600000`），彻底避开 GMU 的 ~21.1 MiB |
| 2026-10-23 | adopt Busy 根因 | 诊断镜像 `vendor_boot_uke_gpu_m9e.img`（`53acfaa7…`）显示 `gpu pool tables 0 at c9800000`、无 buffer 失败，随后 `gpu context alloc failed e00002d5`：adopt 成功后 `status` 仍是 0，代码**无条件**又跑了一次 `tables_.allocate`，对象已 backed → `kIOReturnBusy`。修复：加 `adopted_pool` 标志，成功 adopt 后跳过 VM 回退（NativeAdreno `f1a2aee`，core pin 更新）；本地重编中 |
| 2026-10-23 | GPU context 分配修复 | m9metal 的一次启动在 `Q step 1` 直接 `st=0xe00002bd`（kIOReturnNoMemory）：GPU context 的 1027 页连续表池在 userland wired ~38k 页后抢不到（上一次靠 zone_gc 侥幸成功）。修复：预留池尾部 8 MiB 划给 GPU context（`SHENG_GPU_POOL_BASE = GMU pool + pool_bytes - 0x800000`，GMU 只用 ~21.1 MiB / 32 MiB），`NativeAdrenoGpuContext` 的 tables+5 个 buffers 改为 `NativeAdrenoMemory::adopt`（`3ec59c2`，失败时释放并回退 VM 分配）；`prepare-uke-gmu.py` 生成宏（device `f9ab613`），core pin `3570cd4`。本地增量重编中，待刷 |
| 2026-10-23 | **实机 mode 9 通过**（单流实验） | `vendor_boot_uke_gpu_m9l.img`（`386f9c20…`，本地内核 `f39883cd…`，GPU context 只挂 user SID 0）刷入：`Q s7 st0 k0/0 p5e1 r4v1 n5`（GPU SMMU context Ready、5 spans），`C6sent` → `C10ok`（CP init 完成，RBBM=1、interrupts=0x100000、CP rptr/wptr `1c/1c`），`S zap`、`S irq`、`P start`、`P published (rounds skipped)`；心跳 `H0 R1 I100000 F0 r25/25 … G0 q0`、`H2/H4 R1 I0 F0 r3b/3b … G0 q3 u0`——CP 环排空、无 CP fault、无 SMMU 全局 fault、3 个 GPU 中断已处理，系统稳定。**9 级探针全部走完**（GMU→HFI→GX→GPU SMMU→SQE/CP→ring→zap/irq→publish）。遗留：secure SID 2 流仍跳过；GPU context 的 1027 页连续分配失败后回退；`X v8/0`（本轮 perf vote 返回 8）待查；下一步是 Metal/accelerator 和 scanout |
| 2026-10-23 | 实机 mode 9（第一次） | `vendor_boot_uke_gpu_m9.img`（`e8987c8c…`，同内核）刷入：GMU 冷启动/HFI/`X` vote 全正常，GPU context `Q step 1..4` 后内核 data abort（`esr=0x96000010` 同步外部中止，pc `ArmSmmu4K` 的 `write32`、lr `ArmSmmu4KAttachStreams`，`far` 偏移 `0x804`=SMR[1]）。序列是 S2CR[1] 写成功、SMR[1]（secure SID 2）写中止；同轮 `Q step 1` 里 GPU context 的 1027 页连续分配失败（`vm_page_find_contiguous`，zone_gc 两次后继续）。实验修复：GPU context 支持单流（`sid1==0xffff` 只挂 user SID 0），NativeAdreno `e000d06`、core pin `a1aec39`、device `9f02876`；本地与 CI 重建 |
| 2026-10-23 | **实机 mode 8 通过** | `vendor_boot_uke_gpu_m8.img`（`6ac73077…`，同内核 `c6f7bbfe…`，`sheng-gpu-power=8`）刷入：`X v0/0 o3 x3>0 cf8222800 R00000001 H8` —— HFI perf vote 成功（status 0/fw error 0、3 次 ack 轮询），GX 状态从 3（power+clock off）变 0（GX on），GX GDSCR `0xf8222800`，RBBM_STATUS=1，GBIF_HALT=8；随后心跳 `H0/2/4…` 每 2 s 递增，`G0`、`sdead`（GPU 上下文 FSR 尚无上下文的哨兵）、`g1`（GMU2HOST 信息位）、`r0/0`（CP 环未建）。系统稳定，Gunyah pets 正常 |
| 2026-10-23 | **实机 mode 7 通过** | `vendor_boot_uke_gpu_m7b.img`（`b7751002…`，kernel `c6f7bbfe…`，1200 ms 窗口版本）刷入：`S running`→`GPU m7 G1/0/0/0>G0/1/0/1`（SYSRESET 释放）、`P0 g8 kc6 e0 t0 n4 1ms`（HFI 8 个 stage、ack 掩码 0xc6、4 条命令）、`S0 ph7 h1 f100 H1 r11 a0 c1 v0 p18`（phase 7、firmware hash 通过、FW_INIT_RESULT=0x100、HFI ready、18 次轮询≈1.8 ms；`c1` 是 GMU2HOST 中断信息 bit0，不在 fatal 掩码里，`v0` 是成功路径不重验复位，均正常）。**GMU 冷启动 + HFI 握手完成**；下一步 mode 8（GX 上电 + HFI perf vote），镜像用同一内核换 `sheng-gpu-power=8` 即可 |
| 2026-10-23 | 本地 Linux 构建成功 | 独立 worktree（core `linux-host` + device `5a110b2`）用本地工具链编出 `uke-gpu-userland_defconfig` 内核：`UniXNU-linux/out/uke/kernel/kernel.macho` 52.1 MB、sha256 `da44ce44…`，三个 `gen71100_*` 已嵌入。过程中修了三个本地环境缺口：cold 脚本需预建 `out/`；必须显式 `export UNIXNU_HOST_COMPAT_INCLUDE=$HOME/.local/xnu-linux/include/compat`（否则 firehose 缺 `TargetConditionals.h`）；`~/.local/xnu-linux/bin/compression_tool` 第 19 行少一个引号（用 `xnu-linux-research/toolchain/bin` 的正确版本覆盖）。随后在 device worktree 建 `gpu-m7-local` cherry-pick `b5ae88d` 做增量重编：kernel `bdf69b58…`（含 poll 100 ms 与 `p<poll_reads>`、固件已嵌入），payload `39bef827…`，`vendor_boot_uke_gpu_m7local.img` `a9bd7f0c…`，用于本地迭代 mode 7 |
| 2026-10-23 | mode 7 重试镜像 + 本地 Linux 构建 | `b5ae88d`（poll 窗口 100 ms、budget 1000、S 行加 `p<poll_reads>`）CI `39969384681` 绿色：kernel `c6f7bbfe…`（三个固件 blob 已嵌入），payload `534bd853…`，`vendor_boot_uke_gpu_m7b.img` `b7751002…`，待刷。并行尝试本地 Linux 工具链（worktree `UniXNU-linux`/`UniXNU-uke-linux`，core `linux-host` + device `5a110b2`）：cold 脚本要求预建 `out/`（否则 `find out` 在 pipefail 下静默退出）、`build-xnu-4k-baseline.sh` 需要显式 `UNIXNU_HOST_COMPAT_INCLUDE=$HOME/.local/xnu-linux/include/compat`（env 脚本未导出，导致 firehose 的 `TargetConditionals.h` 缺失）；补上后进入 XNU bootstrap，进行中 |
| 2026-10-23 | 实机 mode 7（第一次） | `vendor_boot_uke_gpu_m7.img`（`0416d0d8…`，kernel `517df52d…`，payload `2c16fafa…`，`sheng-gpu-power=7`）刷入：GMU context/时钟仍正常（`B s7 st0`、`C s7 f0 … h505 g303`、`D … p0 s0`），冷启动到 `S running`，固件 plan/载入/逐字回读通过（`ph5 h1`），但 phase 5 的 `FW_INIT_RESULT` 轮询在 10 ms 窗口内始终为 0 → `S6`（ShengGmuTimeout），HFI 未尝试（`P1`），复位重新拉起（`v1`）；`G1/0/0/0>G1/1/0/1`。Linux `a6xx_gmu_start` 对 FW_INIT 的轮询是 100 ms，而共用 `poll()` 固定 10 ms。修复：`poll()` 窗口改 100 ms、`poll_budget` 200→1000、S 行加 `p<poll_reads>`（`b5ae88d`，CI `39969384681`），待刷 |
| 2026-10-23 | 分支恢复 + 固件入核 | 并行 Linux 移植提交给 macOS CI 带来两处破坏：`patches/xnu/linux-*.patch` 在 macOS 上也生效（hostcompat 头缺失 + XNU 源补丁不兼容 Apple clang），以及 `macos-15` 的 Xcode 16.4 不认识 `-Wno-unnecessary-virtual-specifier`。处理：device `uke-gpu` revert `5a110b2`（`24ab7fd`，Linux 补丁与 LINUX-BUILD.md 移除，历史可查）、`runs-on` 回到 `macos-latest`（`000a433`）；core `uke-gpu` 回到 `ab27f41`，Linux 工作保存在 `mine/linux-host`；NativeAdreno 的 `[[maybe_unused]]` 保存在 `fork/linux-werror`。另修 seed workflow 的 cache path（Actions cache version 哈希含 path，根级 save 永远匹配不上 build 的 `UniXNU-uke/.cache/...`，`f61f3c3`）。CI `39966654280` 绿色：kernel `517df52d…`，三个 `gen71100_*` 已嵌入（kit log PASS）；payload `2c16fafa…`，`vendor_boot_uke_gpu_m7.img` `0416d0d8…`（`sheng-gpu-power=7`，待刷） |
| 2026-10-23 | 实机 mode 6 通过 | `vendor_boot_uke_gpu_m6.img`（`a51a6a7b…`，payload `b79a59b7…`，同内核）刷入后正常到 launchd。时钟快照（写前）`K p00000100 o0 l00000000 g0000 h0000 b1111013 v0`；bring-up 结果 `C s7 f0 l2 u2/1 pe0000005 o1 h505 g303`（step 7、无失败、PLL1 锁 2 次轮询、hub/gmu update 2/1、PLL1 mode `0xe0000005`/OPMODE 1、hub CFG `505`=GPLL0/3、gmu CFG `303`=PLL1/2）；写后重读 `K pe0000005 o1 l44440016 g303 h505 b1111013 v3`（L 值有效、GPLL0 vote 3）；`G1/0/0/0`（SYSRESET 仍 1，等 mode 7），command DB 仍 `p0 s1 x0` |
| 2026-10-23 | 实机 mode 5 通过（只读） | `vendor_boot_uke_gpu_m5.img`（`10ff5cd8…`，payload `84e6d108…`，同内核 `b0fdff6f…`，`sheng-gpu-power=5`）刷入正常到 launchd/OwnerData。GMU context 仍 Ready（`B s7 st0 k0/0 p5w13r1e1 r4w2v1`），SMR 带 valid 位 `R80000005`；GMU 时钟快照 `K p00000100 o0 l00000000 g0000 h0000 b111013 v0`：PLL1 关、GMU/HUB root 关、branch cx_gmu/cxo/hub_cx_int/axi on、demet off、memnoc=3、GPLL0 vote 0；GMU 状态 `G1/0/0/0`（SYSRESET=1，TCM/RSC/HFI 0）；command DB `D v1 m0c0330db p0 s1 x0`：版本 1、magic `0x0c0330db`、解析成功，但 `ShengGmuBuildStartup` 返回 `AdrenoHfiStartupInvalid`（s1），mode 7 之前要查 DB 条目/chip id 匹配；`x0` 表示 GMx 不存在。下一步 mode 6（GMU 时钟 bring-up） |
| 2026-10-23 | 实机 mode 4 通过 | `vendor_boot_uke_gpu_m4b.img`（`46409c67…`，kernel `b0fdff6f…`，payload `e04250fc…`，`sheng-gpu-power=4`，`sheng-smp=0`）刷入成功：`GMUCTX step 1..7 running` → `B s7 st0 k0/0 i5511 p5w13r1e1 r4w2v1`（Ready，status 0；program phase 5/13 writes/readback+enabled；route phase 4/2 writes/verified），`T00010000c900f000 S00000167 C00000001 R00000005`；板级行 `gcc=ok cc=ok sm=ok gp=skip io=0 pw11`、`ahb=80000003>80000003 mem=80000003 dvm=82000001`、`cx=0022f001>f822f000 gx=0022f001 sv=00000001`、`SM 00290406 4c017e18 40000016 v0 R00000000`。系统同时跑到 launchd/OwnerData 挂载、Gunyah pets 正常（t=36s）。**阶段 4 的 GMU SMMU context 门已过**；下一步 mode 5（只读 GMU 时钟 + AOP command DB） |
| 2026-10-23 | mode 4 修复 | NativeAdreno `59bcc5a`：`adopt` 改为 `kIOMapAnywhere|kIOMapWriteCombineCache`（与 sheng `GpuPowerProbe` 的 payload hand-off 一致），清零改为普通 8 字节 store 循环，跳过按 VA 的 cache 维护（`MaintainPhysicalPages` 本就跳过 DRAM 外页面）；core pin `ab27f41`。kernel `b0fdff6f…`，payload `e04250fc…`，`vendor_boot_uke_gpu_m4b.img` `46409c67…`，待刷 |
| 2026-10-24 | 触摸驱动板级对齐 | `TouchBoard.h` 从 sheng 常量重写为 uke（Cliffs）实测值（见 §3.1.4）：wrap0（非 wrap1）、RCG 0x183c8 直接编程 TCXO 19.2 MHz（非 DFS 读表）、TLMM 0x0f000000、SPI 引脚 16-19、IRQ 54、GPLL0 vote 0x52020 不做锁位门控；`TouchProbe.cpp` 回移 sheng 实机修复（自旋 ioDelayUs、GPIO54+TLMM INTID 240 中断驱动帧循环、线程名、原子 sink），轮询回退保留；主机测试全过。阶段 4 需导出 `.cache/firmware/novatek/novatek_nt36532_n81a_fw_csot.bin` |
| 2026-10-25 | 触摸 TLMM 基址修正 | 实机首次运行触摸代码即复位（CI/本地内核同现象，卡在 `IOSurfaceRoot::installMemoryRegions`）：`pinctrl-cliffs.c` 的 `REG_BASE=0x100000` 意味着每 GPIO 块起点是 **0x0f100000**，误用 DT 节点基址 0x0f000000 读到未背线区域挂总线 → 无 panic 复位。修正后阶段 1-3 通过（`clocks+pins ok`/`geni+rcg ok 19200000Hz`/`identify ok`）。device `6f203a5` |
| 2026-10-25 | 触摸固件 + 阶段 4 闭环 | ODM 镜像 `new-port/out/dna/odm/firmware` 拿到 `novatek_nt36532_o82_fw_{csot,tm}.bin`（245,760 B；16 分区 ILM 95447 + DLM 82079，协议模型全过）；默认名 n81a→o82（`8726186`）。本地内核嵌入固件（`4da01e82…`，+245,760 B 精确吻合）后实机 `firmware downloaded crc ok` → `scanning v11 pid5993 60x40` → 帧流 ~58fps、`c1/c2`、坐标跟手（`3017,11373`）。无 UI 长跑 34 分钟+ 稳定：`f119123 ok119123 c1 hid7004 drv1 rpt7004 r0 edge1 99/6`。`2026-10-25-touch4/` 归档 |
| 2026-10-25 | 触摸 UI 死锁根因 | 开画面呈现后触摸在 UI 出现 ~1 分钟死、系统活：`NativeHidInput 588c96f` 在触摸线程直接 `handleReport`，SpringBoard HID user client 挂上后 workloop 上下文死锁。上游 `5f3a37c` 改 `inputLoop->runAction()`（+多指集合分发/按 ID 抬指/非手掌判定/按键）；core `uke-gpu` pin 升级（`48b45aa`），TouchProbe 补按钮弱桩（`21be464`），touch5b `bfde5e22…` 已刷判定中。文档 `docs/TOUCH-PORT.md` |
| 2026-10-26 | 日志第二条通路：userdata + recovery 读取（从 `uke-usb-warm` 并入） | `c2f0582`（cherry-pick 自 `uke-usb-warm @ 068902f`）：`UkeLastLog.c` 在原有 pram 两半区之外，每 2 s 把 msgbuf 尾部写成 16 KiB 记录追加到 userdata 最后 32 MiB（`UKELLOG1` + seq u64 + len u32 + crc32 u32 + 尾部），环形约保留最近 68 分钟；写入走 `native_ufs_log_open/write/sync`，几何未就绪（`off`/`no root layout`）时逐 tick 重试，遇到不会变的理由则停。pram 侧的 FUNK 与 ramoops 半区代码未改动，只去掉了 `uke_last_log_start` 里的 `gPanicBase` 门（两个写函数各自自守）。**recovery 读取**（`uke-usb-warm` 上 warm21 起每版实机验证过；本分支用其转储 `ukelog-warm36.bin`/`ukelog-warm21.bin` 验证解码器，211/43 条全部 magic+CRC 通过）：`fastboot flash boot_a uke-images/boot.img` + `fastboot flash vendor_boot_a uke-images/vendor_boot.img`（原厂镜像，否则 TWRP 起不来）→ `fastboot reboot recovery` → `adb shell "dd if=/dev/block/sda32 bs=4096 skip=58980347 count=8192 of=/tmp/ukelog.bin"` → `adb pull` → `python3 tools/decode-ukelog.py ukelog.bin -o boot.log`。pram 仍受“复位后丢失、设备外不可读”限制，userdata 这条是断电后仍可取的部分 |
| 2026-10-27 | vnode 耗尽根因与修复 | 系统能进桌面但“开始使用”完全无反应：uke 的 XNU 用 `MASTER.arm64.bcm2837`（`CONFIG_VNODES=1024`），ADT 没有 `/defaults/kern.maxvnodes`，`desiredvnodes` 被公式分支钳到 1024，vnode jetsam 阈值 ≈1948；iPadOS 文件负载在 1246 s 顶满，之后半小时 0 free、memorystatus 连续杀进程。`make-uke-adt.py` 增加 `/defaults/kern.maxvnodes=0x13400`（4.75 GiB 窗口的公式值）；8 核 25 分钟长跑 0 jetsam、free ~630 MB。`UkeLastLog` 在 120 s 打印一次 desired/sane 便于核对 |
| 2026-10-27 | 设置闪退根因与图标服务规避 | 点设置黑屏约 20 s 后闪退：`Preferences` bug_type 509 为 `scene-create watchdog`（system 10.98 s vs app 0.267 s）。根因是 `iconservicesagent` 在 CoreImage GL 空函数表上 `EXC_BAD_ACCESS(0x0)` 崩溃循环；上游 sheng 的 `sheng-disable-jobs` 早已包含 `com.apple.iconservices.iconservicesagent`。把两个候选 label 加入内核 `lite_jobs`（**正确 label 是带 `.iconservices.` 的那个**；不带的那次无效），图标服务不再启动，设置及其它页面可进（图标可能缺失） |
| 2026-10-27 | About 卡死是 scene-update watchdog | 设置→通用→关于本机：设置 App 冻结、系统不重启。持久化报告为 `scene-update watchdog`（10 s，Background；system CPU 14.66 s，app 0.014 s），线程栈全在 `mach_msg`/CFRunLoop（主线程、CA render、HID、worker 都空闲等待）→ 同步内核/XPC 等待。疑似 APFS 容量查询（200 GiB 稀疏 Data）或被禁服务的同步 XPC；下一步给 Data 挂载点做带计时的 `vfs_statfs` 或继续禁用服务验证 |
| 2026-10-27 | 上划/转场 backboardd SIGBUS | 上划回桌面时 backboardd+SpringBoard 重启（内核不死）：`EXC_BAD_ACCESS(SIGBUS)` 位于 CoreAnimation 26.1 MiB 区域末尾 +0x40，`ktriageinfo` 为 `mach_vm_allocate_kernel` NO_SPACE。`UKEVMA` 诊断（`zzzzzzzzzzz-uke-vm-alloc-log.patch`）显示 441 次失败全是用户 task 的 4–320 KB 固定地址分配，**没有 26 MB 或贴近故障地址的**，属无关噪声。surface 字段实测：height 2136、stride 0x3200、BGRA、allocation 0x1A13000；CA 区域 0x1A22000 = allocation+0xF000，故障在区域外 64 B → 导出映射缺 guard。已试 guard padding（`+0x90 += 0x1000`，surf2）与帧缓冲 WC 映射（`kIOMapWriteCombineCache` → `VM_WIMG_WCOMB`，surf1/wc1）；About 不受影响，上划效果待复测 |
| 2026-10-27 | CrashLogProbe 与构建坑 | 新 `package/CrashLogProbe.c`（启动参数 `sheng-crashlog=1`，由 UkeLastLog 的 startup 拉起）：首扫列 CrashReporter、`/Applications`、stat Preferences、枚举 Data 容器；按前缀分时打印 backboardd/SpringBoard/最新 Preferences 报告（8 KiB 头 + 完整报告窗口/符号）到 msgbuf→userdata。构建：**archives 阶段先于 XNU prepare**，fresh clone 必须先把 `EXTERNAL_HEADERS/TrustCache` 放好（`zzzzzzzzzzz-uke-trustcache-headers.patch`）；内核命令行上限 1000 B（disable-jobs 因此走内核 `lite_jobs`）。恢复读取见 `docs/RECOVERY-LOG.md` |
| 2026-10-27 | 用户态 Metal addon 管线（未真机） | surf5 payload 只开了 `sheng-gpu-metal=1`，缺 `sheng-gpu-addon=1`，心跳 `M a0.ffffffff/…` 表示 addon 从未尝试。内核已有 `zzy-native-gpu-addon-mount`；审计发现 `zzw-native-adreno-sandbox-grant` 只认服务属性 `NativeSandboxGrantProcess`，而驱动只在 client 上设 `NativeSandboxGrant`（服务属性无人设置），新增设备补丁 `zzzzzzzzzzzz-uke-gpu-sandbox-client-grant` 同时认 client grant 与加速器的 `NativeSandboxGrantService`（0x4147/0x4148），板级发布 `NativeSandboxGrantProcess=backboardd`/`NativeSandboxGrantService`；本次在 Linux 上完成插件构建（iOS 17.0 SDK、`ld64.lld` ld 包装、Turnip、metal2vulkan/Rust 1.98、ldid 4 KiB 签名；CDHash 061a4eb5/6f410266/cea5bf12），把 bundle、publish/clients、`adrenoprobed` 打成 zlib blob 由 `zzzzzzzzzzzz-uke-gpu-addon-blob.patch` 在 Data 挂载后首次 exec 写入 `/private/var/ngpu`（64 KiB 分块、fsync、cookie 标记），owner trust `b8aa3335…` 经 `--owner-trust-cache` 进入 payload；payload 80 MiB（addon5：kernel `cc1a290f…`、stable `0ee77654…`/vendor_boot `5508b2f6…`，CA 实验 `234d213b…`/`9a3f2809…`，noicon `eba32cf4…`/`2d95da8a…`，fbgpu `7a2f4ae0…`/`985806e9…`，探针 `addon5p`（无 .publish，kernel `e272bf4a…`）`b2c4026a…`/`c3ee801d…`；addon1–4 作废：scratch 覆盖 launchd 环境、zlib 空分配器、GPU7/8 宽行未入日志、clients 缺 `WidgetRenderer_Default`），boot args 992 B；首版补丁的 bindfs 挂载参数在 scratch+0x400/+0x600，覆盖了 cache 阶段留给 launchd 的环境串/向量（+0x400/+0x600、向量 +0x500）→ init exec 会把 `/private...` 当 envp；现改为 +0x800（mkdir 仍 +0xc00），chown 失败时只设 mode；**KERNEL 强制 NO_ZCFUNCS，`inflateInit` 不装默认分配器**，原 blob 补丁会在首启空指针调用，addon3 补上用 `kalloc_data`/`kfree_data` 的 `zalloc`/`zfree`（块头存大小）；**GPU7/GPU8 宽行（插件 R/T）现在变更时写入内核日志**（`UKE GPU7:`/`UKE GPU8:`；屏幕第 5/6/7 行在日志里分别是 `UKE GPU2:`/`UKE GPU3:`/`UKE GPU4:`），addon3 及更早没有；clients 名单在 addon5 补上 `WidgetRenderer_Default`（驱动 int-196/200 记录：无原生设备时小组件白块）；校验器强制 `iconservicesagent` 与 `WidgetRenderer_Default`；`sheng-iconservices=1` 让 iconservicesagent 重新启动（默认仍由 lite_jobs 禁用）。`offline-legacy-default-surface=2` 把 32 MiB seed region 映射为 surface 的 +0x30 descriptor（stable=1 保持不变），日志 `UKE: surface mapping -> region … (mode 2)`；用于验证 CA 区域 allocation+0xF000 的短映射。未刷机；下一步看 `UKE GPU addon:`、`M a5.0/5.0`、`o>0`、插件 marker 与 `descbytes`/`regionbytes` |
| 2026-10-27 | addon5 主机侧验证与日志判读 | 五组 addon5 镜像（stable/map2/noicon/fbgpu/probe）重新跑 `scripts/check-gpu-addon-host.py` 全部 PASS：blob 解析、CodeDirectory CDHash、owner trust、内核内嵌 blob、payload 参数 ≤1000 B、vendor_boot@0x1000。新增 `scripts/check-gpu-addon-boot.py`（自测通过）自动判读 `UKE GPU addon:`/`UKE GPU3: M`/`UKE GPU4: K`/`UKE GPU7/8`，覆盖 probe/stable、缓存启动（cookie 命中时不打印安装行，以 `a5.0/5.0` 为准）与 68f1/68f2/68f3。发现契约审计：publish/clients 解析到 bundle 同级的 `/System/Library/Extensions/`，Turnip 在 bundle 内，Metal 按 `Extensions/<MetalPluginName>.bundle` 加载；驱动 4 个连接槽、槽 0 按名保留给 backboardd，其余进程拿不到设备时停在 0x68f2 并保留软件渲染。init-exec scratch 逐项核对（argv 0、cache 挂载参数 0x100/0x120/0x200、环境 0x400/0x600 与向量 0x500、addon 挂载 0x800..0x920、staged mkdir 0xa00、addon mkdir 0xc00..0x1000）互不重叠。未刷机；提交 f346cbb/3c0fa2d/beb2490/6058abb |
| 2026-10-27 | UI 切换时 pmap panic 根因 | panic 精确发生在日志刷屏→图形界面切换（80–236 s，非随机）。原厂 `IOMobileFramebufferLegacy.stop` 在同一时刻清 `+0x1618` 并释放 region/temp（审计 slot333/stop 指令）；region 是 `late_probe` 播种的 32 MiB `IOBufferMemoryDescriptor`，temp surface 建在其上，而 CA 仍持有该 surface 的 user PTE → `pmap_assert_free`（用户映射指向已释放物理页）。修复：`LegacyDisplayProbe.cpp` 创建 region 时 retain，并在收养 temp 时 retain `+0x28`/`+0x30` 内存 descriptor（boot 生命周期内故意不释放），使 user 映射始终有效。另：panic 上下文 UFS 写不可用（flush2 无记录），改为 `panic()` 把标题写 PRAM、下次启动由 UkeLastLog tick4 注入 userdata。addon6fix 镜像（kernel `ab7bfafa…`、payload `fc969bfb…`、vendor_boot `81b19cdc…`，995 B）已构建/校验，按要求**未刷** |
| 2026-10-27 | panic 屏绕过漏斗＝持久化从未执行 | 审计 `patch-xnu-entry-probe.py`：`--panic-screen` 同时把 `_panic` 与 `_panic_trap_to_debugger` 的前两条指令改成跳转到 panic screen 后 `park`，所以 flush/flush2 在 panic 漏斗里加的 `uke_panic_flush`（以及 XNU 自己的 `PE_init_panicheader`/`SavePanicInfo`）**一行都没跑**——这解释了 userdata 里 `PREVIOUS XNU LOG (msgbuf):` 永远为空、panic 标题也不落盘。修复：panic screen 格式化器补 `%#` 标志（此前 `%#llx` 会把 `llx` 当字面量打出来，正是“11x”的来源），把完整消息 + TCR/SCTLR/pages/free 收进 1 KiB 栈缓冲；字号试过 `SCALE 1`（addon8q，一屏能放下但**在 11.2″ 面板上根本看不清**，已回退），仍保留 `SCALE 3`，靠落盘那份取完整文本；置好首-panic 标记后再调用由 patcher 从内核符号解析的 `_uke_panic_flush`（解析不到就编译掉，不影响画屏）。`UkeLastLog` 侧：暂存块改成 `[SAV1 0x31564153][len][text]`，新增 `ukePanicStaged` 让 500 ms 的 FUNK/ramoops 快照不再覆盖它，并等 ~52 ms（`cntvct`）让已在跑的快照收尾；下次启动写者线程开头 `uke_panic_restore()` 把文本以 `UKE PANIC: …` 注入 msgbuf→userdata。构建坑：内核构建**必须** `export UKE_GPU_ADDON_DIR=…/build/uke-gpu-addon`，漏掉时 kernel 少 12 MiB（没链进 `GpuAddonBlob.o`）、addon 不安装、`check-gpu-addon-host.py` 报 “does not embed this exact blob”。addon8q：kernel `41b8dd28…`、payload `1c54c874…`（993 B、tag addon8q）、vendor_boot `1f9f2d7e…`（85,782,528 B），主机校验全 PASS；**未刷** |
| 2026-10-27 | addon8r 实机：panic 真值与 owner 诊断 | 字号回到 3 倍（1 倍在 11.2″ 上看不清），`%#llx` 生效后 panic 屏第一次给出真值：`pmap_assert_free: Found at least one mapping to 0x9a5231000. First PTEP (0xfffffff0a3963868) is a User CPU mapping (pmap: 0xfffffff812c1b048)`，`TCR=0x22A51A251A SCTLR=0x3454599D pages=0x126232 free=0x9a7`。userdata 窗口（`ukelog-addon8r.bin`，本次启动 89 条记录，到 193 s）显示：崩溃前 10 s `UKE MEM: free` 从 13754 页掉到 panic 时的 2471 页（4 KiB/页，≈54 MiB→10 MiB），期间没有任何内核错误行，只有插件持续 `6a3c/69f3` 交替（`CacheExport.h:303`：本轮 cache 没送出去，下一轮重试）和 `InvalidDescriptor(ColorAttachment/VertexAttribute)` 编译失败；心跳 `M a5.0/5.0 … o4/0/2`、238 进程。说明 panic 发生在**内存回收最凶的时刻**，不是随机的显示切换。为定位“谁还映射着这页”，新增 `patches/xnu/zzzzzzzz-uke-pmap-assert-owner.patch`：在 `pmap_assert_free` 的 panic 串里补 `pmap->pmap_pid`/`pmap_procname`（MACH_ASSERT 才有）、PTE 原值、`ptep_get_va` 的虚拟地址和 `__builtin_return_address(0)`（哪个 VM 路径在释放）。注：`vm_page_lookup` 需要 (object, offset)，没有 pn→page 的公开接口，页状态这版没带。addon9：kernel `ca6b220d…`、payload 992 B tag addon9、vendor_boot `1d8dce5a…`，主机校验全 PASS；**未刷** |

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
- 触摸移植：`docs/TOUCH-PORT.md`


### 2026-11-02：initrd 与 DXE 堆重叠独立复核（候选未刷）

代码对照上游 uefi-sheng 66421cb 与官方文档源 a4419c8，发现旧设备 live initrd
0xB2D51000..0xB7FFF41C（本文件阶段表已有记录）落在 uke 的 DXE Heap
0xA8000000..0xC3A00000。0x01000000 是镜像头参数，ABL 会重定位，不能当成运行时实址。

修正：固件 SEC 内存表预留 0xA8000000..0xB8000000，并保守保护到上游同款堆起点
0xBAB00000；新堆 0xBAB00000..0xC3A00000。PublishInitrd 在发布前检查 GCD Reserved，
不再尝试对现有区域晚到 AddMemorySpace。DarwinBoot 去掉写死且错位 1024 B 的 RAWDIFF，
改显示 INITRD/IMAGE/overlap/FDT 实址。分片与原内核保留以隔离变量。

42 DTS 保留区/77 描述符/0 静态错误，firmware 与 bootimg 构建通过。
候选 ../uke-port/releases/2026-11-02-ipados27-initrd-lifetime：
boot db8a7d0b…，FD e4720db0…；payload 527ec783… / vendor_boot ef0635db… 原样复用 resv2。
整载荷内核 cb6e3ba7…，预期 CRC C43F1F3EC8168A4C，IMAGE overlap 0。未刷，未宣告修复。
详细证据/错误推断更正见工作区 docs/status/initrd-lifetime-review-2026-11-02.md。


### 2026-11-02 01:38：initrd-lifetime 四分区已刷

当次用户明确授权“刷吧 设备在fastboot”。设备 b8a81f12，product uke。
release SHA256SUMS 四项全过，boot_a/vendor_boot_a/boot_b/vendor_boot_b 写入全部 OKAY，
set_active a 并回读 current-slot=a，reboot OKAY。boot db8a7d0b…，vendor_boot ef0635db…。
日志：uke-port/releases/2026-11-02-ipados27-initrd-lifetime/flash-20261102-013848.log。
仅确认刷机传输成功；诊断 CRC C43F1F3EC8168A4C、IMAGE overlap=0、live INITRD/FDT
与后续内核画面待观察，未登记新设备里程碑。


### 2026-11-02：设备照片验证加载完整性，纠正日志叠加误读

照片 IMG_20261102_013913：CRC C43F1F3EC8168A4C 与主机一致，E1/E2 六个采样全对，
WINDOW delta=0、BAD n=0、IMAGE overlap=0。live initrd B1F51000..B7FFF4E8，
IMAGE AF7796018 +5AF0000，FDT B1D50000 +EF2D4，WINDOW 8C0004000。
本次加载损坏已消失。照片已归档 initrd-lifetime release，SHA256
b5d045911c5d691a4ab2df31e46c6cc9f318970645df9e032fee8e7e2f5912f1。

用户报告 15 秒后三条带、其他区域未全部刷新。DarwinEnter / stage1 / stage2 各只画 1/16，
残留内容符合实现；品红绘制在 msr SCTLR_EL1 之前，不能据此确认 MMU 后执行。
明确更正本轮口头误判：叠加的 Not Found 属于 UEFI 实体按键/ConIn，不是 initrd 发布失败；
不据此扩大预留区或修改发布逻辑。

下一 stage3-reset 诊断已构建、未刷：boot db8a7d0b…，vendor_boot 2da67ff5…，
payload 07c915c9…，完整内核 9613a452…，预期 fold 34A37EB42FDF7FB4。
首次 stage3 + panic overlay 构建因 cave 超界被脚本拒绝；第二次仅诊断关闭 overlay 通过。
最终 stage3 site fffffff00af14438 / cave fffffff00af125c0 / 72 bytes 逐字节匹配。
board.toml 临时输入已还原（sha256 0b8c59a3…），没有刷写。


## 2026-11-02 02:07 stage3-reset 设备结果

当次授权刷 stage3-reset，boot_a/vendor_boot_a/boot_b/vendor_boot_b 全 OKAY，
set_active a 并回读确认，reboot 成功。boot db8a7d0b…，vendor_boot 2da67ff5…。
用户随后明确反馈“没问题 自动重启了”。

最终载荷中探针位于 0xfffffff00af14438（SCTLR 开启与 ISB 之后），
通过 branch 到 0xfffffff00af125c0 的 72 字节代码，等待配置的 6 秒执行 PSCI SYSTEM_RESET。
此正结果确认本次 MMU 后执行、计时器与 SMC 复位路径，6 秒间隔未仪器测量；
不代表 arm_init/TCR 切换、slide/sign 或 static_if 已通过。
证据：stage3-reset release 的 flash-20261102-020658.log、probe-check.json、device-observation.json。
下一诊断计划移到 arm_static_if_init 调用返回后。


### 2026-11-02：after-sif-reset 候选已构建（未刷）

stage3-reset 已由用户自动复位反馈验证。下一检查点选 arm_init 内 _static_if_init 返回后；
arm_static_if_init 包装已内联，实际调用点 fffffff00b62d98c -> fffffff00ad32440。
72 字节 probe 放 fffffff00af12620，最终载荷逐字节验证，先 BL 原 callee 再 6 秒复位；
旧 stage3 位点恢复，不会提前复位，panic overlay 与上版同样关闭。
release 2026-11-02-ipados27-after-sif-reset：boot db8a7d0b…、vendor_boot 0d339c62…、
payload da688e25…、完整内核 115ec427…、预期 fold 1CEEAB8FEE60E2B8。
board.toml 临时输入已还原，XNU 源码/boot args 没改，ADT seed/seal 重生成。未刷。


## 2026-11-02 after-sif 设备通过与“为什么没有日志”

02:22:58 四分区写入、A 槽核对与 reboot 成功；用户报告“自动重启了”。
本版在 arm_init 对 _static_if_init 的调用返回后执行计时复位，因此 TCR 切换、
此前重定位/签名路径、static_if 返回已验证。没有验证更晚的初始化。
boot db8a7d0b… / vendor_boot 0d339c62… / 完整内核 115ec427…；
证据在该 release 的 flash-20261102-022254.log、probe-check.json 与 device-observation.json。

用户记得的早期 arm_init / sif 刷屏来自 uke-early-console.patch（自定义 uke_fb_putc 与 PE_kputc 钩子）。
当前 series 已不包含此补丁，只有上游系列及两条常规视频控制台适配。
当前源码顺序：arm_init 的 static_if -> PE_init_platform(FALSE) -> arm_vm_init ->
调试输出初始化 -> PE_create_console -> PE_init_printf(FALSE)，更后面的 PE_init_iokit ->
PE_init_printf(TRUE) -> vcattach。当前复位点早于这些输出建立阶段，所以看不到日志是预期结果。
此结论只解释该有意复位的诊断版；原先不复位镜像为何仍无日志，仍需定位更后的停点。
不将 panic_screen=false 当作关闭普通日志；它只关闭额外 panic renderer。
下一检查点：arm_init 内 _arm_vm_init 调用返回后。


## 2026-11-02 after-vm-reset 候选（已构建、未刷）

为继续定位正常视频日志出现之前的启动段，使用现有 stage4 探针验证 arm_init 的 _arm_vm_init 返回。
调用点 0xfffffff00b62dce4 -> callee 0xfffffff00af08d68，cave 0xfffffff00af12620，72 字节代码。
先 BL 原 callee，返回后等待 6 秒并 PSCI 复位；旧 static_if/stage3 位点恢复、panic 入口恢复，
临时 board.toml 原样还原，以上均对最终 FAT 载荷核对。

release：uke-port/releases/2026-11-02-ipados27-after-vm-reset/。
boot db8a7d0b…，vendor_boot 9efd154b6bf5c49ecde164134e4ac8744406aa01f226dd35ee4d71cc5ffbfd55；
payload 502b7b85…，完整内核 23f7dd16…，预期 fold 5F24568B8CC3C9A9。
XNU 源码/boot args 不变，panic overlay 与前两版一样关闭，ADT seed/seal 重新生成。
尚未刷机，不宣称 arm_vm_init 通过。本探针也早于正常调试输出和 PE_create_console；
无刷屏不单独代表输出损坏。负结果需排除 VM 阶段变化后的探针映射等条件，不能直接点名被调用函数。


## 2026-11-02 无复位 VM 阶段记录候选（已构建，未刷）

用户建议取消主动复位并留下阶段记录。新候选 `uke-port/releases/2026-11-02-ipados27-stage-ring/` 不含 PSCI 复位探针，保留入口两条彩带和 panic screen；`payload-patch.log` 确认 `no reset probe`。

XNU 在 `arm_vm_init` 中对 `arm_vm_prot_init`、`arm_vm_physmap_init`、最终 TTBR/TCR、`pmap_bootstrap` 关键边界写阶段名。记录器是 6×96 B 静态环形缓冲，绘制钩子放在 `PE_create_console` 和 `PE_init_printf(FALSE)` 后，仅当 `PE_current_console` 返回 GOP 映射时显示。它不是实时输出：如果代码在 framebuffer console 建立前停止，屏幕仍可能只有三条带，暂存记录不可见。

主机验证：内核构建与 24A446 prelink 通过；`stage-ring-check.json` 检查入口条带、MMU 后原复位位点 `0xd2a00600`、`_arm_vm_init` 原 BL 调用以及环形记录符号；FAT 两半重组为 95,354,880 B。`porting-record.py check --device ../UniXNU-uke-build`：13 个结构化区域通过。

boot `db8a7d0b…` / vendor_boot `110cf296…` / payload `2416891f…` / 重组载荷内核 `6ff13178…` / source kernel `822e1ffc…` / FD `e4720db0…`。尚未刷机；设备状态仍是 after-vm-reset 用户报告的三条带停住、无新日志、未复位。


## 2026-11-02 11:57 stage-ring 授权刷写（待启动观察）

用户明确授权“刷吧”。fastboot 识别 `b8a81f12` / product `uke`，镜像 SHA256SUMS 全部匹配。
`boot_a`、`vendor_boot_a`、`boot_b`、`vendor_boot_b` 四份传输和写入均 OKAY，`set_active a` 后
回读 `current-slot: a`，11:57:38 reboot OKAY；无需写 userdata。
boot SHA256 `db8a7d0b08068d5bd0ea2a3a22766d122e879b84e84d526ca0ae7e8bb32545cc`；
vendor_boot SHA256 `110cf2967e58e35c85c45fd1170c8704e984140638edff1ce9f5b9857ff079a7`。
证据：`uke-port/releases/2026-11-02-ipados27-stage-ring/flash-20261102-115736.log` 和
`flash-result-20261102-115736.json`。这里只确认刷写/命令成功，尚无本次内核/显示验证。

本版不含主动复位探针。请等待约 30 秒，拍全屏：若有 `VM:` 阶段行，保留六行和第一屏内核日志；
若出现 panic，保留完整消息；若仅三条彩带，记录无新文字。记录环需到正常控制台映射后才可见，
不能用本次无文字直接指认 VM helper 停点。


## 2026-11-02 12:00 stage-ring 设备负结果与源码复核

用户照片 IMG_20261102_120008.jpg 仍是绿/蓝/品红三条带，无新内核/VM 阶段文字，用户报告持续不动。
照片已原样留到 stage-ring release；SHA256 0491a21e5100f7de1028db83a9ab35bc7f1a57e12e9dcd0a81a913fac15c50d7。
主机沿实际 FAT 簇链重算完整内核：95,354,880 B，fold CB0692A4D2AED5C3 与照片一致，WINDOW delta=0、BAD n=0。
这次没有得到新的执行检查点；不能把“无文字”点名为 arm_vm_init 内某 helper 卡住。

源码复核：原记录器只在 VM 之后绘制；PE_current_console 返回 PE_state.video，仍是物理 framebuffer 基址，
会被旧绑定器的高 VA 检查拒绝。真正映射在 initialize_screen 的 vinfo.v_baseaddr。
已开始小型实时高 VA 诊断候选，覆盖 static_if 返回后的 CPU/platform/topology 和 VM 边界；
不改 TCR、不加主动复位，不启用整套 BootProgress。主机几何/冲突拒绝/权限/翻译/渲染边界测试通过，
正式构建进行中；未刷。设计与结果见根 docs/status/live-vm-checkpoints-2026-11-02.md。


## live-vm 候选最终主机门禁通过（未刷）

候选：`uke-port/releases/2026-11-02-ipados27-live-vm/`。
boot `db8a7d0b08068d5bd0ea2a3a22766d122e879b84e84d526ca0ae7e8bb32545cc`（与 stage-ring 一致）；
vendor_boot `f71844a52c99dee22820522a59aef9efc0c56cdc2586034bf51331859e07ebc8`；
payload `8223264053b70ee966ee40fa5e8a38ddd7c1ff7d8e1d6e0f53596073058fe492`；
source kernel `6fc74492cb9533fe97972e8ebb98bc21fbdb9b6653134dea1b8347f422910be7`；
最终 FAT 两半重组内核 `4409cead68fb1cd8918f56b265b6873b4c6e7653acfc51833f40664c29740618`，95,371,264 B，期望 fold **EC568E478C79365A**。

正式 XNU kernel/fileset prelink、payload、空 ramdisk bootimg、vendor_boot 重打包均通过；
`porting-record.py check` 为 13 区通过。宿主 mapper/renderer 的 ASan/UBSan 测试通过。
最终载荷保留 12 个相关函数的 source 机器码，48 条标签、调用顺序、入口彩带、MMU 后原字/no-reset 通过。
特别逐条确认 **AT → ISB → MRS PAR_EL1** 及早期映射/退役函数内的 **DSB → TLBI → DSB → ISB**。
boot 及 vendor DTB/table/bootconfig 与上版完全一致；命令行 766 B，noconsole=0。
`live-vm-check.json`、`checkpoint-disassembly.txt`、`host-tests.log` 与 `SHA256SUMS` 留档。

**未刷设备，未取得硬件实时写屏/新卡点。** 本版不主动复位；刷测时拍三条带下方最近六行，
按最大编号判最后进度。首次别名建立失败仍可能没有新文字。沿用既有 allow_missing 的 bring-up prelink，
主机验证不代表完整系统可开机。原 stage-ring 与 gpucomp49 均保留。


## 2026-11-02 13:43 live-vm 授权刷写（待屏幕结果）

用户授权“刷吧”。校验 release SHA256SUMS 全部通过；fastboot serial b8a81f12、product uke，
四个目标分区均回读为 0x6000000（96 MiB），镜像大小符合。
13:43:25–13:43:26 boot_a/vendor_boot_a/boot_b/vendor_boot_b 全部发送/写入 OKAY，
set_active a 成功、current-slot 回读 a，13:43:27 reboot OKAY；未写 userdata。
boot db8a7d0b… / vendor_boot f71844a5… / 最终内核 4409cead…，期望 fold EC568E478C79365A。
本版没有主动复位探针，六行循环检查点的最大编号代表最新进度。
证据：uke-port/releases/2026-11-02-ipados27-live-vm/flash-20261102-134324.log 及
flash-result-20261102-134324.json。这里只确认刷写/命令成功，实时写屏、VM 与内核启动待设备反馈。
观察：等约 30 秒拍全屏，保留六行编号或完整 panic；若仅三条带，保留 CRC 和“无新文字”的反馈。


## 2026-11-02 13:45:19 实机实时写屏通过，最后编号 16

照片 `IMG_20261102_134519.jpg` 已保存到 live-vm release，SHA256 `1f2f2785d91465344fe96467013309edd627b7c6210116288cd13d227a79e9b6`。
屏幕六行按物理顺序为 13 early clock returned / 14 kernel startup enter / 15 kernel startup returned /
16 CPU processor bootstrap enter / 11 thread and CPU bootstrap returned / 12 early clock enter。
最大编号 **16**，不是最底行 12。CRC **EC568E478C79365A**、bytes **5AF4000** 与载荷一致；
WINDOW delta=0、BAD n=0，E1/E2 六个采样与主机 inspection 一致。

这张照片证明 VM 前初始高 VA 别名与实时绘制可用，`kernel_startup_bootstrap` 已返回；
尚未验证新 TTBR1 接续、正常 console 或 arm_vm_init 返回。
**CPU processor bootstrap 标签实际包住 timer_call_init → cpu_init → processor_bootstrap 三次调用**，
不能只据此点名第三个函数。照片是瞬时观测，不附带精确停留时间或寄存器现场。

下一诊断只拆分这段并给内部主要调用打点：timer longterm/abstime、CPU timer queue/CPUID/cache/debugid、
processor boot pset/processor_init 与锁边界。不跳过初始化、不改映射/TCR、不加复位；候选构建中、未刷。


## 2026-11-02 CPU bootstrap 细分候选（主机通过、未刷）

根据 live-vm 设备照片最大编号 16，拆分 timer_call_init/cpu_init/processor_bootstrap 三调用；
新增 timer longterm/abstime、启动 CPU 的 CPUID/cache/debug/subtype、AMP pset 与 master processor_init/锁边界。
不更改原始调用行为，不跳过初始化，不主动复位；已实机通过的映射/渲染与 VM 打点原样复用。
本次对照 sheng 本地 origin/main b76a1679433b0ab5386ac9957715f5e3fecdf46e（不是旧 checkout HEAD 08cde20）；
未在线刷新，patch 系列没有直接修改上述三函数及 do_cacheid 的针对性修复。

release：uke-port/releases/2026-11-02-ipados27-cpu-bootstrap/。
boot db8a7d0b…（逐字节复用 live-vm）；vendor_boot `60f8924376e703c2c41064595239ee666aa42258639cfa86e9e778776f8b32ff`；
payload `e83c42b1c54eaed1572e56c65c2f163970a0d1e8d9c95779275f991d640cdc19`；source kernel `c5d122610e521e4ae13a09c6376aa29c393fd4d419500a5e59910d5a3e74f87e`；
最终 FAT 重组内核 `502586b1c3e7eb6612bf124f51035fca5a04b3209cd2e8e51849265a4447c52f`，95,371,264 B，预期 CRC **8746BA27C34E1E6A**。
实际目标预处理 97 个有效标签全部存在最终载荷；16 个相关函数机器码与 source 一致。
原三调用顺序、mapper/VM/console 接线、no-reset、AT/ISB/PAR 和 DSB/TLBI 序列及容器门禁通过。
正式 kernel/prelink/payload 成功；porting-record 13 区通过，未新增重复 mapper/renderer 测试。
证据：cpu-bootstrap-check.json、cpu-call-order.json、active-checkpoint-labels-by-source.json、源码和反汇编。

**未刷下一版**。应在下一次当次授权后刷四分区、设 A、回读并重启；再拍六行最大编号附近的阶段或 panic。
新编号随细分改变，不能拿上一版 16 对应关系去解释细分版。


## 2026-11-02 AMP pset 停点与最小 ADT 修复（候选未刷）

用户照片 `IMG_20261102_140731.jpg`（原件保存在 cpu-bootstrap 与 amp-cluster releases）显示编号
39/40/41/42 的两个 processor lock 初始化均返回，最大编号 **43 PROC AMP boot psets enter**；
CRC `8746BA27C34E1E6A`、bytes `5AF4000`、WINDOW delta=0、BAD n=0 均与该版 payload 一致。
本版内核实测配置 `__ARM_AMP__=1`、`CONFIG_SCHED_EDGE=1`。最终旧载荷 `kernel.macho.adt` 的 8 个 CPU 节点
均缺 `cluster-type`；`ml_parse_cpu_topology()` 对缺失值默认为 `CLUSTER_TYPE_SMP=0`，
而 `cluster_type_to_pset_type()` 在 AMP 分支只接受 E/M/P，其余执行
`panic("Unexpected cluster type %d")`。该确定的 ADT/XNU 契约错误与停在 `psets_create_for_cluster()` 的 43 号检查点吻合；
**照片没有捕获 panic 文本**，因此不声称观察到了 panic 本身。

已核对原厂 DTB CPU domains/capacity 和上游 sheng DTS 映射。只改设备 DTS：CPU0–2 E、CPU3–7 P；
重建 payload 后用 FAT 簇链解析器提取最终 ADT，逐字段证明新增 8 个 4-byte cluster-type 属性，
其它 ADT 节点/CPU state/reg 未变（chosen 随机种子按正常构建更新）。boot 和 vendor_boot header/DTB/table/bootconfig 不变，
payload 里唯一变化文件是 ADT；内核两半逐字节相同，CRC 仍 `8746BA27C34E1E6A`。

release `uke-port/releases/2026-11-02-ipados27-amp-cluster/` 已完成 host 验证与 SHA256SUMS。
**未刷设备**；待用户下一次明确授权。建议验收顺序：43 返回、继续创建 E/P pset、processor bootstrap 返回，
再进入已存在的下一阶段打点；越过 43 只验证本项契约修复，不代表 VM/完整启动通过。


## 2026-11-02 14:28 AMP cluster-type payload 修复授权刷写（待结果）

用户授权“刷吧”。设备 serial b8a81f12 / product uke，四个目标分区均为 0x6000000（96 MiB）；
release SHA256SUMS 校验通过。14:28:41 boot_a/vendor_boot_a/boot_b/vendor_boot_b 全部发送/写入 OKAY，
set_active a、current-slot 回读 a、reboot OKAY；未写 userdata。
boot db8a7d0b… / vendor_boot 9fcf4a5e…；内核仍为 502586b1…，期望 CRC 8746BA27C34E1E6A。
新版只含 8 个 CPU cluster-type ADT 属性；本次刷机已授权，设备是否越过 AMP pset 检查仍待观察。
证据：release flash-20261102-142841.log / flash-result-20261102-142841.json。
请等待约 30 秒拍全屏，确认 43 有返回、是否出现 44 PROC AMP boot psets returned，并带上最大编号或完整 panic。


## 2026-11-02 AMP 修复后推进至 AppleImage4，补 strprefix reviewed 绑定（未刷）

用户转述：amp-cluster 刷机后屏幕刷新，出现
`original component called an unmapped kernel function from 0xfffffff009019bd8 (x0 0xfffffff00ba64230 x1 0xfffffff00751b025)`。
没有本轮照片/日志原件，保留为用户近似转述；不能虚构中间检查点。新 panic 说明启动已进入原版组件代码。

确认 reporter 的 from 是去 PAC 后的 **LR/调用返回地址**；旧 docs 把它当目标的规则有误，已更正。
from-4=0xfffffff009019bd4 -> BL AppleImage4 thunk 0xfffffff00903a908 -> 认证槽输出 0xfffffff0080e1a78 /
原槽 0xfffffe00080e1a78 -> 真正原目标 **0xfffffe000ae8254c = strprefix**。
x0/x1 分别对应 com.apple.security.AppleImage4 与 com.apple.security.。原目标 40 B，
去 BTI 后 36 B 与 native _strprefix 逐字节相同，原 sha256 8af8e336c9f47462aa6495b0238ecdb9e747287fde579fe65723b2f531389515。

实际活动 kit 是 UniXNU-build/kits/XnuPrelink（9cfa5f5 + 本地未提交改动，解析路径已核对），
只给 24A446 fixture 加 1 条 reviewed 锚点（131→132）；没有跳过 panic 或 AppleImage4 功能，没有重编译 XNU 源码。
正式 build prelink / build payload 和 vendor_boot 打包通过；59 项 kit 测试通过、13 区结构化检查通过。
最终 FAT 载荷槽实际从 panic stub 0xfffffff00b620274 改为 _strprefix **0xfffffff00aecb7fc**，
3 个 retained 引用槽都修正且 PAC 属性原样；caller/thunk 字节不变，8 CPU E/P ADT 保留。
原 16 个诊断函数机器码、97 标签、no-reset 与 AT/ISB/PAR / DSB/TLBI 指令门禁通过。

候选 release **uke-port/releases/2026-11-02-ipados27-image4-strprefix/**；**未刷设备**。
boot db8a7d0b… / vendor_boot `70cb4c887b243051d61b9bf57aee25ce43d1639403490e00129d4f2582659c6a` /
payload `8b82441ab4ee879e95a545c7989acacfc5b82598b6072afc6ec3e656e2fd53ce` / 源预链接内核 `f8ddb56c2788b86d53d2f3b1beda3ea93b4c39a415d2b7b4efe71bd658df0b71` /
最终 FAT 内核 `7be51b1e5a69a388d752919e655ade315b9653556628b31c788e19310d4b6673`，95,371,264 B，预期 CRC **485AB76E1D008C12**。
证据：strprefix-binding-check.json、final-code-check.json、kernel.map.json、kernel-prelink.json、
original-strprefix/native-strprefix.disasm.txt、fixture 前后副本、host-tests.log 与 SHA256SUMS。

下一次当次授权后刷四分区、active a/readback/reboot；观察是否继续、若有新 panic 保留全文及 from/x0/x1。
其它原版接口仍有 missing/stubbed，不能承诺完整启动。本轮不主动复位、不写 userdata。


## 2026-11-02 新 panic 与进度交接

strprefix 版已刷（flash-result-20261102-145745.json）；用户报告下一次 from 0xfffffff009023d34，x0 0xfffffff0080e16d0，x1 0x4。
此轮先总结文档，核对 sheng 最新 main 与实际构建路径；完整进展见根 docs/status/ipados27-panic-upstream-review-2026-11-02.md。未新增刷机。


## 2026-11-02 进度总结与最新上游启动流水线核查（本轮不改镜像）

先保存用户新 panic，再 fetch sheng/core/XnuPrelink main：6a3d35c / 04573c5 / 2b39ff7。
image4-strprefix 实际已在 14:57:45–14:57:48 刷四分区、A 槽回读/reboot 全 OKAY；用户报告新
from 0xfffffff009023d34 / x0 0xfffffff0080e16d0 / x1 4。最终内核沿 LR-4/thunk/认证槽定位
原目标 0xfffffe000ad3e380：AppleImage4 调 kalloc_type_impl_external，x0 描述符名字为 site.nonce_magazine_t。
本轮没有新增 fixture 或下一镜像。

确认 **8/8 sheng 设备 XNU patch 与最新 main 逐字节一致**，但上游新 core 474cfee、kit 2b7baca/b1888c1
和 sheng 6dafef9 的构建/预链接组未引入。本地 reference 实际 DEVELOPMENT/BUILD_LTO=0，
与上游 RELEASE/LTO 不同；缺 25 个上游函数锚点、19 个数据锚点，另缺 N_ABS 原版跳板重定位和回归门禁。
当前报告 missing 569 sites / stubbed 2501 sites，上游白名单 2 / 24；配置不同，差值不是预计 panic 数，
但足以说明不应继续逐个填洞上机。上游报告有 SpringBoard/backboardd/wifid、120s 无 panic 的历史设备证据，
并限定它不是 clean-checkout 或视觉 UI 的新验收。本地尚在原版安全组件初始化，没有根卷/用户态验收。

下一优先是保留 Linux/uke 资产、修复与诊断前提，成组对齐 RELEASE-LTO、fixture、builder/emitter、门禁；
不盲目覆盖 gitlink/源码，不再只因一个 reviewed 绑定通过就刷下一版。
完整报告：根 docs/status/ipados27-panic-upstream-review-2026-11-02.md。
证据：uke-port/fixes/2026-11-02-upstream-startup-review/（patch SHA、原版/本机反汇编、差异与上游快照）。


## 2026-11-02 上游启动流水线同步实施中

用户明确要求开始修复并尽量对齐上游。编辑前已保存三个活动仓的 tracked diff/index/changed/untracked 普通文件，
备份：/home/allenwdk/UniXNU/uke-port/fixes/2026-11-02-upstream-startup-sync-20261102-154650；旧 image4-strprefix release 原样保留。未写设备。

已将 XnuPrelink 最新 origin/main 合入活动 fork（merge 616c268），保留本地已验证 anchor、
root/member subtype 契约和 Linux data-section guard，fixture 合集 157 functions/26 data；62 项测试通过。
core 按 474cfee 的整组启动流水线补丁接入（含 runtime/回归 policy），保留本地 Linux/供源/payload 布局改动；
设备显示/HID builder 删 native-slide 补偿，改由 emitter 最终布局后重算 N_ABS 跳板。
board recipe 切换上游真实 RELEASE/LTO 对照。源 HID adapter 接线并以公开 IOHIDDevice.iig 生成缺失声明，
源码驱动和 53 个 24A446 IOHIDDevice 绑定/vtable diversity 检查通过，不再用旧弱 HID 空桩。

现有 /opt ld64 是 --disable-lto-support，系统 libLTO23 又读不了 clang24 产生的新 bitcode 属性。
本轮在用户目录单独编译同版本 cctools-port 开 LTO，再按实际 clang commit 编译 libLTO24，
不覆写 /opt 或系统工具。local build inputs 指定该 ld/lib；reference 缓存签名覆盖两个工具文件哈希。
完整27.0 RELEASE/LTO最终链接进行中；4项 reference 测试、4项 runtime/gate 测试与 check-unixnu 通过。

本轮尚未有最终 prelink/payload/实机成功结论，不刷超过门禁的镜像，不临时放宽上游剩余缺口白名单。


## 2026-11-02 Linux 编译差异与匹配器续修（主机阶段）

已完成真正 RELEASE/LTO 和运行内核的 BTI 对齐；数据缺口从 78 降至上游同款 2 处。
matcher v9 先用代码/类 vtable 强证据再用函数顺序回退，修正类归属前缀和终止后对齐填充识别；66 项测试通过。
四个新增数据锚点均有原始代码/字符串/链式指针证据。中间候选仍 498 处 stubbed 引用，未通过上游 24 处边界。
继续构建只用于识别的 RELEASE/LTO 对照，以 -fno-ptrauth-auth-traps 对齐 Apple 原版的指令生成；
运行内核保留认证检查。没有新 release/payload 或刷机，设备仍是 14:57 image4-strprefix。
证据：/home/allenwdk/UniXNU/uke-port/fixes/2026-11-02-prelink-linux-audit/。


## 2026-11-02 18:48 当前可接续状态（以此节覆盖前面的中间构建状态）

启动相关上游修复组已接入；core 尚为 `1525faf` 加 staged/unstaged 修复，未伪造整仓 merge 完成。
XnuPrelink 为 `616c268` 加本轮 matcher v9/fixture 修复。sheng/core/kit 本轮 fetch 的 main 仍为
`6a3d35c` / `04573c5` / `2b39ff7`。没有推送或刷设备。

已完成 Linux 的真正 RELEASE/LTO、BTI 和仅识别对照的认证检查生成对齐。
仅识别用 RELEASE 的 `-fno-ptrauth-auth-traps` 不作用于运行内核；native 仍保留 PAC/认证失败检查。
对照 SHA256 `91760a0da98c0ca41ea7c145ea093c033a9be3f4ef873977496c26e79c10812c`；当前 native SHA256 `7014bb4aeb620f16e71201bbe4af34b3f60806c746ad70268dbec7d41e9f9fe3`。
这些是本次产物指纹，不代表可逐字节复现构建，也不是硬件验收。

最终保留的 matcher 修复：
- 代码、字符串、类 getter/完整 PAC vtable 证据先于函数顺序回退；reference-data 表可被独立证明升级。
- 类归属严格匹配 Itanium owner 前缀，参数中出现类名不构成归属证据。
- 只剔除终止后的不可达零/NOP 对齐；跳入填充的内部边会阻止剔除。
- 短包装函数用全部数据/已识别直接调用消歧，并使用保留分支几何的 checked shape。
- 仅含 BTI/B 的尾跳别名只在身份推断结束后用于调用重绑定，不作为符号命名/顺序证据。
- **已撤回**私有数据批量扩展和多候选“行为等价”通用复用；不以相似代码或参数类名猜身份。

Reviewed fixture 为 182 functions / 30 data；每个新增函数带原字节 hash、size、ABI/源码依据。
例如 OSMetaClass 非删除析构绑定 D2，避免误用 D1/D0 的虚拟陷阱入口；IOService/IOEventSource 构造/析构、
IOBufferMemoryDescriptor 工厂、IOUserClient 包装、thread_tid、clock_get_uptime 等按实际调用形态核对。
分析中曾提出的 get_bsdthread_info 绑定已在发射前删除：该源码加 sizeof(thread)，与原版 thread_id 加载不符；
最终绑定为独立核对的 thread_tid，不保留该猜测。此前两个实机 panic 的 strprefix 与 kalloc_type_impl_external
都已在最终 map 解析；后者确认目标同地址符号别名含 `_kalloc_type_impl_external`。

### 最终门禁结果：仍失败，不能进入新载荷发布

| 项目 | 初始真 RELEASE/LTO（BTI 前） | 当前源码的正式 prelink | 上游边界 |
| --- | ---: | ---: | ---: |
| missing 引用点 | 78 | **2** | 2，逐项一致 |
| stubbed 引用点 | 1497 | **116** | 24 |
| 超出白名单的函数引用 | 1473 | **92，67 个目标** | 0 |

`./unixnu build prelink` 实际退出 **1**，首项实际输出为：

```
unixnu: prelink regression: stubbed com.apple.driver.AppleARMPlatform original 0xfffffe000aec7e34, 1 sites (allowed 0)
```

（允许数未放宽）；未放宽上游 policy。不要把“emitter 写出了临时候选”或单测通过说成正式内核通过。
被拒候选 `6655a921757701f9ffd3fea009395ad3c2e48e4966a40f9facb43dbd041160de`，95,387,648 B；
正式 `kernel.macho` 没有提升为这份候选。报告保持 `kernel.failed.json`。
264 个 N_ABS 原版跳板：205 MobileGraphics、51 HID、8 IOSurface；相对 sheng 的 266 少两个 GPU 跳板，
因为当前 Uke 维持 CPU 渲染隔离，不能为了凑计数开启 GPU。

68 项 kit 回归、4 项 reference cache、4 项 runtime/gate、check-unixnu 与 porting-record 13 区通过。
本轮没有新 payload/vendor_boot、没有发布新 release、没有设备启动结论；设备仍是 14:57 image4-strprefix。
下步直接从 `final-audit.json` 的 67 个超标目标清单继续逐项核对源码/ABI或修正可证明的匹配规则，
全部过上游门禁后才能重新生成和验收最终 FAT/ADT/打点代码与四分区候选。

### 本轮证据入口

`uke-port/fixes/2026-11-02-prelink-linux-audit/`：
`final-audit.json`、`final.failed.json`、`final.map.json`、`final-fixture.json`、`prelink-identity-final.log`、
`host-tests-final.log`、`reference-tests-final.log`、`runtime-tests-final.log`、`check-unixnu-final.log`、
`final-core/prelink/device-*.txt`；`remaining-review.log` 与排名脚本只辅助审阅，**不是自动绑定证据**。


## 2026-11-02 上游 reference 控制实验

固定原版 kernelcache / native 7014bb4a / Linux RELEASE-LTO reference 91760a0d，
将上游与本地 matcher、fixture 分开组合。原版上游 2b39ff7 为 missing 15 / stubbed 904；
上游算法+本地 fixture 为 2/755，本地算法+上游 fixture 为 3/237，当前本地为 2/116。
四组均未过原门禁。新证据显示两个 IOEventSource gate 方法原版为 48 B、Linux 对照为 44 B，
PAC discriminator 与 vtable 偏移相同，但寄存器分配不同；不能据此认定全部 67 个缺口的单一根因。
优先采用上游 Apple clang RELEASE/LTO reference 方案，暂不继续批量添加 fixture。
已备 Mac 输入包 37,459,394 B / SHA256 ba82b51472be56d3c3d8ad6fd7d69664d2edfb66e5e8afc87596345f00a01a1e，
13,590 文件哈希及 shell 语法检查通过；尚未在 macOS 构建。
本轮无运行代码/正式 map/内核替换，无 payload 或设备操作。
工作区详情：docs/status/ipados27-upstream-reference-control-2026-11-02.md；
证据：uke-port/fixes/2026-11-02-upstream-reference-investigation/。


## 2026-11-02 GitHub Actions reference-only 控制实验已启动

用户授权在本机缺少 Mac 时使用 GitHub Actions。已推送研究分支
`investigate/ipados27-reference-20261102`（commit `9935f291c01188d8496a0de76c361f9403aa03d4`），
仅新增 `.github/workflows/reference-control.yml`，活动工作树与运行代码未推送。
输入放在 Allenwdk/UniXNU-uke 的草稿 release `research-ipados27-reference-20261102`，
143 个 256 KiB 分片合计 37,459,394 B，重组后校验原包 SHA256。
大文件和 4 MiB 分片上传均未完成，256 KiB 分片成功；传输探针与未完成资产已安排清理。

运行：https://github.com/Allenwdk/UniXNU-uke/actions/runs/41448047750 。
20:51:31（Asia/Shanghai）开始上游 RELEASE/LTO reference 构建，前置下载、13,590 输入哈希、
Apple 工具链检查已通过；当前尚无编译成功或匹配改善结论。
runner 为 macos-26；缓存按输入包 SHA 与 Apple clang 指纹限定。
下一步下载 reference + provenance，保持同一 native/roots/policy 复测上游及本地 matcher。


## 2026-11-02 20:57 第一轮 Apple reference 结果与控制输入修正

Actions run 41448047750 成功，完整 job 368 秒，Xcode 26.6 (17F113)、Apple clang 21.0.0 (clang-2100.1.1.101)。
reference SHA256 `672f26044284339c0b1f14425d6424d4b306f3abb88e4c0f28bbcdc0cc10198a`，16,320,640 B。
下载后核验 reference JSON 哈希、13,590 份输入 manifest；source/SDK/archives/versions/mode 与 Linux 记录相同。
原版上游 matcher/fixture 的结果是 missing 15 / stubbed 24，函数陷阱逐项符合原 policy，
此前 67 个超标函数目标全部消除；剩余超标数据只有 `kOSBooleanTrue`，13 个引用点。
本地 v9/fixture 则为 18/25，不能把 Linux 专用规则盲目沿用到 Apple reference。

进一步查出独立工作流漏传 `XNU_KERNEL_CHAINED_FIXUPS=1`：source makedefs/MakeInc.def:1124
据此选择 chained fixups 或 ld-classic/threaded_starts。第一轮 Mac reference 实际无 chained pointer diversities，
所以这不是已完成的“仅编译器变量”控制实验，不能把数据差异归因于 matcher 或缺少锚点。
已在研究分支 commit `9141d399ea853b693433613eacf725c0e81dd69d` 补传上游同一选项并触发第二轮，
继续使用原有输入包；尚未添加 kOSBooleanTrue 手工绑定，运行代码未改。
原始证据：actions-run-41448047750/、apple-reference-verification.json、
upstream-macos/local-macos map/report/log、matrix-with-apple-threaded.json。


## 2026-11-02 上游 Apple RELEASE/LTO 预链接门禁闭合（主机验证）

- 上游：sheng main 6a3d35c、core main 04573c5、XnuPrelink main 2b39ff7；匹配器恢复上游 v8，fixture 143 functions/26 data。
- Actions run 41449626495：Xcode 26.6 / Apple clang 21，真实 RELEASE/LTO 且 XNU_KERNEL_CHAINED_FIXUPS=1。
- Apple reference SHA256 `2f9eabe43241bb9b64d367151d79d660e81e3ec7fee8399c3f37a0f5ba3b5aa8`。
- 运行 native SHA256 `7014bb4aeb620f16e71201bbe4af34b3f60806c746ad70268dbec7d41e9f9fe3` 未变，PAC/失败检查保留。
- 正式 `./unixnu build prelink -j 8` 退出 0；missing 2 / stubbed 24，逐项符合未放宽的上游 policy，超标引用与目标均为 0。
- 正式预链接内核 SHA256 `40338ed16f690a3da165b37bf4f4f9ad0f08835386b3274146e339ccedc0e7e3`，95,387,648 B；264 个原版跳板保留。
- 62 项 kit 回归和 porting-record 13 区检查通过；设备仍是 image4-strprefix，当前成果尚未刷机/硬件验收。
- 证据：`uke-port/fixes/2026-11-02-upstream-reference-investigation/formal-success.json`、formal-apple-prelink.log、formal-apple-kernel.json/map.json、upstream-restored-tests.log。


## 2026-11-02 最终待测 release 与上游方案收尾

Actions 41449626495 的 reference 含 42,973 个 fixups，SHA 2f9eabe4…；source/SDK/archive/version/mode 与 Linux 输入相同，chained 开关也对齐。
原版上游及活动 fork 都达到原门禁 2/24，超标目标为 0。匹配器只保留工作区要求的 C++ owner 安全校验、缺 section guard 和 subtype 契约；
owner 校验不改变任何 functions/data 配对或内核字节。额外 v9 规则与 39 函数/4 数据锚点已撤下，原证据留档。
63 项 kit 回归、check-unixnu 与 porting-record 13 区通过，运行 native 7014bb4a… 未变。

待测归档 `uke-port/releases/2026-11-02-ipados27-upstream-reference/`：
boot db8a7d0b… / vendor_boot d7f323e9… / payload e9746979… / 最终内核 7b7f7887…，95,387,648 B，CRC 0779B5A679B2FF5F。
最终 FAT 重组、16 诊断/启动函数、97 标签、264 原版跳板、root/member subtype、3/2/13 个 strprefix/kalloc/kOSBooleanTrue 槽及 PAC、AMP E/P ADT均通过。
命令行、Data seed、trust cache 保留；无主动复位。未刷机/设备验收。下一步为当次授权后的四分区测试。
现有 payload.json 的 payload_kernel_sha256 仍为分片0；完整 SHA 与 CRC 以 payload-inspection/final-code-check 为准。


## 2026-11-02 upstream-reference 已授权刷入（设备启动待观察）

用户明确授权“刷吧 设备在rec”。本轮从 adb recovery 确认 b8a81f12/uke，校验六项 SHA256SUMS 后，
使用 adb reboot bootloader；fastboot 再确认 product uke 和四个分区尺寸。
2026-11-02T21:38:03+08:00 至 2026-11-02T21:38:12+08:00，boot_a/vendor_boot_a/boot_b/vendor_boot_b 全部 OKAY，
set_active a 后 current-slot 回读 a，reboot OKAY。未写 userdata。
boot SHA256 db8a7d0b08068d5bd0ea2a3a22766d122e879b84e84d526ca0ae7e8bb32545cc；vendor_boot SHA256 d7f323e93508c4927698fa8916a948a1aa4a7af2d48480ab217577fae63c2c68。
最终内核 7b7f7887…，95,387,648 B，预期 CRC 0779B5A679B2FF5F；CPU E/P、97 标签、no-reset 保留。
目前只确认刷入与重启命令成功，尚无此版内核/根卷/launchd/桌面启动证据。
证据：uke-port/releases/2026-11-02-ipados27-upstream-reference/flash-result-20261102-213803.json 与同名 flash-20261102-213803.log。
下一步收完整加载屏 CRC/WINDOW/BAD、最后六行阶段编号；若 panic，保留完整 from/x0/x1 与 pc/lr/esr/far。


## 2026-11-02 22:15：接续照片，定位 IIG PAC 与清屏（主机修正，未刷）

已读上一会话的 IMG_20261102_213856.jpg（SHA256 b4f242fb1e77f09983e35617ede15107f76f857dd6a89cb09e1c4c87dc24799a）。
已刷 upstream-reference 的 PC 0xfffffff00b464858、LR 0xfffffff00b4635c4、ESR 0x72000000，
为 IOService::_Dispatch 的 StateNotificationItemCreate_Impl 回调认证失败；x8 完整值被照片右边缘裁掉。
原指令用 IA discriminator 0xa2a5 签名、0xbcad 验证；native 预链接前已存在同一不一致。
portable IIG 不具备 Apple load_member_function_pointer builtin，旧 union 按位转换没有重新认证/签名。

已修 portable IIG generator 的 PAC 分支，以成员类型 schema 读取 cfun，让编译器完成认证转换；保留检查。
QEMU 开 PAC 的同源最小例：旧版 EC=0x1c，修正版正常返回。无 PAC arm64 汇编相同。
清屏修正覆盖第 0 行，并在清屏前 quiet_console；保留换行；AT/ISB/MRS 改为换行分隔。
QEMU 预填全屏旧像素与三行/heap/guard/device-memory 测试通过。最终 renderer 2520 B 字节完全相同。

正式 kernel/payload 退出 0；missing 2/stubbed 24 原 policy 通过，63 项 kit 回归、check-unixnu 和 13 区通过。
最终旧停点改为 0xfffffff00b4644e0 直接 BL 0xfffffff00b55f994；PAC failure handler 保留。
新候选：uke-port/releases/2026-11-02-ipados27-iig-pac/，boot db8a7d0b… / vendor_boot 7b3bd88f… /
最终内核 6229de56… / CRC **008FDA414816627C**。
FAT 重组、97 标签、264 跳板、AMP E/P、3/2/13 PAC 槽、no-reset 均通过；**未刷，root/launchd/桌面未验收**。
系统 /opt 的旧 IIG 未修改；当前 unixnu.local.toml 的 build.iig 指向审计目录 iig-fixed，
构建支持 build.iig / UNIXNU_IIG；setup-toolchain.py 包含同一 generator patch，避免重装丢修正。
证据：uke-port/fixes/2026-11-02-pac-branch/、新 release 的 candidate-status/pac-code/renderer/final-code/final-binding 检查。


## 2026-11-02 22:41:35：iig-pac 已授权刷入，启动待观察

用户本轮授权“设备在rec,刷吧”。recovery adb b8a81f12，product twrp_uke / device uke；
六项 SHA256SUMS 全部通过。使用 adb reboot bootloader，fastboot 确认 product uke，
boot_a/vendor_boot_a/boot_b/vendor_boot_b 均为 0x6000000。
2026-11-02T22:41:35+08:00 至 2026-11-02T22:41:39+08:00，四分区写入全部 OKAY；set_active a 后 current-slot 回读 a，reboot OKAY。
boot db8a7d0b… / vendor_boot 7b3bd88f… / 最终内核 6229de56… / 预期 CRC **008FDA414816627C**。
未写 userdata。97 标签、E/P ADT、无主动复位。只确认刷写与重启命令成功；root/launchd/桌面及 PAC 修复尚无本版实机结论。
证据：`uke-port/releases/2026-11-02-ipados27-iig-pac/flash-result-20261102-224135.json` 与 `flash-20261102-224135.log`。
接着收加载屏 CRC/WINDOW delta/BAD、最后六行阶段编号；panic 需完整首行、PC/LR/ESR/FAR、X8/X16/X17（右边缘完整）。


## 2026-11-02：SourceStorage root step 3 与双根窗口候选（未刷）

用户在 iig-pac（22:41 已刷、vendor_boot 7b3bd88f… / 最终内核 6229de56…）转述：
`SourceStorage: UFS start failed (root step 3: root windows require distinct OS and SystemOS) @SourceStorage.cpp:99`。
本轮无原图；按实际 SourceStorage.cpp 与 pinned ShengUfs 78c93b0 解码，IOStorageFamily/APFS 加载、
策略 handoff 验证、deferred class 注册、hostBringUp 与 findRootLayout 已返回成功，manifestStatus=0；
卡在 ufsBuildRootPlan 的窗口命名/数量门禁，尚未发布根设备或挂载根卷。
旧主机 userdata manifest 仅有 OS window + OS.vroot object；不是“两个已有窗口重叠”的证据。

24A446 J820AP Customer Erase Install 确有 Cryptex1,SystemOS，成员 043-69086-713.dmg.aea（2,348,810,240 B），
解密后 6,016,729,088 B，SHA d5b4873d35c85f96eaafaa31c59602b485afe33b5067eff1af67ba31204cdb4a。
APFS checksum-valid APSB 的 UUID F914C62A-AD00-47C6-8281-5746291C339A，与 profile 相同。
SystemOS 策略复用 OS 的 owner anchor 97a3dbf0…，全部 1,468,928 块通过 C 验证。
board/defconfig 已选双窗口/双策略；builder 拒绝缺失任一策略，驱动的完整门禁不变。

主机发生实际 Btrfs delayed-writeback -5：进程退出 0 不保证内容已落盘。
本轮关闭压缩和 NoCOW 单独尝试仍失败；最终 tmpfs 解密，再分 8 MiB flush/fsync 写盘与完整复读通过。
同时发现旧 OS 缓存实际 SHA 368ecbf0… 与记录不符；从原 release 的已完整复读 OS 窗口恢复到 8a12cdad…。
原 userdata 归档 SHA/窗口未被覆盖。新 userdata 先写 .partial、分批 fsync、检查 root profile SHA 后再替换，
3 个回归覆盖延迟写回失败、错误 root SHA、旧归档硬链接保留。

正式 kernel/payload/userdata 已通过；原 prelink policy missing 2/stubbed 24、63 kit 回归、RootPublication 生命周期测试通过。
新 release：`uke-port/releases/2026-11-02-ipados27-systemos-root/`：
boot db8a7d0b… / vendor_boot c0c43263… / 最终内核 9c7339e5… / CRC **6C8EF01FE60D264E**。
userdata 994f1824…，15,753,805,824 B（14.67 GiB）：
OS @0x100000、SystemOS @0x23d100000、OS.vroot @0x3a3b00000、SystemOS.vroot @0x3a8300000。
最终 FAT 中 kernel.macho.vroot 为两份 signed header 的 1152 B bundle；97 标签、264 跳板、E/P、PAC 与 no-reset 保留。
根镜像和策略表完整复读、实际 production RootPublication.h 的 SystemOS→OS 顺序验证通过。
**本版未刷、双窗口运行和 root/launchd/桌面未验收**。下一次需要当次授权更新 userdata 根镜像前缀及四分区；
默认只写 userdata 起始 15,753,805,824 B，先核设备几何/尾部布局并保留范围外 Data store/日志。
证据：release 内 root-layout-verification/payload-policy-verification/policies/candidate-status 与审计目录。


## 2026-11-02 23:41:10：双根窗口已授权写入并刷四分区，启动待观察

用户明确授权“设备在rec 刷吧”，包括前文说明的 userdata 根镜像前缀和四分区。
本轮恢复 adb server 后确认 recovery b8a81f12 / uke；七项 SHA256SUMS 全通过。
实际 userdata=/dev/block/sda32，容量 241,617,055,744 B；start=23,691,264、size=471,908,312（512 B sectors），与板级几何一致，分区未挂载。
备份旧 manifest 所在的首 1 MiB；原始为 OS + OS.vroot 两项、imageBytes=9,689,890,816。
仅更新起始 **15,753,805,824 B**（14.67 GiB），dd 输出明确 seek=0、iflag=count_bytes、conv=notrunc,fsync。
写入和设备 fsync 完成；整段回读 SHA256 **994f182477966ae3d84d808f1e4839d548cfdb7e0d75aad90c3ee6ba2e8a2b82**，与候选完全一致。
新前缀后 64 KiB、尾部 1 GiB reserve 起点 64 KiB、分区末尾 64 KiB 写前/写后哈希均相同；未擦除 userdata，范围外未写。
随后 adb reboot bootloader；fastboot product uke，四份分区尺寸 0x6000000，boot_a/vendor_boot_a/boot_b/vendor_boot_b 全 OKAY。
set_active a 后 current-slot=a，reboot OKAY。执行时间 2026-11-02T23:41:10+08:00 至 2026-11-02T23:42:36+08:00。
boot db8a7d0b… / vendor_boot c0c43263… / 最终内核 9c7339e5… / 预期 CRC **6C8EF01FE60D264E**。
当前只确认镜像写入和根布局回读，不据此认定 SystemOS/OS 发布、root 挂载、launchd 或桌面通过。
证据：release 中 `flash-result-20261102-234110.json`、同时间 flash log 与 preflash-* 目录。
刷后采集：加载屏 CRC/WINDOW/BAD、`SourceStorage: UFS started...`、SystemOS/OS 发布和 root/launchd 日志；
若 panic，拍完整首行与 PC/LR/ESR/FAR；右侧寄存器和最后阶段编号也保留。


## 2026-11-02：root window preparation failed 与 .vroot 打包修正（未刷）

用户转述 systemos-root 已刷后的新错误：
`SourceStorage: UFS start failed (root step3: root window preparation failed) @SourceStorage.app:99`。
无新照片。实际位置为 SourceStorage.cpp:99；按已刷 NativeUfs 控制流，manifest/双窗口 plan/根 IOService start 已通过，
Prepare（allocation/init/configure/attach）失败，尚未到 Identity/Schedule 或根挂载。
不能仅凭这条概括错误断言第一次失败恰好是表读取；以下是已证明且必然阻止 Prepare 成功的独立打包缺陷。

上一版把完整 manifest.bin 放进 .vroot 对象，而 pinned ShengUfs 的 readPolicyTable 要求纯 block hashes。
OS.vroot 实际 75,104,800 B，比所需 75,104,256 B 多 544 B；SystemOS.vroot 为 47,006,240 B，比所需 47,005,696 B 多 544 B。
两份对象开头均为 VROOT01；544 B 是 160 B header + 384 B RSA signature，已在 payload 的 signed header 中携带。
**上一版此处打包错误；之前只核对对象与完整 manifest 一致，没有核对实际消费者接口，门禁不足。**

正确命令改为 --object OS.vroot=<OS>/manifest.bin:544 和 SystemOS.vroot=<SystemOS>/manifest.bin:544（或 hashes.bin）。
新构建器拒绝长度不符/无对应窗口的 .vroot；真实 CLI 旧参数已拒绝；5 项 userdata 回归通过。
直接编译当前生产 readPolicyTable 做文件模拟 transport：旧数据拒绝，新两表读取成功；
重新与各 signed header 拼接后，真实 C vr_open 两份都返回 0，signed table SHA 匹配。
主机不等于设备分配/IOKit/实际 UFS 运行通过，下一版显式开上游 native-ufs-diagnostics=1 区分更早失败点。

新 release：`uke-port/releases/2026-11-02-ipados27-vroot-tables/`。
boot db8a7d0b… / vendor_boot 286bd6d9…；最终内核与已刷版逐字节相同 9c7339e5…，CRC 仍 **6C8EF01FE60D264E**。
本版靠 native-ufs-diagnostics=1 区分，命令行 791 B；只凭 CRC 不足以区分。OS/SystemOS 与 signed header/key 不变。
完整 userdata 981110da…，仍 15,753,805,824 B。
新增三段精确补丁：OS.vroot（含旧尾页清零）、SystemOS.vroot（同样）、4096 B manifest，共 **122,122,240 B（116.46 MiB）**。
将三段虚拟替换到旧完整 userdata，整镜像 SHA 与正式完整重建相同，证明无需更改范围外任何字节。
下次授权后备份并验证 before SHA，再按两表→manifest 的顺序有界写入，逐段 fsync/回读 after SHA，随后四分区/A 槽/reboot。
本轮未刷；还需 root Prepare、SystemOS/OS 发布、根挂载、launchd/桌面的设备证据。
证据：release 的 consumer-verification、userdata-patches、observation、candidate-status、最终 FAT/绑定验证；审计目录 `uke-port/fixes/2026-11-02-vroot-tables/`。


## 2026-11-03 00:00:33：纯哈希表三段修正与诊断四分区已刷，启动待观察

用户授权“刷吧 设备在fastboot”，随后指定原厂四分区与 fastboot reboot recovery 的原命令；
该命令全部 OKAY。恢复 adb server 后确认 recovery b8a81f12/uke，未挂载 userdata=/dev/block/sda32，
容量 241,617,055,744 B、start=23,691,264、size=471,908,312（512 B sectors），与已验几何一致。
初次 recovery 检查因 adb 未枚举失败，尚无 userdata 写入；重连后成功执行，失败记录保留。

三段 before SHA 与候选 delta 元数据全部匹配，逐段备份到 release 的 preflash-20261103-000033/。
按 OS.vroot→SystemOS.vroot→manifest 写入，共 **122,122,240 B（116.46 MiB）**：
- OS.vroot：seek=3816192、count=18337（bs=4096），写后 SHA cfb16d73…；
- SystemOS.vroot：seek=3834624、count=11477，写后 SHA 48fe5a70…；
- manifest：seek=0、count=1，写后 SHA ebf57107…。
每段 conv=notrunc,fsync，并回读逐字节哈希匹配；未重写 OS/SystemOS 镜像或擦除 userdata。
根前缀终点后、尾部 reserve 起点、分区末尾的 64 KiB 样本写前后哈希均不变。

随后 adb reboot bootloader；确认 product uke/四分区 0x6000000，诊断 boot/vendor_boot 四份全部 OKAY，
set_active a 后 current-slot=a，reboot OKAY。执行区间 2026-11-03T00:00:33+08:00 至 2026-11-03T00:00:44+08:00。
最终 boot db8a7d0b… / vendor_boot 286bd6d9…，内核仍 9c7339e5… / CRC **6C8EF01FE60D264E**；
本版带 native-ufs-diagnostics=1（791 B 参数）。当前仅确认补丁写入回读和刷机，不确认 Prepare/root/launchd/桌面通过。
证据：`flash-result-20261103-000033.json`、flash-20261103-000033.log、preflash-20261103-000033/。
下一步拍完整 UFS ROOT FAILURE，尤其 stage/result、WINDOWS found/prepared/published/bytes/policies/verify、pubPhase/window；
如前进，保留 SourceStorage/UFS started、SystemOS/OS/root/launchd 日志。


## 2026-11-03：SDT 查询 unsafe-kernel-text 初始化断言，ADT-only 候选未刷

新照片 IMG_20261103_000158.jpg，SHA 7bec7d323768e5114b8e614597af72011ab7f2d421955d8e1aed090f82b3b276。
`machine_routines.c:3379 Assertion failed: _unsafe_kernel_text_initialized`；
PC 0xfffffff00af1f194 = ml_unsafe_kernel_text+0x2c；LR 0xfffffff00af4e73c = dtrace_sdt_probes_restricted+0x10。
照片 ESR 只有 0x，不推断缺失值。已刷 ADT 的 /chosen 缺 kernel-ctrr-to-be-enabled。
实际 initializer 只有属性存在且恰为 sizeof(int)=4 时才置 initialized，缺失正好保留 false 并触发查询断言。

补 board [adt.chosen.u32] kernel-ctrr-to-be-enabled=0，4 B little-endian，描述本板没有 Apple CTRR。
按源码 0=>unsafe=true；不伪称硬件 CTRR enabled，不删除断言、PAC 或保护代码。
真实 initializer 宿主四分支测试（缺失/宽度错误/0/1）通过；最终 FAT ADT 仅此新增属性及正常 random-seed 改变。
内核 source/final 逐字节与上一版相同，Data seed/trust cache/boot args/signed policy bundle 不变。

**启动顺序推断（不是媒体/根卷挂载实测）**：最终 SourceStorage BL 0xfffffff00b21e13c 早于内联 pseudo_init 循环；
pseudo_inits[10] 的 signed fixup 0xfffffff00b8511f0 -> sdt_init 0xfffffff00af52930。
照片已到 SDT 初始化，由该实际顺序可推断 SourceStorage 同步 Prepare/策略认证已返回，之前 .vroot 错包停点已越过。
异步媒体发布仍待观察，vfs_mountroot 位于主启动流后续，未宣称 root/launchd/桌面通过。

新候选：`uke-port/releases/2026-11-03-ipados27-ctrr-adt/`，vendor_boot 6d646b36…，
boot db8a7d0b… / 最终内核 9c7339e5… / CRC **6C8EF01FE60D264E**（不变）。
四分区候选已验最终 FAT/ADT/PAC/subtype/97 标签/264 跳板/AMP/no-reset；**未刷，无需写 userdata**。
下一次授权后四分区测试，看是否越过初始化断言与后续 root/launchd 日志；仍保留 UFS diagnostics=1。
证据：observation/symbolized/bsd-init-order/actual-init-tests/adt-property-verification 及 final-code/final-binding。


## 2026-11-03 00:13:01：CTRR ADT 四分区已授权刷入，启动待观察

用户明确授权“刷吧”。本轮检测设备已在 bootloader fastboot，serial=b8a81f12，product=uke、is-userspace=no。
六项 SHA256SUMS 全部核验，四个分区均为 0x6000000，镜像尺寸合法。
2026-11-03T00:13:01+08:00 至 2026-11-03T00:13:05+08:00：boot_a/vendor_boot_a/boot_b/vendor_boot_b 全部 OKAY，
set_active a 后 current-slot 回读 a，reboot OKAY。**未写 userdata**，保留上一版已回读核验的纯哈希表三段修正。
boot db8a7d0b… / vendor_boot 6d646b36… / 内核 9c7339e5… / CRC **6C8EF01FE60D264E**。
最终 ADT 为 /chosen/kernel-ctrr-to-be-enabled u32=0，4 B；内核断言/PAC 保留。
当前只确认刷入与重启命令成功；是否越过初始化断言及 root/launchd/桌面仍待本版设备反馈。
证据：release 的 `flash-result-20261103-001301.json` 与 `flash-20261103-001301.log`。
下一步：完整加载/CRC、后续日志；若 panic，保留完整首行与 PC/LR，若前进收 root/launchd/实际画面。


## 2026-11-03：新票据锁超时，panic 单写入者与去掉15秒等待（候选未刷）

新照片 IMG_20261103_001417.jpg（SHA 7a86e606fd76c6280f9ed5d43ac7fe8c9f84a7b6ebec5d21819af941cd47edcd），
用户看到又刷新日志后 panic；可辨 Ticket spinlock timeout / lock_ticket.c:398、owner CPU4，
owner=0xfffffff75f4c30a8，start=1221987748、now=1222000546、timeout=12000、irq time=0。
首行重叠；锁地址、完整票据字段和触发调用链不能可靠转录，不用近邻符号猜锁身份。
当前停点与先前 unsafe_kernel_text_initialized 断言不同，根挂载/launchd 仍无直接证据；锁根因未定。

1. 用户要求移除最初进内核等待：定位为活动 DarwinBoot 的诊断页 gBS->Stall(15000000)，已删固定15秒。
   正式 firmware 与 bootimg --ramdisk none 成功。最终 FD 的压缩 guided section 解出新 DarwinBoot PE，
   Boot 函数无原 Stall 槽位load；实际 boot gzip 解压后的FD完全一致，ImageShim基址/大小正确，最终固件FDT与已刷版相同。
2. 旧 panic marker 在绘制后才设，多CPU可同时清屏/绘图。旧 renderer 的两核 QEMU 控制已复现混合像素；
   新 renderer 在绘制前用现有 kernel BSS uke_panic_shadow_len 内联 LDXR/STXR 原子抢占，失败CPU park，
   文本复制后STLR发布长度。不是在设备framebuffer上做exclusive，不依赖外部原子runtime。
   这证明旧实现有竞态，不证明当前照片一定由两CPU同时panic造成；新版叠字改善仍待实机。
3. 新增最多6层原始LR的BT行，高地址/AT可读/帧对齐/单调/≤16KiB步长门禁；不读取任意内存。
   六层高VA样例保留原PAC位，通过实际ARM64机器码QEMU逐像素验证。
4. 为使2683B renderer仍在原entry padding内，只在单B入口和116B全零填充均验证时回收96B；
   活动caves前移，renderer 0xfffffff00af17560..0xfffffff00af17fdb，距原末尾剩21B。
   非零填充和源码已有条带入口保留原布局；3组布局控制通过。小空间版本保留不带BT的兼容模式。

四组QEMU（全屏清除、字符串/guard/device、两核panic、六帧链）hash均等于最终载荷renderer。
最终native/source内核字节不变；最终kernel差分仅诊断填充、4条诊断branch和按契约重算的32B POST HMAC，
HMAC已独立复算并验证，原runtime自检/断言/PAC未关闭，锁超时及boot args未放宽。
最终FAT/97标签/16阶段函数/264跳板/PAC槽/subtype/AMP/CTRR ADT/no-reset门禁通过，13区检查通过。

新release `uke-port/releases/2026-11-03-ipados27-panic-owner-nowait/`：
boot eada6132… / vendor_boot 5f647fd8… / 最终kernel f67f57f0… /
CRC **C9630ACDF7253040**，FD764605d2…。**未刷，不改userdata；锁根因尚未解决，等待可读BT现场**。
下一次授权后只刷四分区，A槽回读/reboot；验没有固定15秒hold，panic首行/第二行/BT是否完整且无混合，
随后符号化票据锁调用链。不能据新CRC或主机测试认定设备锁问题已修复。
证据：release的observation、diagnostic-verification、nowait-verification、qemu/*/report、qemu-race-old.log、entry-padding-tests及最终code/binding检查。


## 2026-11-03 01:20:47：panic单写入者/BT/无15秒等待版已刷，设备结果待观察

用户本轮明确授权“刷吧”。设备 bootloader fastboot b8a81f12，product=uke，is-userspace=no；
六项 SHA256SUMS 核验通过，四分区尺寸均为 0x6000000，镜像尺寸合法。
2026-11-03T01:20:47+08:00 至 2026-11-03T01:20:50+08:00，boot_a/vendor_boot_a/boot_b/vendor_boot_b 全部 OKAY，
set_active a 后 current-slot=a，reboot OKAY。**未写 userdata**，保留已回读匹配的纯哈希表修正。
boot eada6132… / vendor_boot 5f647fd8… / 最终内核 f67f57f0… / 预期 CRC **C9630ACDF7253040**。
当前仅确认刷入和重启命令；15秒hold实际移除、panic不重叠、BT可读及锁超时/root/launchd/桌面仍待实机反馈。
证据：release 的 `flash-result-20261103-012047.json` 与 `flash-20261103-012047.log`。
下一步观察无固定15秒停顿、加载CRC；如panic，拍完整首行、第二行及BT行（含最右侧原始LR），确认是否仍混合；
若前进，收 SourceStorage/root/launchd 日志或实际画面。


## 2026-11-03：可读BT确认视频文字控制台vcputc_lock；单变量图形启动候选未刷

照片 IMG_20261103_012307.jpg，SHA 0db799f964289339f4abd3271d592682a1b6e7707833e2ecb08c76225883b8a3。
本次首行/第二行/六个BT可读，无上次混合；原子单写入者和BT已得到一次实机画面证据，不能外推所有panic无竞态。
用户未单独确认15秒等待的体感变化，因此该速度结果仍不写成已验收。

超时锁 **0xfffffff00b967008 = _vcputc_lock**，owner CPU4/thread0xfffffff3e10b8398，orig owner同值。
cticket=e1、nticket=e3、waiting=e2；after12765ticks；start927836389、now927849154、timeout12000、irqtime0，lock_ticket.c:398。
BT raw PAC按此kernel范围还原，并用 return-4 校对caller：
__doprnt→vcputc_options→lck_ticket_contended→hw_lck_ticket_contended→hw_spin_should_keep_spinning→lck_ticket_timeout_panic。
首LR0xfffffff00b635468位于下一函数起点，return-4实际属于lck_ticket_timeout_panic，不误写成hw_lck_ticket_timeout_panic。

已刷spec默认ticket-lock=3,000,000、interrupt-masked=12,000；最终hw_spin_compute_timeout机器码与源码均包含
IRQ关闭且debug-mode=PANIC等条件下取较小关中断预算的分支，实际源码控制复现12000阈值。
不能将其与ml-timeout-lock-panic-scale=40视为同一设置。CPU4持有者PC、是否递归或绘制太慢尚未观察，
不能仅凭等待方BT宣称根因完全修复。根卷/launchd未有直接证据。

控制候选仅撤掉本地诊断参数 **-uketextconsole**；恢复DarwinBoot Video[1]=1对应的原版图形启动路径。
实际vcattach前缀/vcputc_options源码控制：有flag启文字并取锁，无flag保持graphics、gc_enabled门控不取文字锁。
保留-v、UFS diagnostics、panic屏/BT、锁阈值/IRQ预算/SMP，不编新内核，不改userdata。
普通屏幕文字会减少，独立panic渲染仍保留；这是先测的隔离实验，不是已宣布修复所有控制台性能问题。

正式payload和最终FAT差分通过：只unixnu.toml参数及ADT random-seed改变。
source/final kernel、boot/无15秒FD、signed policy/Data seed/trustcache都与已刷版相同。
新release `uke-port/releases/2026-11-03-ipados27-graphics-console/`：
boot eada6132… / vendor_boot 92356130… / 最终内核f67f57f0… / CRC仍 **C9630ACDF7253040**，参数775 B。
最终97标签/16阶段函数/264跳板/PAC/AMP/CTRR/noreset门禁和13区通过。**未刷；无userdata写**。
下一次授权后四分区/A槽/reboot，确认是否越过同一vcputc_lock超时；有新panic继续采首行/第二行/BT。
证据：observation、frames-callsite、timeout-defaults、control-tests、graphics-control-verification、final-code/final-binding。


## 2026-11-03 01:33:45：图形控制台单变量对照已授权刷入，设备结果待观察

用户明确授权“刷吧”。设备fastboot b8a81f12，product=uke、is-userspace=no；六项SHA256SUMS通过，
boot_a/vendor_boot_a/boot_b/vendor_boot_b 均为0x6000000且镜像尺寸合法。
2026-11-03T01:33:45+08:00 至 2026-11-03T01:33:49+08:00，四分区写入全部OKAY；set_active a后current-slot回读a，reboot OKAY。
**未写userdata**。boot eada6132… / vendor_boot92356130… / 最终内核f67f57f0… / CRC **C9630ACDF7253040**。
仅撤掉-uketextconsole，保留775 B其余参数、无15秒FD、独立panic屏/BT、锁与IRQ预算；内核/boot与上一版字节相同。
目前只确认刷入与重启，不认定控制台超时已越过或root/launchd/桌面通过。
证据：release的 `flash-result-20261103-013345.json` 与 `flash-20261103-013345.log`。
下一步：普通文字日志预期减少；观察是否越过vcputc_lock，新panic保留首行/第二行/BT，若前进收实际锁屏/桌面画面。


## 2026-11-03：根挂载后launchd exec遭安全策略拒绝；目标trust cache修正候选未刷

照片 IMG_20261103_013449.jpg，SHA9929dd2bc69e05e70ee2f558c2155edb2a7009df3cfddee83470cb2f3c542e0d：
`unexpected SIGKILL of launchd with reason -- namespace 9 code 0x8 description none`。
9=OS_REASON_EXEC，8=EXEC_EXIT_REASON_SECURITY_POLICY；不是namespace3坏代码页定义。
BT去PAC以return-4还原：bsdinit_task→load_init_program→load_init_program_at_path→__mac_execve→process_signature→psignal_internal。
本次已越过vcputc_lock控制台停点，根文件系统挂载和init executable装载/exec尝试已到达；
launchd尚未进入正常运行，不宣称SystemOS/Data具体挂载状态或桌面通过。
未观察具体MAC hook/errno；缺CS_KILLED字样和reason8/none支持ip_mac_return路径，但不是唯一可能原因的最终证据。

发现已刷输入错配：本地owner_trust_cache仍用61,456 B旧bundle037c77a1…，含3条历史owner条目及
原21H461 static da88460e…(2478条)/System d121ba89…(76条)，与17线原模块SHA一致。
24A446原版sbin/launchd从OS APFS只读提取，639,376 B / SHAc640246d…，
其CDHash **3d2ccf3da81ca5ac44d8f073b9bcb81147ecf8e2** 不在旧bundle中，存在于同IPSW的24A446 static模块。
原CodeDirectory150个4KiB代码页复算匹配；未改launchd或重签，本轮没有独立验证CMS链。

已用现有bundle工具选择原24A446 StaticTrustCache + Cryptex1,SystemTrustCache，
两IM4P与本地IPSW字节相同且SHA384匹配J820AP Customer Erase Install BuildManifest。
两个raw module分别04bdc992…(3547条)、599a9c9d…(130条)，完全逐字节复制，无新增自造hash。
新bundle88,308 B / SHA **dc655bcb2ddfaea214f9ec83659b3fc8d8b658bafac2b5a004fb4179a25c89bf**。
local build.trust_cache指向24A446-original-trust.bundle，board.payload.trust_cache_sha256固定该值；旧标准trust参数被真实CLI拒绝。
17线旧asset保持原样，安装Data seed独立保持。

实际DarwinBoot validator宿主测试：旧/新均格式合法，坏hash type拒绝。因此仅过结构门禁不足以确保目标身份。
最终FAT读出的trustbundle与target模块完全相同。其余payload只正常random-seed变化，source/final kernel、boot/无15秒FD、
775 B全部args、panic/BT、root policies、Data seed、userdata均不变；未关AMFI/MAC/代码签名检查。
新release `uke-port/releases/2026-11-03-ipados27-target-trust/`：
vendor_boot 2e5e5f4f… / 内核f67f57f0… / CRC仍 **C9630ACDF7253040**，**未刷，无userdata写**。
已验证原IM4P来源/目标CDHash/原签名代码页/loader实际格式函数/最终FAT和13区，
旧cache缺launchdhash是已证实输入缺陷，不能宣称它必为唯一拒绝原因或AMFI已放行。
下一次当次授权四分区测试，若仍namespace9/code8，继续查具体MAC钩子与errno，不先禁用策略。


## 2026-11-03 01:48:32：24A446目标trust四分区已授权刷入，launchd策略结果待观察

用户明确授权“刷吧”。bootloader fastboot b8a81f12，product=uke、is-userspace=no；
七项SHA256SUMS核验通过，四分区均0x6000000且镜像尺寸合法。
2026-11-03T01:48:32+08:00 至 2026-11-03T01:48:35+08:00，boot_a/vendor_boot_a/boot_b/vendor_boot_b全部OKAY，
set_active a后current-slot回读a，reboot OKAY。**未写userdata**。
boot eada6132… / vendor_boot2e5e5f4f… / 最终内核f67f57f0… / CRC **C9630ACDF7253040**。
载荷trust bundle dc655bcb…，两份原24A446模块3547+130条，含原launchd CDHash3d2ccf3d…；未改安全检查。
目前仅确认刷入与重启，不能据此认定exec策略放行、launchd用户态或桌面通过。
证据：release的 `flash-result-20261103-014832.json` 与 `flash-20261103-014832.log`。
下一步看是否越过launchd namespace9/code8；若新panic，拍完整首行和BT，若前进收日志及实际画面。


## 2026-11-03：CRC留屏与恢复XNU日志的需求复核

用户报告当前启动屏幕停留在加载CRC日志，无法判断内核是否继续运行；没有新的panic/用户态通过证据。
直接复核活动DarwinBoot和xnu-work：CRC由DarwinEnter之前的DiagDraw画出；XNU vcputc_options在gc_enabled时取得vcputc_lock，gc_putchar可能滚屏；无串口时PE_init_kprintf选console_write_unbuffered。故删除加载器日志不能解决XNU自己的文字锁争用，去掉-uketextconsole后的图形门控又会使普通日志不可见，旧CRC像素不能作为卡死位置。
后续需恢复真实启动信息并先测绘制/滚屏持锁成本，再降噪或优化；保留独立panic/BT，不放宽锁阈值。本轮仅解释与记录，无代码/参数/镜像/设备变更。详细见根docs/status/ipados27-console-lock-2026-11-03.md首节。


## 2026-11-03：缓冲kprintf候选已授权刷入，日志稳定性待实机反馈

正常kprintf从IRQ-masked直绘改为existing console_ring trylock入队、EARLY_BOOT/ONCE任务后台flush；忙/满仅丢屏幕副本并计数，os_log调用保留。恢复-uketextconsole，参数811B；不改timeout/AMFI/MAC/帧缓冲映射/userdata。正式patch replay另移除缓存中的上次panic重放调用，当前独立panic/6BT保留。
主机负对照/并发/满队列/绕环/IRQ状态、QEMU竞态/回溯、最终机器码/POST HMAC/FAT/264分支/2missing24stubbed门禁通过；都不是实机稳定性证明。
2026-11-03T02:32:28.564166+08:00–2026-11-03T02:32:32.289475+08:00获当次“刷吧”授权，6SHA/四96MiB检查通过，四分区全OKAY，activea回读/rebootOKAY，未写userdata。
release `uke-port/releases/2026-11-03-ipados27-deferred-console/`，boot eada6132… / vendor_boot 5b78fef1… / final 21bf53d8… / CRC **A39DA0801DCB37ED**。
待观察CRC页后真实XNU日志是否持续推进；新panic拍首行/BT，无panic停住拍末行。launchd/桌面仍未验收。
详细：根 `docs/status/ipados27-deferred-console-2026-11-03.md`；flash-result-20261103-023228.json及匹配log。


## 2026-11-03 02:33:34：实机越过exec策略停点，到launchd的dyld Ignition初始化

照片 IMG_20261103_023334.jpg SHA36588cf8ff8ef8aa768ecc4c18cc43d1968cd4a5a542523d6421356f401c7a71，对应已刷final21bf53d8…/vendor_boot5b78fef1…/预期CRC A39DA0801DCB37ED（照片未拍加载CRC）。
可见SystemOS ready、published OS、OS ready、BSD root disk5s1及mount-complete；随后launchd(pid1)的Darwin Ignition/libignition64参数输出，level0x5、force root0、halt n/a。用户报告之后只TCH心跳且触控正常，本观察期无新panic。
这证明可见日志恢复并在本次观察期越过旧文字锁/exec9/8停点，launchd进程已执行dyld/libignition用户态代码；不等同launchd LC_MAIN或服务/桌面成功。SystemOS ready不等同其用户空间graft完成，Data挂载仍无证据。
只读提取24A446原dyld，SHAab9887ed96c9c08ad68d4c6a5c27e8289fb0c9e55ec3e134547292a1ec579608；真实调用顺序arguments_dump→configuration_init→configuration_dump。configuration_init首先查询security.mac.img4.ignition_blob，已刷AppleImage4该handler绑定到lck_mtx_gate_wait及后续IOSleep就绪轮询，故为下一项待观测候选，尚未证实现场阻塞在哪条指令。不使用17线skipIgnition经验直接改27安全流程。
审计：uke-port/fixes/2026-11-03-ignition-startup/observation.json、dyld反汇编及handler-kernel-calls.json。照片还见swap errno30（EROFS），不能归为主机Btrfs/ENOSPC。


## 2026-11-03：Ignition查询诊断候选已备（未刷）

基于02:33:34实机照片，24A446 dyld调用链已核到arguments_dump后的configuration_init/ignition_blob查询；实际等待点仍未知。
TCH仅IOLog心跳降为10秒、保留采样/发布，首次HID状态打印修正；只对PID1前8次ignition_blob查询加进入/分派/返回诊断，原MAC/PAC/结果不变。
正式kernel/payload、最终832B args/FAT/POST/PAC/264分支、原2missing24stubbed门禁及QEMU抢panic/6BT通过；Image4/AMFI/CoreTrust/sandbox执行段与已刷版完全一致。
release ignition-trace：vendor_bootb88e1114… / finald8a0ee8b… / CRC **21B43B437E708095**；boot仍eada6132…，未写userdata、未刷。
下一次授权后看UKE IGN enter/dispatch/returned及后续配置，不先改skipIgnition。详见根docs/status/ipados27-ignition-startup-2026-11-03.md。


## 2026-11-03 02:45:29：Ignition诊断版已获当次授权刷入

用户明确授权“刷吧”。2026-11-03T02:45:29.022723+08:00 至 2026-11-03T02:45:32.860695+08:00，fastboot b8a81f12/productuke/is-userspaceno；6项SHA256SUMS和四个0x6000000分区尺寸通过。
boot_a/vendor_boot_a/boot_b/vendor_boot_b全部OKAY；set_active a、current-slot=a、reboot OKAY。**未写userdata**。
boot eada61328aebb0aab5e86f01cc3528a4025445e93c7f08a592be7b89889346dd / vendor_boot b88e11147cf57fcb953fcf8a579b72890a0109aecbfff8afd80b8fc23fa21f7d / final d8a0ee8b4474866207215fe41c003aed3039c9f7672b4450d7144b9bb77f6b97 / CRC **21B43B437E708095**。
保留缓冲kprintf、独立panic/6BT、原24A446trust/root/Data；TCH仅日志限频10秒，有限PID1 ignition_blob enter/MAC/dispatch/returned诊断，未改原安全组件执行段/策略/超时。
目前只证明写入与重启成功，实际sysctl等待位置、心跳限频和后续用户态/桌面仍待设备反馈。
证据：release `flash-result-20261103-024529.json` 与 `flash-20261103-024529.log`。下一步拍UKE IGN行与后续配置；若panic拍首行/BT。


## 2026-11-03 02:46:47：Ignition查询已进入原版handler，未观察到返回

照片024605 SHA cc4a2c916f5f9daa67d7b60d4018412e958541106cac3dc5553d2cb6ffb885a4；照片024647 SHA ebca54b4dc1a298985644ead62a68efb76fdb0f32f66cac51dacae221742560b，对应已刷ignition-trace finald8a0ee8b/vendor_bootb88e1114。
图1显示AppleImage4 root power domain not yet available:19等，用户确认之后继续刷；图2到pid1 libignition后明确有 `UKE IGN: ignition_blob enter oldlen=132 newlen=0`、`dispatch`，之后TCH心跳f3/f9/f16及首次HID stage3/load0/match0/res1/meta0。未见returned/MAC rejected，用户报告后续只有TCH。本观察期未报新panic。
这将重点从前置MAC/全局sysctl锁推进到原Image4 handler内部；尚不能区分gate wait和ready标志的IOSleep轮询，也不能由缺一行日志直接当作线程快照（屏幕副本best effort）。图1并非永久停点，root power警告是线索而非已证实根因。
新窄范围等待类型诊断在构建中，保留原policy/返回/等待规则，不刷入未授权新镜像。证据uke-port/fixes/2026-11-03-ignition-handler/observation.json。


## 2026-11-03：Ignition调用区间已定位，等待类型候选未刷

图2明确enter132/dispatch，未见returned；TCH继续，本观察期无新panic。图1停顿后继续，不当最终卡点；具体gate/ready等待未区分，屏幕best-effort不能代替线程快照。
新ignition-wait仅在登记的sysctl线程标记gate try/result/wait和IOSleep，每种最多2条，原返回/参数/分支保持；安全组件执行段和原trust/root/Data/832Bargs/boot相同。
主机hook控制/最终机器码/FAT/PAC/POST/原2missing24stubbed/264分支及QEMU竞态6BT通过。vendor_boot222baafa… / final72fd03d6… / CRC **084340DD14DE8E90**；未刷，未写userdata。
下一次授权后看gate wait deadline0或IOSleep ms1；不得从原root power警告直接猜根因。详细：根docs/status/ipados27-ignition-handler-2026-11-03.md。


## 2026-11-03 02:57:55：等待类型诊断已刷；用户授权后续做好直接刷

用户“刷吧 你做完直接刷就行”授权本次刷写，并允许本持续调试任务后续通过检查的boot/vendor_boot候选直接刷，无需逐版重复确认。仍保留设备身份/哈希/分区尺寸检查、四分区/A槽/回读/reboot和证据留档；本授权不扩展为任意userdata擦写。
2026-11-03T02:57:55.689449+08:00 至 2026-11-03T02:57:59.421523+08:00：fastbootb8a81f12/productuke/is-userspaceno，6SHA/四0x6000000尺寸通过；boot_a/vendor_boot_a/boot_b/vendor_boot_b全部OKAY；set_active a、current-slot=a、rebootOKAY。**未写userdata**。
boot eada6132… / vendor_boot222baafa18701c8b2f18d41702acef627d9bd9b8339151169cb3963d5823cc22 / final72fd03d68bc0771cf267bace592f334e32e36330d82f9874a51e7566596d55da / CRC **084340DD14DE8E90**。
保留原gate返回/等待规则、IOSleep参数和安全组件执行段，仅对登记的Ignition查询线程做有限gate/sleep标记。当前只确认刷入/重启，实际等待类型与launchd LC_MAIN/服务/桌面待观察。
证据：release `flash-result-20261103-025755.json` 与 `flash-20261103-025755.log`。下一步拍全部UKE IGN gate try/result/wait或IOSleep ms行；有新panic拍首行/BT。


## 2026-11-03 02:59:22：gate等待路径实机确认

照片IMG_20261103_025922.jpg SHA06ae3ff5176eea8bc785af44aba8676599bfcb77b21036966fd0faec104d4798，对应已刷final72fd03d6/vendor_boot222baafa。明确enter132/dispatch→gate try-close0→gate result5→gate wait deadline0，随后TCHf3/f9/f16/f23；未见IOSleep或returned。源码gate_try_close的5为KERN_FAILURE，说明当时gate已由另一线程关闭；deadline0不设截止时间。已确认走gate_wait路径，不再把ready轮询当同等已观测事实；当前持有者/开gate服务的实机状态仍未知。
静态复核24A446原Image4：唯一独立开gate路径由AppleImage4的IOService::start调用，start此前的finalize会waitForMatchingService(IOAESAccelerator, UINT64_MAX)。原personality匹配IOPlatformDevice/defaults并在最终载荷保留；SoftwareAES目前仅17线ABI资格，27线sourcecrypto构建也有明确使用门禁，故不直接启用旧provider。正在准备不改行为的服务start/AES等待记录，检查通过后按用户持续授权直接刷。
证据：uke-port/fixes/2026-11-03-ignition-gate/照片、observation、gate-xrefs、open-gate/open-caller及匹配元数据。


## 2026-11-03：实机gate_wait确认，Image4服务/AES依赖诊断已刷

025922照片SHA06ae3ff5…明确gate result5、gate wait deadline0，之后TCH继续，未见IOSleep。原Image4服务start→finalize等待IOAESAccelerator(UINT64_MAX)→独立gate open链路经最终vtable/认证槽核实；实际AES等待尚未观测。SoftwareAES只具17线ABI资格，本轮未启用/修改它。
新image4-provider只观测真实service start/AES wait委托和gate holder快照，原timeout/返回/安全组件保持；832Bargs/trust/root/Data/boot不变。正式构建/原2missing24stubbed/264分支/最终机器码/PAC/POST/FAT及QEMU双panic6BT通过。
持续授权下2026-11-03T03:10:52.557948+08:00–2026-11-03T03:10:54.132000+08:00刷四分区全OKAY、activea回读/rebootOKAY，6SHA/四96MiB检查通过，未写userdata。
vendor_boot8f0c2735… / finala0d03581… / CRC **6EA6C56C3254667F**；实际服务/AES等待及launchd LC_MAIN/桌面待反馈。
下一步拍全部UKE IMG4 service start/wait IOAESAccelerator/returned与UKE IGN gate owner。详见根docs/status/ipados27-image4-provider-2026-11-03.md。


## 2026-11-03 04:13:22：Image4启动集成修复已刷

用户要求直接修复，停止逐层观测版循环。本版接入未改完整SoftwareAES，新增24A446资格化builder：0x120/181PAC槽、原构造/继承BTI绑定全部核实，修正getAlgorithmName在IOCryptoAccelerator的声明归属。KAT成功才发布真实provider；匹配平台根避免IOBSD时序环；只在真实defaults nub缺失时发布它，再正常OSKext重匹配原Image4。sourcecrypto/原安全组件执行段/参数trust/root/Data保持，不改gate/策略/返回。
完整算法与启动序列宿主正负控制、正式build、最终PAC/POST/FAT/原2missing24stubbed及QEMU抢panic6BT通过；不代表设备KAT/启动已成功。
2026-11-03T04:13:22.221558+08:00–2026-11-03T04:13:23.837870+08:00持续授权刷四分区全OKAY、activea回读/reboot确认，7SHA/四96MiB检查通过，无userdata写。
release image4-repair：vendor_bootd314dc6f… / finalafdc535b… / CRC **B42AAD6E20492AE3**。实机修复结果待反馈；下一步看是否越过gate、后续启动画面，若失败拍末屏。
详见根docs/status/ipados27-image4-repair-2026-11-03.md，审计uke-port/fixes/2026-11-03-image4-repair/。

## 2026-11-03 AES同步启动修正（已刷，设备启动结果待反馈）

用户转述上版panic SoftwareAES unavailable kat=0/interface=0/registered=0，SourceStorage.cpp76；BT首帧fffffff00b62d8a4。
这表明5秒异步匹配路径没有得到已发布AES服务，也未观察到KAT进度；不能据此认定算法失败。原驱动start在crypto/IOService前置检查失败时也可能保持KAT0，旧报错没有区分这些情况。
修正为实际factory→init→attach(real platform root)→start，同本板sourceUFS/RAM启动集成方式；驱动start内完整KAT仍必须成功才registerService。保留静态服务引用，取消新加catalogue personality和同步wait，不重复匹配/启动AES。新的失败文本区分init/attach/start与publication校验。
SoftwareAES算法/已资格化24A446 ABI、原Image4匹配/defaults修复、sourcecrypto及安全策略保持；args832B/trust/root/Data/boot不变，没有userdata写。

用户新要求每轮最多一个自测或不做，其余真机验证。本轮只运行正式kernel/payload及既有prelink门禁（missing2/stubbed24），再做一次最终镜像检查：真实FAT分片、容器/尺寸、保留输入、boot一致及SourceStorage确实调用factory；没有重跑宿主算法/并发/QEMU。驱动原运行时KAT保留，docs13区记录检查仍按工作区要求执行。

release `uke-port/releases/2026-11-03-ipados27-aes-direct-start/`：
- boot `eada61328aebb0aab5e86f01cc3528a4025445e93c7f08a592be7b89889346dd`。
- vendor_boot `34762ebe563f0af049456020d87902e7e86c3d7b4040621ebc0c7230eb5c6512`。
- final kernel `083f31c66abc57f0cb7447f8ac33e36e10682a150de37f041ec4d1c3beebb516`，CRC **36F27F36B4354A08**。
- 当前状态：**已刷，设备启动结果待反馈**；没有设备KAT或越过gate的成功证据。

持续授权下2026-11-03T04:19:57.819121+08:00至2026-11-03T04:19:59.471573+08:00，fastboot b8a81f12/productuke/is-userspaceno，5SHA/四96MiB尺寸检查通过；四分区全OKAY，activea回读/rebootOKAY，未写userdata。证据flash-result-20261103-041957.json及匹配log。

接续：看真实SoftwareAES ready/KAT通过标记和是否越过原gate。若失败拍末屏，按具体init/attach/start错误继续修复。回退image4-repair四分区（vendor_bootd314dc6f…），保留userdata。


## 2026-11-03 04:21:11：gate已通过，进入ready轮询；发现第二Image4服务personality被剥掉

照片IMG_20261103_042111.jpg SHA4622614d7e6539859a8d9fbdef8cd03a1c74096b64e809d18a128586efeeb73b。已刷aes-direct-start的gate result从5变为0，随后IOSleep ms1两条及TCH继续；未见returned。证实已越过旧gate阻塞并进入原handler ready轮询，不能写成整个Ignition已完成。
实际原版ready writer@fffffff009023b4c设置context+0x10，认证回调指针@fffffff008479bb0属于com.apple.security.Image4。该新组件在已刷载荷中为代码依赖，其原defaults personality被prelink明确列入personalities_dropped；roots只选了AppleImage4，未选Image4。
现按板级roots补com.apple.security.Image4，保留其完全原始personality；正常OSKextLoad请求两原服务匹配。不手工写flag、gate或原函数，不改算法/安全结果。修复正在构建，按用户要求仅构建与一次最终镜像检查，之后直接刷。
证据：uke-port/fixes/2026-11-03-image4-ready/observation.json、ready writer与caller静态反汇编；静态回调来源不能单独当作实际服务未启动的线程快照，但personality剥除为已证实输入缺陷。


## 2026-11-03 04:26:00：Image4 ready匹配入口修复已刷

042111照片gate result0→IOSleep1，旧gate阻塞已越过。原ready回调由第二组件com.apple.security.Image4调用，其personality此前作为dependency被剥除。board roots已增加此原组件，保留完全原始defaults personality；SourceStorage正常OSKext请求两原服务匹配，不改flag/gate/策略。
正式构建/原2missing24stubbed门禁和一次最终镜像检查通过，无宿主/QEMU重跑。参数/trust/root/Data/AES/boot保持。
2026-11-03T04:26:00.075217+08:00–2026-11-03T04:26:03.918685+08:00持续授权刷四分区全OKAY、A槽回读/rebootOKAY，5SHA/四96MiB检查通过，未写userdata。vendor_bootf8ef8b1b… / final8dfa3c9c… / CRC **6AD2174F72C7F4C2**；设备ready返回与后续启动结果待反馈。
接续根docs/status/ipados27-image4-ready-2026-11-03.md；审计uke-port/fixes/2026-11-03-image4-ready/。


## 2026-11-03：原Image4已启动，magazine初始化返回内部7

用户转述Image4 unable to initialize magazine:7 @IOKit.cpp45，BT首fffffff00a9f13a0、次raw55d7c7f00a9e272c。按return-4核最终image4-ready，错误来自原Image4::start首次init@a9eeea0（不是后续nonce slot循环）。该路径读krn.zine缺失返回内部7，随后重建依赖当前boot manifest hash；当前chosen无该属性。7不能直接当普通errno7或算法失败。
修复采用项目现有VerifiedRoot.OwnerBootSet.v1规则：完整OS/SystemOS已签名策略各SHA384，角色1/2分隔加带NUL域再SHA384。明确owner-domain标记，不称Apple票据。board chosen携带48B摘要和域；SourceRootPolicies仅在实际RSA/header验证后比对精确headerSHA256和身份/域，不取消任何验签。
摘要148316f0bb4484b19cda323503ffcb454fcc96d089087cd1f140725337c7cb8c435b86e560c193df76821ca121e262e9。仍需实机验证它是否解除本次初始化失败，不能从已知缺项直接宣称唯一根因。构建中，保持用户最小测试偏好并按持续授权直接刷。


## 2026-11-03 04:36:14：owner启动集合身份修复已刷

用户转述原Image4 magazine初始化内部7；静态read helper属性不存在返回7，重建依赖当前boot hash；chosen缺该属性是已证实输入缺项，不称已证实唯一根因。
按已有OwnerBootSet.v1规则由完整已验证OS/SystemOS签名策略生成身份148316f0…，board显式owner域tag；运行时在原RSA验证后绑定两精确signed header和identity/domain，不造Apple票据/krn.zine、不改安全结果。
正式构建/原门禁与一次最终镜像检查通过，无组测。2026-11-03T04:36:14.262323+08:00–2026-11-03T04:36:18.013358+08:00持续授权刷四分区全OKAY/A槽回读/rebootOKAY，无userdata写。vendor_boote7714e34… / final5eb093cb… / CRC **3A7F0A19B90C68FD**。
原magazine/NVRAM实际写入、Ignition返回/桌面待反馈。详见根docs/status/ipados27-image4-magazine-2026-11-03.md。


## 2026-11-03：原magazine加密要求特殊key2212，改接已有认证owner启动路径

用户转述encryption failed:0xfffffffe00002c7，BT首9036ab4、次raw59eebbf009024ea8，后续902d904/9017724/a9f0ec8/a9e2644。实际原AppleImage4请求bits128、selector0x8a4=2212、operation0，keybytes未提供；其descriptor performAES调用返回kIOReturnUnsupported0xe00002c7。完整SoftwareAES按设计不支持此特殊key，没有算法失败/PAC错误证据。
不会用全零/随机/冒充硬件的key映射selector或返回假成功。现接通项目已有owner_cache_prepare：实际只读OS根与SystemOS media独立认证、真实挂载后才设置dyld_flags bit3。原24A446 dyld CacheFinder在2217c测试bit3，置位跳22244，未置位才在221bc调用ignite。该选择在真正卷验证后发布，不是直接bootarg。
因此不再匹配仅用于Apple硬件Ignition的第二Image4服务（代码依赖/原AppleImage4安全policy保留），避免它在owner准备前抢跑请求不存在的特殊key。SystemOS用真实24A446 UUID，保留现有seed并启用已有Data挂载。已有owner boot metadata publisher在两真实media proof之后，只对精确相同的owner域+digest接受早期身份重校验，其他已有身份仍拒绝覆盖。
新owner-startup构建中，仍只正式构建/既有门禁和一次最终镜像检查，按持续授权直接刷。代码签名/MAC/块验签、原AES拒绝硬件key均保持；设备卷挂载/后续userland尚未证明。


## 2026-11-03 04:44:11：原特殊key2212 unsupported，认证owner启动已刷

原AppleImage4 performAES请求selector2212/128bits返回unsupported0xe00002c7；不伪造key/成功。原24A446 dyld2217c bit3选择已核；启用既有owner实际OS/SystemOS认证+真实挂载后才设选择，并挂载现有seedData，失败拒绝不变。
第二Image4硬件准备服务不自动匹配（原代码/policy保留），早期owner身份只在真实media proof重算得到相同域+digest后幂等接受。sourcecrypto/原签名检查/AES特殊key拒绝/832Bargs/trust/root/Data seed/boot保持。
正式构建/原门禁+一次镜像检查通过。2026-11-03T04:44:11.136587+08:00–2026-11-03T04:44:14.826523+08:00持续授权刷四分区全OKAY/A槽回读/rebootOKAY，无host userdata刷写；正常启动的可写Data可能持久化。
vendor_bootc62d8274… / finalcab65c70… / CRC **1E6629CA8DD40FEA**。实机实际挂载/分支/后续userland结果待反馈；根docs/status/ipados27-owner-startup-2026-11-03.md。

## 最新进展（2026-11-03，SystemOS 默认缓存路径修复候选）

- 最新设备 panic：initproc namespace6/subcode1，原 launchd 缺 /usr/lib/libSystem.B.dylib，dyld 报 no dyld cache；已越过前次特殊 key 加密停点，桌面未通过。
- 原 SystemOS 确有 arm64e 共享缓存及分片。启用既有认证只读 SystemOS union 根组合，缓存位于默认 /System/Library/Caches/com.apple.dyld；关闭旧 preboot 环境覆盖，保留实际双卷认证与 Data 挂载。具体上一版 cache 拒绝原因未独立证实。
- 正式构建/原 missing2-stubbed24 门禁、一次最终镜像检查及 porting-record13区通过；vendor_boot236310c4… / final32c7dd04… / CRC **B733EBAAFB513F18**。镜像完成，当前等 fastboot，尚未刷本版。
- 接续：[SystemOS 默认缓存路径修复](ipados27-systemos-union-2026-11-03.md)；审计 uke-port/fixes/2026-11-03-systemos-union/。实际 cache/userland/桌面待验。


2026-11-03 07:53:45–07:56:45 检测目标180秒未枚举，fastboot/adb均为空；部署未执行任何flash命令，设备分区未改。本候选保持未刷，等待用户把设备切回bootloader fastboot。证据：release/flash-result-20261103-075345.json（commands=[]）。

## 2026-11-03 SystemOS union 四分区已刷

用户本次明确授权后，2026-11-03T07:58:03.528203+08:00 至 2026-11-03T07:58:05.598844+08:00，目标 b8a81f12/product uke/is-userspace no；5SHA/四96MiB检查通过，boot_a/vendor_boot_a/boot_b/vendor_boot_b 全OKAY，set_active a/槽位回读/reboot OKAY。vendor_boot236310c4…、final32c7dd04…、CRC **B733EBAAFB513F18**。无host userdata刷写；运行期Data可能正常持久化。实际union/cache加载、用户态/桌面仍未验收。证据：uke-port/releases/2026-11-03-ipados27-systemos-union/flash-result-20261103-075803.json 及匹配log。

## 最新进展（2026-11-03 08:04:30，GOP nub父对象修复已刷）

- 075953照片为IA PAC failure：PCb4fb804=IOPlatformDevice::compareName，LRb4858f4=IORegistryEntry::compareNames。现场x4槽7e50700对应IOResources vtable+0x580，实际OSMetaClass::release/schema3a87，却以平台专家compareNubName/schema402c调用；与照片x16一致。
- 修LegacyDisplayProbe的GOP IOPlatformDevice父对象：由IOResources改为动态类型校验的真实IOPlatformExpert。保持PAC与错误拒绝、实际GOP/DT资源、union/OS-SystemOS授权、Data/trust/参数；独立普通IOService handoff父对象保留。
- 正式构建/原missing2-stubbed24门禁、一次镜像检查/porting13区通过；08:04:30四分区全OKAY、A槽回读/rebootOKAY。vendor_boote7599d5d… / finalf3ae0f8e… / CRC **C2349F0C80CEF895**；无host userdata刷写，运行期Data可能持久化。实际PAC消失/cache/用户态/桌面待验，不能仅因新panic称共享缓存已通过。
- 接续：[GOP nub父对象修复](ipados27-gop-platform-parent-2026-11-03.md)，审计 uke-port/fixes/2026-11-03-gop-platform-parent/。


## 2026-11-03 08:04:30 GOP nub父对象修复刷写

正式构建、missing2/stubbed24门禁与一次镜像检查通过：最终late_probe实际调用getPlatform，getResourceService调用由2处降为1处（保留普通IOService可选GPU handoff）。porting-record13区通过；未重跑宿主/QEMU套件。

- boot `eada61328aebb0aab5e86f01cc3528a4025445e93c7f08a592be7b89889346dd`。
- vendor_boot `e7599d5dfc44d3fdc71693109b60b6d5e2f3dda05a826251ddab8c80106fff69`。
- final kernel `f3ae0f8ee29fb09f397b11bd27dd8b4984f0dd423bb9ace929c9024fa1238181`；CRC **C2349F0C80CEF895**。

持续授权下 2026-11-03T08:04:30.558827+08:00 至 2026-11-03T08:04:32.630071+08:00，序列号b8a81f12/productuke/is-userspaceno，5SHA/四96MiB检查通过，四分区全OKAY、A槽回读/rebootOKAY。无host userdata刷写；运行期Data可能正常持久化。真机PAC是否消失及缓存/用户态/桌面仍未验收。证据：`uke-port/releases/2026-11-03-ipados27-gop-platform-parent/flash-result-20261103-080430.json` 及匹配log。

## 最新进展（2026-11-03 08:22:46，原生Data持久化与Block描述符修复已刷）

- 080604照片已见原launchd Early boot complete，越过libSystem停点；桌面未通过。随后disk2/disk2s1写未就绪，以及Block_copy+0x34对带签名descriptor直接解引用data abort（PCb464234/原descriptor08346a20）。
- Data问题对应200GiB SeedDisk未启持久化。recovery只读核既有窗口：header/CRC/52428800页和现用seed完全一致、generation2、1069022已写页（约4.08GiB），无需初始化。仅加sheng-data-persist=2，无reset；原RamBlock代码/seed保持，无临时RAM扩展。
- Linux arm64e编译启原cc1 Block descriptor签名特性，C/C++及archives统一重建；最终Block_copy含0xc0bb/AUTDA/失败BRK。原安全结果/双卷授权/union/trust保持。正式构建、原2missing/24stubbed、一次镜像检查及porting13区通过。
- 08:22:46四分区全OKAY、A槽回读/rebootOKAY；vendor_boot8d0ef78d… / final2e314524… / CRC **256FA2C2F7D00ED3**。无host userdata刷写/初始化；设备后续正常Data写入会持久化，实际读写/跨重启、panic消失/桌面待真机。
- 接续：[Block描述符与持久化Data](ipados27-blocks-persistent-data-2026-11-03.md)；审计 uke-port/fixes/2026-11-03-blocks-ram-data/。

部署证据：uke-port/releases/2026-11-03-ipados27-blocks-persistent-data/flash-result-20261103-082246.json。

## 最新进展（2026-11-03 08:29:06，arm64e旧客户端虚表插桩退出版已刷）

- 082423照片PCb64eadc=memcpy+0x2c，LRb5ff428=native_legacy_client_observed+0xd0，x1/far为带PAC虚表指针8dbb3ff0083645f0，长度0x800。旧插桩裸拷client vtable并改168/175槽，搬表后的address-diversified槽也不能普通复制。
- 把该旧shadow表/selector适配包装限定非PAC目标；27线保留原newUserClient/原方法派发与typed状态观察，不改PAC/schema。真实GOP backend、Data持久化2/无reset、Block descriptor认证/双卷授权/union/trust/参数保持。
- 正式构建/原2missing24stubbed、一次最终镜像检查及porting13区通过。08:29:06四分区全OKAY、A槽回读/rebootOKAY；vendor_boote928ad23… / final5dd8f9ac… / CRC **C0A17571D41F547C**。无host userdata刷写，设备正常Data持久写入保留；客户端/画面/桌面和持久化实机验收待定。
- 接续：[客户端虚表插桩修复](ipados27-client-vtable-2026-11-03.md)，审计 uke-port/fixes/2026-11-03-client-vtable/。


## 08:29:06部署记录

- boot `eada61328aebb0aab5e86f01cc3528a4025445e93c7f08a592be7b89889346dd`。
- vendor_boot `e928ad231db930efc8a53fe31c68a65f287a617663161e0ad6854f54b4e7b191`。
- final kernel `5dd8f9acd633d68b3c35f2bb14be0940d3f6d261ec999a64946e5bbf3bfdcda2`；CRC **C0A17571D41F547C**。

持续授权下2026-11-03T08:29:06.939524+08:00至2026-11-03T08:29:09.063095+08:00，目标b8a81f12/productuke/is-userspaceno；5SHA/四96MiB检查通过，四分区全OKAY、A槽回读/rebootOKAY。无host userdata刷写；原生持久化2/无reset保留，设备运行期会正常持久写入。正式构建/原2missing24stubbed与一次最终镜像检查通过：两旧wrapper符号消失，实际observer无表分配，Block_copy的0xc0bb/AUTDA保留。porting13区通过，无宿主/QEMU重跑。

本轮只是定位并移除不兼容插桩；客户端创建/原方法调用、Data恢复与跨重启持久性、画面/桌面尚待实机。证据：uke-port/releases/2026-11-03-ipados27-client-vtable/flash-result-20261103-082906.json及匹配log。


## 最新进展（2026-11-03 09:43:30，补回27线userdata尾部日志，已刷）

- 已刷client-vtable漏了sheng-log=1且未链接UkeLastLog。TWRP只读尾部32MiB得到708条有效旧GPU记录，已留档并排除，不能当本轮27日志。
- 上轮Data快照现已宿主只读挂载，筛出14份明确24A446报告：chronod/cameracaptured/contextstored在QuartzCore query_displays中abort；datamigrator采样reason为unsupported screen class。SpringBoard的iconservices报告是simulated fault，不能当作退出证明；具体显示契约仍待核。设备无panic/无GUI接管为用户反馈。
- source-storage接回设备现有UkeLastLog/CrashLogProbe，启用sheng-log=1/sheng-crashlog=1（参数882B）；640KiB报告缓冲改worker分配以避固定__DATA边界。panic屏仅在已有BSS原子claim路径移除不用的像素marker，六帧BT/清屏/暂存保持，大小0xa5b B。Data persist2/无reset、trust/双卷授权/union、原显示客户端保持。
- 正式构建/原missing2-stubbed24精确集合及最终镜像检查通过。09:43:30四分区全OKAY，A槽回读/rebootOKAY；vendor_boot a0e126c0… / final436c6c82… / CRC **A25A761D7FCC72B3**。host未写userdata；正常运行期会写尾部日志和既有Data。日志实际落盘与桌面仍未验收。
- 接续：[27线持久日志与Data报告](ipados27-persistent-log-2026-11-03.md)；release uke-port/releases/2026-11-03-ipados27-persistent-log/；读日志前与旧环逐slot对比界定本轮。

审计与完整部署证据：根docs/status/ipados27-persistent-log-2026-11-03.md；release flash-result-20261103-094330.json。


## 2026-11-03 09:51：27线userdata尾部日志实机回读通过

- 09:43:30 persistent-log（final436c6c82… / vendor_boot a0e126c0… / CRCA25A761D7FCC72B3）启动后，用户回fastboot。原厂四分区+A槽回读/reboot recovery成功；ADB libusb默认后端出现closed/reset，重启server并用ADB_LIBUSB=0后shell恢复。
- 09:51:14按live userdata=471908312个512B扇区，只读末尾32MiB，SHA e5bde46c78c26de2d9dffb14dddc4c4afcdcc1b648b21b9229e33e6a3c5ede2b。与09:32旧环逐slot比较，仅slot0..136改变；137条新记录seq0..136全部magic/CRC通过，其余1911槽逐字节相同。只解码新slot，不混入slot137之后的旧GPU记录。
- 本轮log49.664755s明确UkeLastLog打开绝对LBA61941755/8192块，末行321.876064s。日志写入/跨重启到TWRP回读实机通过，桌面仍未通过。原始ukelog-20261103-095114-persistent-log.bin与同tag clean.log归reference/logs/{ukelog,boot}/；证据uke-port/fixes/2026-11-03-persistent-log-readback/readback.json。
- 49.730875s原始日志有uke-fb默认表面status0/valid1、3200x2136/stride12800/27340800B。后续observer的surface/temp=0来自旧17线0x1618/0x1620，27 fixture default_surface=0x1b58，不是表面未创建证明。优先追真实用户态显示服务契约。
- 设备现为原厂boot/vendor_boot四分区+TWRP；本轮host未写userdata。正常Data与日志写入是此前XNU运行期行为。


## 最新候选（2026-11-03，显示调用边界诊断，已出镜像未刷）

- 本次boot20538962…的统一日志确认backboardd窗口服务器初始化完成、主屏3200x2136/scale2，SpringBoard完成启动登记；无图形接管不能再当作两服务完全未启动。137条新内核记录无swap validation/transfer，具体提交失败点仍未明确。
- 修27线client-open观察错误：旧0x1618/0x1620读取退出arm64e，采用fixture生成的default0x1b58/current0x1a40/pending0x1a48/vsync0x1ab8/dims0x1bf0。新XNU调用入口的有界observer保留原IOUserClient2022/IPC/PAC/结果，不重写vtable，不加旧swap padding。
- 正式kernel/payload构建、原missing2/stubbed24精确集合与最终镜像检查通过。仅加uke-display-trace=1，902B参数；日志/Data persist2无reset/trust/双根union保持。vendor_boot d9bd96b2… / finald19c5e71… / CRC **804CD0D45D379053**。
- 用户明确要求“最后出镜像就行先不刷”：候选未刷、未重启、未host写userdata；设备留TWRP。此版修正误读和观察缺口，呈现故障未证实修复，桌面仍待验。
- 接续：[显示提交边界与候选](ipados27-display-submit-2026-11-03.md)；release uke-port/releases/2026-11-03-ipados27-display-submit/。

审计：根docs/status/ipados27-display-submit-2026-11-03.md及release/candidate-status.json；本轮不部署。


## 2026-11-03 12:59:27：display-submit已获当次授权刷入

用户新指令“刷吧”覆盖此前“先出镜像不刷”的候选阶段约束。刷前5个产物SHA、porting13区通过；从TWRP用adb reboot bootloader，核目标b8a81f12/productuke/is-userspaceno及四个96MiB分区界限。boot_a/vendor_boot_a/boot_b/vendor_boot_b均OKAY，set_active a、current-slot a回读及rebootOKAY。

- 时间：2026-11-03T12:59:27.501465+08:00 至 2026-11-03T12:59:34.310485+08:00。
- boot eada61328aebb0aab5e86f01cc3528a4025445e93c7f08a592be7b89889346dd。
- vendor_boot d9bd96b253d77a0c0611514911dd4548bd2d005587b7f606adf0dedd801c1010。
- final d19c5e71ee194eaf9ccd0429a67eb85b15c4269194061a3acfa4063f9922ca99；CRC **804CD0D45D379053**。
- 证据：uke-port/releases/2026-11-03-ipados27-display-submit/flash-result-20261103-125927.json及匹配flash log，进bootloader记录在审计目录bootloader-transition.json。
- host没有刷写/初始化userdata；XNU正常运行时保留原Data持久化及尾部日志写入。刷写/重启成功不等于出图成功，显示修复仍待设备观察。

后续：确认CRC页，若新panic拍完整首行/BT；若继续无图形，让它运行至少三分钟后回fastboot取日志。按09:51旧dump逐slot排除旧记录，核UKE DISP selector3/4/5/6/72的enter/return/result与swap/transfer。无需逐行拍屏，完整日志从TWRP读取。


## 2026-11-03：SwapEnd原调用成功但未进后端，初始requested修复候选

- 13:06:33 TWRP只读回读新环123条（slot0..122，全部CRC有效），其余1925槽与09:51相同排除；raw SHA d8e8331b…，日志到293.326847s。设备现为原厂四分区+TWRP，host未写userdata。
- backboardd两次SwapBegin/1760B SwapEnd均result0，SwapWait和默认表面Mach port0x13513返回成功；default表面存在，但current/head/validate/transfer均0。已排除未提交与输入尺寸不匹配，未称呈现成功。
- 原slot180在requested(+0x1c0c)=0时可成功返回并释放请求，不进后端；旧GOP启动请求{1,0,0,1,1}只写powered，跳过requested。源码缺口已证实，旧运行时该字未直接读取，根因推断与现场相符但不保证唯一。
- 新候选用原setter发布{0,1,0,1,1}，仅已验证27 ABI/primary GOP启用；不补写对象、不改SwapEnd/selector12返回、不禁PAC。新增requested/powered观察和更多selector72类型记录，原902B参数/日志/Data persist2无reset/trust/双卷union保持。
- 正式kernel/payload、原missing2/stubbed24及最终镜像检查通过；final483206b8… / vendor_boot c6a21a09… / CRC **E989F0CE55D5EB3F**。实际栈请求和原setter PAC调用已核。本候选未刷，入后端/面板/桌面待验。
- 接续：[requested状态修复](ipados27-swap-requested-2026-11-03.md)；release uke-port/releases/2026-11-03-ipados27-swap-dispatch/；审计 uke-port/fixes/2026-11-03-swap-dispatch/。

完整原始记录、分支地址和编译实参证据见根docs/status/ipados27-swap-requested-2026-11-03.md。候选未刷。


## 2026-11-03 13:27:56：requested初始化修复已获当次授权刷入

用户当次明确“刷吧”，部署swap-dispatch候选。5个产物SHA及porting13区通过；由TWRP执行adb reboot bootloader，核目标b8a81f12/productuke/is-userspaceno和四个96MiB分区上限。boot_a/vendor_boot_a/boot_b/vendor_boot_b均OKAY，set_active a及current-slot a回读成功，rebootOKAY。

- 时间：2026-11-03T13:27:56.249263+08:00 至 2026-11-03T13:28:02.848375+08:00。
- boot eada61328aebb0aab5e86f01cc3528a4025445e93c7f08a592be7b89889346dd。
- vendor_boot c6a21a09fdecde986d69c2cf9ecd33efe99d7e3698d87b4b8f3419a72cb616a1。
- final 483206b89bd9b9e5a0ab9dd74cad4cf833449564b51ad368446d582ba4a70f68；CRC **E989F0CE55D5EB3F**。
- 证据：uke-port/releases/2026-11-03-ipados27-swap-dispatch/flash-result-20261103-132756.json与匹配flash log；进bootloader记录在审计目录bootloader-transition.json。
- host没有刷写/初始化userdata，正常XNU运行期保持Data持久化及尾部日志写入。刷写成功不代表requested实际取值、后端入队或画面已经验收。

后续：确认CRC页；若出现图形，反馈画面及触摸。若panic，拍完整首行与BT。若仍无图形但系统存活，至少运行三分钟后回fastboot读取新环，核requested/powered、swap validation及transfer；以13:06旧dump逐slot排除旧记录。


## 2026-11-03：已出图，触摸被backboardd按键盘服务拒绝；split候选未刷

- 用户确认swap-dispatch亮屏进系统，但滑动无响应；已记里程碑。本轮日志实测requested/powered均1，前8个swap校验accepted1/reason0，出图用户证据与交互未验收分开。
- 13:33:21只读尾部新环66条seq0..65全部CRC有效，排除1982个未改旧slot；raw80406d28…，日志到179.377485s。触摸到2613有效帧，304个HID报告均result0、坐标变化；不是本采样内扫描或HID上报停止。
- 本次boot851FECBA…的统一日志明确unexpected digitizer usage pair0x10006、keyboard1/6和ignoring digitizer event from unsupported service（提取结果73条拒绝消息）。触摸事件已到backboardd，被服务分类丢弃，不能把handleReport0当手势成功。
- 仅启用已链接上游sheng-touch-split=1，准备独立Digitizer/Touch Screen(0x0d/4)设备；无驱动/算法/轴/安全策略改动。payload重打和最终检查通过，内核与已出图版本逐字节相同；922B参数，vendor_boot5d252ad4…。
- **CRC仍为E989F0CE55D5EB3F（内核未变）**，不能用CRC区分split配置。候选uke-port/releases/2026-11-03-ipados27-touch-split/未刷，设备当前原厂四分区+TWRP；host未写userdata，真实手势待验。
- 接续：[27线触摸分类拒绝](ipados27-touch-classification-2026-11-03.md)；审计uke-port/fixes/2026-11-03-touch27/。

原始证据与候选SHA见根docs/status/ipados27-touch-classification-2026-11-03.md。


## 2026-11-03 13:43:32：touch-split已获当次授权刷入

用户当次明确“刷吧”。刷前5SHA和porting13区通过，vendor_boot精确为5d252ad4973bd9361b66534781d49df2f6e068ba4bdff5d073fbcfb8eb29a821。TWRP执行adb reboot bootloader后核b8a81f12/productuke/is-userspaceno及四个96MiB分区界限。boot_a/vendor_boot_a/boot_b/vendor_boot_b均OKAY，set_active a及current-slot a回读通过，rebootOKAY。

- 时间：2026-11-03T13:43:32.842168+08:00 至 2026-11-03T13:43:39.568251+08:00。
- 仅增加sheng-touch-split=1；922B参数，final483206b8…保持，CRC仍为**E989F0CE55D5EB3F**，不能单用CRC区别配置。
- 部署证据：uke-port/releases/2026-11-03-ipados27-touch-split/flash-result-20261103-134332.json和匹配flash log，进bootloader记录在审计目录bootloader-transition.json。
- host未刷写/初始化userdata；正常XNU运行期仍保持Data与尾部日志写入。仅刷写/重启成功，新触摸设备匹配、分类与真实滑动尚待验收。

后续：确认仍能出图，实测上滑/横向滑动/点击，说明是否有响应及方向是否正确。若无响应，至少运行三分钟并反复实际滑动后回fastboot，核独立Offline touch digitizer、0x0d/4分类及unsupported digitizer是否消失；不凭HID报告0宣称手势成功。panic拍完整首行/BT。


## 2026-11-03：触控恢复，上划触发backboardd合成断言

- 用户确认touch-split后触控恢复；上划进入系统会卡住并显示重启动画。只作为触控/上划用户证据，未记完整桌面稳定通过。
- 13:50:56 TWRP只读末尾32MiB，raw SHA `00bd456af323b75ea4e1f32c691f434af86edbb9909183ddc744ab803c9729dd`。与13:33:21逐slot对照，114条新CRC有效记录，其余1934槽不变排除。原始环和同tag日志：`reference/logs/{ukelog,boot}/*20261103-135056-touch-split-unlock*`。
- 本轮backboardd pid62/341/373分别在154.025/210.343/267.478s以SIGNAL6退出；SpringBoard因`/usr/libexec/backboardd respawn`终止并重启，Gunyah心跳到271s、日志到275s。本采样支持用户态界面服务重启，未见新内核panic；不能把重启动画直接当内核重新启动。
- 新Data只读重构：generation2，1224590已写页，传输5016690688B，原seed588bd73f…保持。统一日志031f/0320/0321解析2178898条，全部boot UUID `569A4B36-0765-4284-9334-EBB0D97D369B`。
- 同boot真实backboardd pid341/uptime210s报告`backboardd-1970-01-01-080333.00011.ips`：SIGABRT、faultingThread8 `com.apple.coreanimation.display.primary`，`__assert_rtn -> CA::OGL::Context::push_surface +556 -> AsynchronousNode::retain_surface +1108`。统一日志pid62/341两次原消息`context(1) transient surface ... not found`；不是触控扫描停止。内核TCH到13572有效帧/516HID报告/result0。
- 对24A446原QuartzCore反汇编：unslid push_surface0x184464080，0x1844642a8调用assert，字符串`found != slist->end ()`/ogl-context.cpp行1309。报告shared cache slide0x3b4000，与内核用户BT0x1848182ac对应；不拿slid地址直接当unslid符号。
- 同版原QuartzCore内置`CA_DISABLE_ASYNCHRONOUS`：原选项表index62 -> byte0x1e70211aa，CADefaultsGetFloat先getenv/strtod再原偏好路径，原0x1844bb78c读取并在1时清异步标志。采用此原有同步分支作为可回退候选；这不是已证明的表面丢失根因修复。
- 候选只新增27线设备patch与`uke-ca-sync=1`，在原只读认证union成功后使用已有scratch+0x400/0x500传入`CA_DISABLE_ASYNCHRONOUS=1`启动环境，不写偏好/原系统文件。原mount/owner/trust/PAC/失败返回/Data persist2/触控split保持。launchd是否向backboardd转交环境、同步合成实际效果待实机；构建中，尚未部署。
- 审计：`uke-port/fixes/2026-11-03-unlock-restart/`；用户报告、recovery转换、只读脚本、raw索引、真实IPS、统一日志与原始反汇编均已保留。本轮host未写userdata。


## 2026-11-03 14:03:03：原QuartzCore同步合成候选已刷入

正式kernel/payload构建通过，原missing2/stubbed24精确集合保持；最终FAT/参数/trust/Data种子/实际代码引用与原认证错误路径检查通过，porting13区通过，未重复宿主/QEMU套件。936B参数只新增`uke-ca-sync=1`，保留触控split与Data persist2/无reset。

- boot `eada61328aebb0aab5e86f01cc3528a4025445e93c7f08a592be7b89889346dd`。
- vendor_boot `7ea5a7ff7084661eabeba893018e12f13af20a7bbb16e10b5e24457014151f88`。
- final `07b16e4fcd5488559c9b6d0079a1cb0fca6db5d953eb1104ec605941123d329f`，CRC **AA3AB98871D9491A**。
- 部署范围遵循本持续任务已有“刷吧 你做完直接刷就行”授权；从TWRP执行adb reboot bootloader，刷前5SHA/目标uke/b8a81f12/is-userspaceno及四96MiB分区检查通过。四分区均OKAY，A槽回读/rebootOKAY，时间2026-11-03T14:03:03.376173+08:00至2026-11-03T14:03:07.676737+08:00。
- 证据：`uke-port/releases/2026-11-03-ipados27-ca-sync/flash-result-20261103-140303.json`与匹配flash log，及审计目录bootloader-transition.json/pack.log。宿主Data/SystemOS FUSE已卸载；只读快照保留，host未写userdata。

候选使用原QuartzCore CA_DISABLE_ASYNCHRONOUS同步分支，原assert保留，不称表面生命周期根因已修。launchd向backboardd传递环境、上划进桌面和三分钟连续交互仍待实机。失败按13:50:56原始环逐slot排旧，并核同boot的backboardd报告。去掉uke-ca-sync参数或刷回touch-split即可回退。


## 2026-11-03 15:54：ca-sync仍在原断言退出，尾部证据足够

用户反馈上划仍卡死，设备运行较久，要求优先看尾部。已进TWRP只读取末尾32MiB，raw SHA `82a86aac9ded8d979a14c27eca6d24766c341ec0d372af82bcbd282cb5a83bf3`；2048槽均被本次写过且CRC有效，序号1183..3230，早期启动已覆盖。末尾保留6437.827s/backboardd62与6493.371s/backboardd1596的真实异常和SpringBoard respawn，内核心跳到6514s、日志到6520.770s；不需要为了拿同一异常立即重录。

Data只读快照generation2/1225190已写页、传输5018935296B、原seed588bd73f…保持。最新统一日志033a/033b/033c共解析2596942条，全部boot `C8AC96B9-9902-4C61-98F7-47DFD02FA94E`。真实报告`backboardd-1970-01-01-094816.00012.ips`/incident785A5813…/pid1596与尾部PID对应，仍是SIGNAL6，`push_surface+556 -> AsynchronousNode::retain_surface+1108`；原消息`context(1) transient surface 101AD4540 not found`仍在。报告uptime字段为6400，与内核精确6493s分开记录，不用报告时间替代内核时间。

**上版ca-sync的用户效果已失败；只给init传环境不能证明原backboardd拿到了开关。**早期UKE CA init行已覆盖，当前原生报告不携带环境，现有证据不能区分launchd未转交和开关不足。停止把此版称为有效同步合成修复。

新候选ca-job-env：仅将24A446原`com.apple.backboardd.plist`加原`EnvironmentVariables`字典`CA_DISABLE_ASYNCHRONOUS=1`，MachServices/UserName/ProgramArguments/_Conclave等原配置全部保留。复用上游NGPUADD2 installer与已有只读bindfs union；864B addon只含该配置，无代码。`uke-ca-sync=2`仅安装/挂载专用`/private/var/ngpu/ca-sync-daemons`到LaunchDaemons，核一文件安装成功后才挂载，不启GPU/其它addon挂载/旧init环境分支。

同时只读记录实际backboardd exec中该变量是否为1，不输出其它环境；若仍有异常，仅对当前线程且共享缓存UUID匹配24A446时，读取原选项byte0x1e70211aa+当前sr_slide。原断言/异常流、AMFI/owner/trust保持。候选构建中，尚未部署；开关投递和实际稳定性都待实机。

审计`uke-port/fixes/2026-11-03-ca-sync-readback/`：readback、只读Data、同bootIPS与统一日志、原始job与语义diff清单、864B addon及源码前态。原环/同tag解码在`reference/logs/{ukelog,boot}/*20261103-155418-ca-sync-tail*`。host未写userdata；候选下一次XNU正常运行会按既有addon实现向Data专用目录写入配置，卸载重启后仅在mode2再挂载。回退mode1/不带参数不挂此目录，原系统卷未改。


## 2026-11-03 16:06:25：ca-job-env已刷，实际投递与上划待验

ca-sync长时间启动的尾部原异常已取到，无需立即重录旧版。新候选使用原backboardd job的EnvironmentVariables显式投递`CA_DISABLE_ASYNCHRONOUS=1`，仅864B单配置addon，语义检查确认其它job键完全保持，无可执行代码/新trust项。`uke-ca-sync=2`只走现有installer/只读bindfs union的专用目录，不启GPU；去掉参数或回退旧版不挂该目录。

- 正式kernel/payload构建通过，原missing2/stubbed24精确集合与owner/trust/Data种子保持；936B参数，实际FAT/6SHA/机器码调用检查通过，porting13区通过，无重复宿主/QEMU套件。
- payload仍94MiB，实际mtools成功分配后还剩1168384B，vendor_boot100462592B小于96MiB分区。原2%预估误拒（内容96608042B仅超估算13221B），构建器已改为原mformat/mcopy实际容量校验，未放大payload/分区。
- final `5fccb387155ac91ede970c78551c2c58f795dc61d39883dc10a4425d876e5891`，CRC **B53979C594EB2039**；vendor_boot `e26aadfb49e365735e496780659fc5f827feb792a98bc15910c0f4f79724b869`；boot eada6132…保持。addon SHA `3db0aced4909d03eaf43f1f46b2d4833a8d88ba683f1ff03695d559f38bb3b58`。
- 本持续任务已有做好直接刷授权；TWRP经adb reboot bootloader，刷前6SHA/uke/b8a81f12/is-userspaceno/四96MiB检查通过；boot_a/vendor_boot_a/boot_b/vendor_boot_b均OKAY，A槽回读/rebootOKAY，2026-11-03T16:06:25.896069+08:00至2026-11-03T16:06:28.205899+08:00。
- 部署证据：`uke-port/releases/2026-11-03-ipados27-ca-job-env/flash-result-20261103-160625.json`及匹配log，审计目录bootloader-transition.json。host未写userdata，FUSE均卸载；下一次XNU正常运行会按原addon实现向Data专用目录写入2510B配置。本地unixnu.local.toml已指向release的864B addon，避免下次标准payload构建漏带配置。

验收：确认CRC，测试上划能否进入桌面并点击/滑动三分钟。若仍卡死/转圈，约一分钟后回fastboot取日志，无需长时间运行。重点读`UKE CA: backboardd job union stage=5 status=0`、`backboardd exec disable_async_env=1`；若仍abort，读UUID匹配的`disable_async_byte`，区分投递失败与原开关无效。实机结果未验收，不称断言根因已修。


## 2026-11-03：用户要求直接修复；软件表面缺失登记修复构建中

ca-job-env实机仍卡死。16:12:54新环114条seq0..113全部CRC有效，1934槽与旧环不变排除，raw SHA `f7b3d02cec160ccec006280c73c4125ffe42731d4e2d6c1ca63f3ab04080b434`。本轮50.887970s原job union确实stage5/status0/written1，但81.038/151.033/211.444s实际backboardd exec均disable_async_env=-1；150.399/210.783s原选项byte均0、copyin成功、cache_match1、slide0x21b0000。后续仍是原push_surface断言，内核心跳到271s。Data回读安装job SHA ca22afd3…匹配候选；同bootCC1FE8FE…统一日志仍有两次transient surface not found。**该版失败，开关没有生效；不能把它算成开关本身的阴性实验。**

24A446原launchd服务cache `/System/Library/xpc/launchd.plist`（2427409B，SHA7291b582…）含不带EnvironmentVariables的原backboardd job，实际env缺失与其相符；消费缓存为静态源码/配置推断，未伪称逐指令跟踪。原launchd_no_cache分支仅从本轮反汇编确认cache失败处理，未据此再发候选。

用户明确要求“不要一直反复验证没有实际修复效果的东西，直接修复，由用户验效果”。未刷的exec-env/mode3实验取消；三个CA环境/诊断patch退出series，uke-ca-sync参数移除，addon输入与专用job挂载停用。保留已取证据，不再以验证投递为下一版目标。

**直接修复的静态缺口：**原QuartzCore `SWContext::create_surface_from_iosurface`0x1846c8718调用`create_surface_from_data`并retain IOSurface后直接返回，两个函数都没有登记表面；`AsynchronousNode::retain_surface`的原slot0x308返回后立即push_surface。原SWContext普通表面创建路径则明确调用`Context::track_surface`0x1845096a8。本轮将原工厂成功返回的Surface接回同一个原登记函数，NULL保持不登记；不删除/跳过push_surface断言。此为与现场吻合的缺口修复，真实上划效果待用户验证。

复用已链接上游native-text-fixup与payload.text_fixups，24A446/.01原厂缓存页面签名验证成功且原字节匹配后应用。工厂入口PACIBSP保留，第二条改为直跳56B wrapper，wrapper重放原STP并进入原body；返回非NULL后调用原track_surface并原样返回Surface。双层PACIBSP/RETAB、栈与x19保存；不改虚表/PAC签名，不跳原CF retain/锁/失败处理。

wrapper仅放在原__TEXT两个section之间60B零padding的前56B（0x1847ef484），未覆盖任何section内容。缓存物理文件`.01`偏移0x42c8718为16B原入口守卫，0x43ef484..4bb分四条<=16B守卫写wrapper，共5条修正；不误用聚合CACHE offset。原缓存UUID06BBBEA7…/根共享缓存ce33b2ba…与24A446输入留档。

按用户要求仅一个有必要的机器码检查：qemu-aarch64-static -cpu max执行实际原工厂/原track_surface及新wrapper，真实PAC指令、非NULL登记并保留IOSurface、NULL分支、SP/x19..x24与返回值全部通过，退出0；这不是实机修复证明。现正式构建/打包，尚未刷。

审计：`uke-port/fixes/2026-11-03-ca-surface-track/`，原始当前日志在`2026-11-03-ca-job-env-readback/`及`reference/logs/{ukelog,boot}/*20261103-161254-ca-job-env*`。host未写userdata；原系统文件不落盘改写。回退touch-split四分区或移除payload.text_fixups，Data seed保持。


## 2026-11-03 16:43:30：软件IOSurface缺失登记直接修复已刷入

按用户“直接修复，由用户验效果”的要求，本版将原`SWContext::create_surface_from_iosurface`的非NULL结果接回原`Context::track_surface`，处理原push_surface断言前缺失的登记。原工厂、NULL/retain/锁、入口PACIBSP及返回认证保持，原assert保留。此前环境/投递实验全部退出series/参数/addon输入。

- 5条原字节守卫通过已链接上游native-text-fixup交付：16B工厂入口守卫仅改变第二条为直跳；56B wrapper位于60B section间零padding，补原登记调用。应用前仍要求原页面签名有效且未taint、原字节匹配，原磁盘缓存与root/trust不改写。
- 单次必要的实际机器码QEMU检查通过；正式kernel/payload、原missing2/stubbed24精确集合、最终FAT/实际ADT修正表/7SHA/vendor_boot边界通过，porting13区通过。参数恢复touch-split的922B，Data seed/trust/触控/显示requested保持；无更多宿主测试。
- final `988d162c178a8b02eaffe7cc9aefe833918868e4f23567f10adc9cde5bdcb3e6`，CRC **0485290E96ACC9B3**。
- vendor_boot `150b5c063c0d97580ddb012e31596486aaf9f86cb5d2a5f911b3958643295b4d`，100462592B；boot eada6132…保持，payload94MiB保持。
- 本持续任务已有直接刷授权，用户本轮再次明确要求直接修复并自行验效果；TWRP经adb reboot bootloader，7SHA/目标uke/b8a81f12/is-userspaceno/四96MiB检查通过。四分区均OKAY，A槽回读/rebootOKAY，2026-11-03T16:43:30.934249+08:00至2026-11-03T16:43:37.725392+08:00。
- 证据：`uke-port/releases/2026-11-03-ipados27-ca-surface-track/flash-result-20261103-164330.json`及匹配flash log；审计`uke-port/fixes/2026-11-03-ca-surface-track/`。host未写userdata，三处FUSE已卸载。旧Data中的ca-sync-daemons文件保留但本版不挂载它。

**实际上划/桌面稳定效果待用户验证**，不把QEMU/刷写成功作为设备修复通过。若再失败，取新尾部辨认5条text-fixup实际应用和新的真实栈；不回到环境投递反复验证。回退touch-split四分区或移除payload.text_fixups，Data保持。


## 2026-11-03：27线上划不再卡死，ca-surface-track用户实机验收通过

用户在本次修复刷入后明确反馈：“这次正常了，不会卡死。记录一下吧”。结合紧接此前“请直接测试上划进入系统”的任务上下文，记录为**上划进入系统正常、此前卡死现象消失（用户实机验证）**。该版作为27线当前可交互基线；17线gpucomp49基线保持独立。

- 修复：原`SWContext::create_surface_from_iosurface`成功返回后补调用原`Context::track_surface`，原NULL/retain/PAC与push_surface断言保留。此前环境实验已退出。
- release：`uke-port/releases/2026-11-03-ipados27-ca-surface-track/`，16:43:30四分区/A槽/reboot记录保持。
- final：`988d162c178a8b02eaffe7cc9aefe833918868e4f23567f10adc9cde5bdcb3e6`，CRC **0485290E96ACC9B3**。
- vendor_boot：`150b5c063c0d97580ddb012e31596486aaf9f86cb5d2a5f911b3958643295b4d`；boot：`eada61328aebb0aab5e86f01cc3528a4025445e93c7f08a592be7b89889346dd`。
- 用户证据：同release的`user-observation.json`及审计`uke-port/fixes/2026-11-03-ca-surface-track/user-observation-success.json`，记录时间2026-11-03T17:05:05.179372+08:00。
- 验收范围仅为用户此次确认的正常上划/不卡死；没有给出运行时长或覆盖的应用，不扩展为长时间、所有应用、跨重启或GPU加速验收。本轮只维护记录，未操作设备。

本次合成卡死任务已按用户可见结果完成。后续从此基线继续其它功能与日常使用验证，不再把上划卡死列为当前未解决阻塞。


## 2026-11-03: upstream-main iPadOS 27 GPU candidate

See GPU-IOS27.md. Full host build and compressed paired-image candidate; hardware pending. CPU interactive rollback remains ca-surface-track. No flash or userdata reset performed.


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
