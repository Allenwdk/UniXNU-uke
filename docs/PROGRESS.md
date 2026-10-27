# UniXNU on Xiaomi Pad 7 (uke) — 进度文档

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
