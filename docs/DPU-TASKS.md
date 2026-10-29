# uke DPU 硬件直扫 — 任务清单（可直接派活版）

目标：让 SM7675（cliffs）的显示处理单元（DPU）**直接从合成器 surface 扫出**到面板，
替代现在的 CPU 解码+拷贝（3–7 fps），达到满帧率、低延迟、CPU 零搬运。
每项任务写明：做什么 / 材料在哪 / 怎么算完成。里程碑 M1–M5，任务号如 `A3`。

> **总原则（每张工单都要带上）**：信息**优先**从官方文档获取——
> <https://unixnu.com/zh/porting/gpu>、<https://unixnu.com/zh/drivers/adreno740>、
> <https://unixnu.com/zh/drivers/adreno-metal>；
> **不造轮子**：凡是上游已有实现（Adreno740 / AdrenoMetal / GopFramebuffer / core 构建链），
> 一律使用上游的实现方式与路线；**上游怎么做就怎么做，就算复杂也不得自行简化**——
> 简化出来的"捷径"就是下一轮返工和 panic。实在缺能力时，向上游的结构里"长"新能力，
> 而不是另起炉灶。

---

## 0. 范围与非目标

- **在范围**：单管道直扫一条合成器 surface（先线性 BGRA，后压缩格式）；flip/防撕裂；
  与现有 swap 流水线集成；失败回退现有 GOP CPU 拷贝（永不黑屏）。
- **不在范围**：多层硬件合成、DSC/视频模式调教、亮度/背光（已有轨）、热插拔、
  外接显示（cliffs 有 mdss_1，忽略）、以及 `'b3a8'` 的 GPU 解码（归 GPU 轨，见 D1 的合流点）。

## 1. 硬件事实（已核对，直接用）

| 件 | 事实 | 出处 |
|---|---|---|
| DPU/MDP 块 | `qcom,mdss_mdp@0xae00000` | `reference/dts-reference/03_dtbdump_Qualcomm_Technologies,_Inc._Cliffs7P_SoC.dts:17653` |
| DSI 控制器 | ds0 `@ae94000`、dsi1 `@ae96000` | 同上 :17919/:17948 |
| DSI PHY | phy0 `@ae95500`、phy1 `@ae97500` | 同上 :17977/:18010 |
| 显示时钟域 | disp_cc GDSC：`mdss_core_gdsc@af09000`、`mdss_core_int2_gdsc@af0b000`（还有 mdss_1@`a909000`，不用） | 同上 :13484/:13500 |
| 面板 | O82 3200×2136，现由固件 GOP 点亮 | `docs/GPU-PORT.md` |
| 现状 | **固件已在用 DPU 扫 GOP 显存**——上电配置是活教材 | 见 A3 |
| 合成器 surface | 在高 DRAM（`0x8C0000000+`，XNU VM），`'b3a8'` 两平面 XR10（5 B/像素） | 工作台账 2026-10-28 |

## 2. 参考实现（先读再写，禁止凭空发明）

**0 号来源（优先于一切仓库考古）——官方文档**：
- <https://unixnu.com/zh/porting/gpu>（GPU 移植路线/阶段方法）
- <https://unixnu.com/zh/drivers/adreno740>（内核 GPU 驱动的实现路径）
- <https://unixnu.com/zh/drivers/adreno-metal>（Metal 前端/翻译层的实现路径）
先读文档定路线，再进仓库看代码；两者冲突时以文档+上游最新代码为准。

1. **sheng 的 DPU 直扫**（结构样板）：`reference/GopFramebuffer/NativeLegacyDisplay.cpp`
   commit `33e941a`（`sheng-dpu-scanout=2`）：VIG 0/1 管道 @ MDP+0x4000/0x6000（源地址 +0x14、
   stride +0x24）、CTL 0 flush @ +0x15018（位 0/1）、5.4 ms latch 实测、失败回退拷贝、
   surface 经 `native_ufs_dpu_map` 的 stage-1 窗口寻址。**结构照抄、寄存器按 cliffs 考古**。
2. **stock 内核的高通显示驱动**（寄存器/序列权威）：`reference/Xiaomi_Kernel_OpenSource-uke/
   drivers/gpu/drm/msm/`（`msm_mdss.c`、`dsi/`、DPU 初始化序列）+ `include/dt-bindings/display/`。
3. **本仓现有显示链**（集成点）：`UniXNU/drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp`
   的 `presentBase`（swap 后呈现）、`package/BootProgress.c`（诊断页/面板接管）。
4. **GPU 轨 staged 方法论**（bring-up 阶梯写法）：`docs/GPU-PORT.md` 模式 1–9。
5. **sheng 侧窗口机制**（surface 寻址参考）：`reference/UniXNU-sheng/drivers/ShengUfs/` 的
   `native_ufs_dpu_map`（stage-1 窗口）。uke 对应物见 B2。

## 3. 任务分解

### A. 考古与现状侦察（只读，可并行，先行）

- **A1 DT/寄存器地图整理**
  做：把 cliffs DTB dump 里 `mdss_mdp` 节点的 reg/interrupts/clocks/power-domains/iommus
  连同 disp_cc 时钟表抄成一页寄存器地图（含每个 clock 名 → 偏移）。
  材料：`reference/dts-reference/03_dtbdump_...Cliffs7P_SoC.dts`（:17653 起）。
  完成：`docs/dts-reference/mdss-cliffs-map.md`，含与 stock 驱动常量的对照。
- **A2 stock 驱动初始化序列通读**
  做：读 stock `msm_mdss.c`/DPU 初始化，摘出 cliffs 的上电序列（GDSC→时钟→SMMU→管道→
  INTF/时序→PP/flush）与面板 O82 的 timing 配置；标注每一步对应的寄存器。
  完成：一张"上电序列×寄存器×源码行号"对照表，并入 A1 的文档。
- **A3 活态寄存器快照（黄金材料）**
  做：写只读探针（kernel 模块/探针归档，照 `UKE GPU` 行的写法），dump 0xae00000 起的
  管道/CTL/INTF/PP 寄存器（几十个偏移即可）；开机后读一次并打印成 `UKE DPU: …` 行。
  **固件正在跑的配置 = 现成 working config**：源地址（=GOP FB）、stride/format、
  时序、哪条管道、哪个 INTF，全在快照里——抄它，别猜。
  材料：GPU 轨 `package/GpuPowerProbe.cpp`（探针+行格式样板）。
  完成：一次开机日志里有完整快照行，且与 A1 地图逐项对上。
- **A4 面板连接判定**
  做：从快照 + 引脚（`dts-reference` 里的 pinctrl/面板节点）确定面板挂在 DSI0 还是 DSI1、
  实际用的 INTF 端口。
  完成：A3 快照旁标注"面板=INTFx@…"一句结论。

### B. 时钟 / 电源 / 总线 / 寻址（依赖 A1；B1/B2/B3 可并行）

- **B1 显示时钟与 GDSC**
  做：按 A2 序列实现 disp_cc/GDSC 引用计数式开（照 `docs/TOUCH-PORT.md` 的 GCC 编程写法：
  RCG 编程+CMD_UPDATE 轮询+回读校验）；只开不改频率（沿用固件频率）。
  完成：探针镜像里 GDSC/时钟状态读数与快照一致（说明我们开关正确）。
- **B2 surface 寻址通路（关键未知项）**
  做：确定 DPU 读内存走不走 SMMU（看 A1 的 iommus 属性）；合成器 surface 在高 DRAM
  （0x8C0000000+），DPU 需要能寻址：走显示 SMMU 的 IOVA 映射，或学 sheng 开一块
  stage-1/低位窗口（对照 `reference/UniXNU-sheng/drivers/ShengUfs/native_ufs_dpu_map`）。
  完成：一段测试内存在目标地址可被 DPU 管道读到（由 C2 验证），方案写进 A1 文档。
- **B3 总线带宽投票**
  做：显示通路的 BCM/带宽投票（3200×2136×60bpp 若 60 Hz ≈ 1.6 GB/s 读），按 stock 序列的
  最小投票实现；不足会花屏/挂起。
  完成：投票后读数稳定，长跑无总线超时。

### C. 最小接管（依赖 A3、B1；阶梯推进，panic-safe，每步一个可刷镜像）

- **C1 只读探针镜像（M1 里程碑）**
  做：A3 的探针进正式构建（`[kernel] archives`），带 `UKE DPU` 行与门禁脚本
  （host 侧核对快照字段，照 `check-gpu-addon-host.py` 的样式）。
  完成：**M1 gate** —— 日志有快照、host 门禁绿、与 A1 地图一致。
- **C2 单管道改源地址（M2 里程碑，第一次"碰"硬件）**
  做：把固件那条在跑的管道（快照定的那条）的源地址改到一块测试图案 buffer
  （纯色/渐变，低 DRAM 或 B2 通路可达处），CTL flush 一次。
  **屏幕出现图案 = DPU 已受控**；失败只黑/花一帧，回滚参数即恢复。
  完成：**M2 gate** —— 屏幕可见图案 + 快照读回源地址已变。
- **C3 stride/format 变体**
  做：同一管道扫线性 BGRA（先 3200×2136 满屏，再非满屏 stride 验证缩放/裁剪行为）。
  完成：三种已知图案（纯色/棋盘/渐变）色位正确。
- **C4 latch/flip**
  做：CTL flush 的 latch 时机测量（sheng 实测 5.4 ms 级）；双缓冲切换（两块 buffer 轮换）
  不撕裂；记录 `UKE DPU` 行的 flip 计数/耗时。
  完成：连续 flip 1000 次无挂、无撕裂（相机拍屏幕验证）。

### D. Surface 直扫与集成（依赖 C4、B2；D1 与 GPU 轨有合流点）

- **D1 合成器 surface 直扫（M3 里程碑）**
  做：把管道源指向 swap 记录解析出的合成器 surface（本仓 presentBase 已解析该 surface）。
  **格式注意**：当前帧是 `'b3a8'`（两平面 XR10）——cliffs DPU 大概率不认；两条路：
  (a) DPU 认压缩/平铺格式则直读（快照里固件管道的 format 字段先给出线索）；
  (b) 不认则走 **GPU 解码到 linear BGRA 再直扫**（GPU 轨的 GpuScanoutClient 扩展，见
  GPU-PORT 的 next_action；两轨在此合流，接口 = "给我一块 BGRA，我扫出去"）。
  完成：**M3 gate** —— 直扫合成器画面（允许经 GPU 解码中转），色位正确。
- **D2 presentBase 集成（M4 里程碑）**
  做：swap 后把 surface 交给 DPU（替代 CPU 拷贝），保留现有 CPU 拷贝为回退
  （boot-arg 门控，如 `uke-dpu-scanout=1`，照 sheng 的 `sheng-dpu-scanout` 命名）；
  失败自动回退、计数可见。
  完成：**M4 gate** —— 交互流畅（>30 fps 观感），CPU 拷贝计数为 0，回退路径手动验证可用。
- **D3 flip 与 SwapWait 对齐**
  做：DPU flip 完成 ↔ swap retire 的时序对齐（vblank 语义）；帧节奏与 SwapWait 不互相卡。
  完成：长跑无 busy-wait、无 swap 积压（`UKE: swap` 时间线平直）。

### E. 验收 / 稳定 / 记录（依赖 D2/D3）

- **E1 长跑与 teardown（M5 里程碑）**
  做：>1 小时交互长跑、panic 恢复（panic 屏仍可用）、休眠/唤醒路径不碰、
  反复进出诊断页（BootProgress 接管/归还面板）不挂。
  完成：**M5 gate** —— 1h 无 reset、panic 屏与诊断页共存正常。
- **E2 证据与记录**
  做：帧率/撕裂/latch 证据进 `porting.toml` display 区（按 record check 格式）；
  新建 `docs/DPU-PORT.md` 收口本清单的结论；GPU-PORT 交叉引用。
  完成：`porting-record.py check` 绿；文档与实现一致。
- **E3 回归护栏**
  做：host 门禁脚本扩展（快照字段/寄存器写序校验），进 CI 的 `ci.yml` 检查链。
  完成：故意改坏一个寄存器序号时门禁变红。

## 4. 依赖图与并行建议

```
A1 ──┬── B1 ──┐
A2 ──┼── B2 ──┼── C1 ── C2 ── C3 ── C4 ── D1 ── D2 ── D3 ── E1
A3 ──┤        │                              ↑
A4 ──┴── B3 ──┘                        (GPU 轨 D1-b 合流)
```
- **可并行**：A1–A4 四项互不依赖；B1/B2/B3 三项互不依赖；E3 可随时插空。
- **单人快线**：A3 → C1 → C2 是最短路径（约一周量级），先证明"管道受控"再铺开。
- **两人分工建议**：一人 A+B（考古/时钟/寻址），一人 C+D（驱动/集成）；D1-b 合流时拉 GPU 轨。

## 5. 风险清单（按概率×影响排序）

1. **surface 寻址（B2）**：合成器 surface 在高 DRAM，DPU/显示 SMMU 的可达性未证实——
   若 SMMU 不通，需 stage-1 窗口方案（sheng 有先例，风险可控）。
2. **`'b3a8'` 格式支持（D1）**：硬件多半不认 → 落到 GPU 解码中转（依赖 GPU 轨的解码 pass，
   已在 GPU-PORT next_action 里）。
3. **面板时序私有性（A2/A4）**：固件的 INTF/PHY 配置可能含私有序列——A3 快照就是解法，
   抄固件现状而不是从零配。
4. **总线带宽投票不足（B3）**：症状是随机花屏/挂起，靠长跑暴露。
5. **与 BootProgress 诊断页抢面板（E1）**：现有 `boot_progress_prepare_scanout` 协商机制要
   在 D2 里保留。

## 6. 里程碑与验收一句话

| 里程碑 | 一句话验收 | 预估 |
|---|---|---|
| M1 | 只读快照进日志，host 门禁绿 | 小 |
| M2 | 屏幕出现我们写的测试图案 | 中（首个硬件接触点） |
| M3 | 直扫合成器画面（可经 GPU 解码中转） | 大 |
| M4 | swap→DPU 集成，>30 fps，CPU 拷贝归零 | 中 |
| M5 | 1h 长跑 + panic/诊断页共存 | 中 |

## 7. 派活时的三条规矩（本项目血泪，写进每张工单）

1. **只读先行**：碰硬件前先出快照（A3），抄固件现状，不猜寄存器。
2. **每步可回滚**：C2 起每步一个独立可刷镜像 + 回滚参数；写寄存器前把原值打进日志。
3. **证据进门禁**：每里程碑配 host 侧检查脚本；`dd` 写 userdata 一律 `seek=`（RECOVERY-LOG 教训）。
4. **不造轮子、不简化上游**：实现路线以官方文档（unixnu.com/zh/porting/gpu、/drivers/adreno740、
   /drivers/adreno-metal）和上游代码为准；上游复杂就照抄复杂，**禁止"我这样更简单"**——
   需要新能力时沿上游的结构扩展（模块边界、命名、阶段划分都跟上游），不另立体系。

---

## 8. 2026-10-29 实测后的增补：走向 60/120 fps

### 8.1 已完成（设备实测）

| 项 | 结果 |
|---|---|
| A3/C1（M1） | ✅ `UKE DPU` 快照：GOP `e3940000` 3200×2136、VIG0/VIG1 各 1600×2136、INTF 120 Hz、SMMU bank 2 CBAR type1/SCTLR 0xe0 |
| B2 stage-1 窗口 | ✅ `window state=80000002`；`test=3 iova=10000000 pages=6675`；修复 = Linux 同款 `SCTLR=0x10e7`/`TCR=0x803519`/`TCR2=0x38015` + 4×L2/2MB block 表 |
| C2/C3 | ✅ test=12 重定向到 `gop+0x800000`，红/绿/蓝/棋盘颜色正确、2 s/色 |
| D1b BGRA 直扫 | ✅ `presentBase` 把 `'BGRA'` IOSurface 物理页映射进 stage-1 窗口并 flip VIG0/1，无 CPU 拷贝；用户实测约 **10 fps**、大部分场景正常，部分场景花屏 |

### 8.2 60/120 的瓶颈判断

10 fps 的主因**不是 DPU 扫描**：DPU 每帧只做地址写 + CTL0 flush（实测 latch 1.2 ms，INTF 本身 120 Hz）。
主因是**合成器还在软件渲染**——D1b 日志 `UKE GPU3: M a5.0/5.0 k0 s5/0 r1 o0/0/0`：
插件视图挂载了，但 `k0`/`o0` 表示没有客户端打开 AGX/Metal 设备，合成器没有走 GPU。

### 8.3 增补任务（按优先级）

- **P0 测量**：在 `presentBase`/`dpuPresentBgra` 加每帧计数与每秒 `UKE DPU: flip n fps=…`，
  区分“合成器提交 10 fps”还是“显示路径丢帧”；同时记录 `direct slot`/`fallback` 计数。
- **P1 GPU 合成器（GPU 轨，最大头）**：让 `o>0`、`K68xx` 出现。改 `drivers/NativeMetal`（AdrenoMetal）
  的发布/客户端路径、`scripts/add-gpu-plugin-data.py --publish/--clients`、addon blob/信任包；
  启动参数保持 `sheng-gpu-metal=1 sheng-gpu-addon=1 sheng-iconservices=1 sheng-fb-gpu=1`。
  验收：`UKE GPU3` 的 `o` 打开数 >0、插件 `K68xx`、CU/CARenderServer 走原生 Metal。
- **P2 b3a8 上屏（已实现，gpucomp26）**：合成器主 surface 是 `'b3a8'`（RGB10A8_2P_XR10 双平面：
  plane 0 = 4 B/texel 的 10-10-10-2 颜色字，plane 1 = 1 B/texel alpha，alloc = 5 B/texel）。
  原先只直扫 `'BGRA'`，b3a8 帧 `presentable=0` 被跳过 → 屏幕停在上一个 BGRA 帧 = 闪烁。
  **做法不是加 GPU render pass，而是让 DPU 直接按 XRGB2101010 扫 plane 0**：
  stock `dpu_hw_sspp.c`/`dpu_formats.c`（reference 里 SM7675 原厂内核）中
  `ARGB8888` 的 `SRC_FORMAT` 正好等于 firmware 留下的 `0x000237ff`（`unpack=0x03020001` 逐位复算一致），
  `XRGB2101010` 只差两位：清 bit 8（alpha）、置 bit 14（DX/10-bit unpack），即 `0x000276ff`，unpack 不变。
  `NativeLegacyDisplay` 现在保存/恢复 VIG0/VIG1 的 `SRC_FORMAT`/`UNPACK`，每次 flip 随源格式一起写；
  b3a8 帧的槽只映射 plane 0 的 27,340,800 B 前缀（整帧 34,177,024 B 超过 32 MiB 槽），stride 用 `4*width`。
  未改动 `presentable` 的 CPU 解码路径仍是 fallback（DPU 不可用时）。
- **P3 vsync/帧节奏**：uke 的 `NativeMobileDisplay` fork 还没有 sheng 的 `sheng-vsync` 代码；
  从 `GopFramebuffer@33e941a` 移植 `startVsync/vsyncTick/getRefreshAttributes`，
  board 参数 `sheng-vsync=120`，让 render server 按 120 Hz 走 `SwapWait`。
- **P4 D3 非阻塞 flip**：`dpuPresentBgra` 现在在 presentBase 里轮询 `CTL_FLUSH`（最长 100 ms）；
  照 sheng 的 `latchTick` 改成 workloop timer，flush 后异步 retire，合成器线程不等待。
- **P5 B3 带宽投票**：3200×2136×4 B = 27.34 MB/帧：
  **60 Hz ≈ 1.64 GB/s，120 Hz ≈ 3.28 GB/s**；DTB `qcom,sde-ib-bw-vote`、`max-bw-low` 2.99 GB/s /
  `max-bw-high` 3.31 GB/s。按 stock `msm_mdss_icc_request_bw` 在 `ShengPlatformPower`/
  `ShengGmuVotes`（或显示驱动）投 `qcom,sde-data-bus0`/`ebi`；120 Hz 必须 high 档。
- **P6 B1 显示时钟**：`dispcc` 的 `disp_cc_mdss_mdp_clk`（0x800c）+ `mdp_clk_src`（0x80d8）
  当前 514 MHz、max 600 MHz；120 Hz 前确认 600 MHz 或读数足够；GDSC 保持 on。

### 8.4 验收

| 目标 | 判定 |
|---|---|
| 60 fps | P0 计数 ≥60/s；P1 `o>0`；P2 b3a8 不再 CPU 解码；长跑无 underrun |
| 120 fps | P3 vsync=120、P4 presentBase 不阻塞、P5 带宽 high 档；DPU latch <8.3 ms/帧；无花屏/撕裂 |
| 画质 | 三种图案（纯色/棋盘/渐变）色位正确；b3a8 解码后与 CPU 解码逐像素一致（抽样） |

---

## 9. 2026-10-29 晚：主线对齐后的 S 系列任务（120 fps 前置）

背景：GPU 合成器已真正工作（`o6`、`UKE GPU8 PUB tex`），`uke-dpu-scanout=3` 直扫 BGRA + 延迟一帧，
约 15 fps；详见 `docs/DPU-PORT.md` §9。实测 GPU `commit-to-complete` 平均 51 ms、9.7 帧/s，
6 次失败提交，stall 时 `STATUS3` bit24（SMMU stalled），集中在 `KaleidoscopePoster` 的模糊/特效。

- **S1 修 stall（先做，阻塞项）**
  做：在 `GpuPowerProbe` 里每次 stall 打上游 `native_adreno_stall_mem`（FSR/FAR/IB words/映射 handle），
  行格式 `UKE GPUFLT:`；同轮加 `sheng-disable-jobs=com.apple.KaleidoscopePoster` 对照镜像。
  完成：stall 消失，或 `UKE GPUFLT` 给出确定 VA/BO/管线；对照镜像不再 stall。
- **S2 GPU 频率/带宽确认**
  做：读 GMU/RBBM 频率与 `qcom,sde-data-bus0`/`ebi` 带宽投票，确认 950 MHz 与 high 档；
  主线同步后旧的 mode 8 perf vote 需重新验证。
  完成：负载下频率/带宽读数稳定；120 Hz 3.28 GB/s 有余量。
- **S3 每帧分解测量**
  做：presentBase/dpuPresentBgra/插件分别记录 GPU commit、present、`CTL_FLUSH` 等待、copy 字节；
  每秒一行 `UKE DPU: flip n fps= … gpu=…ms present=…ms copy=…MB`。
  完成：51 ms 的分项表。
- **P3 vsync 120 / P4 非阻塞 flip / P2 b3a8 / P5 带宽 / P6 显示时钟**：见 §8.3；在 S1–S3 后按序做。

## 10. 2026-10-29：P2 实装（gpucomp26）

- 关键事实：b3a8 帧的 record `stride=3200` 是 **plane 1（alpha）的行字节**，不是颜色行；
  plane 0 行距 `4*width=12,800`。alloc `34,177,024 = 3200*2136*5 + 1024` 与 metal2vulkan
  `texture/surface_two_planes.rs` 量到的布局（颜色字在 plane 0、alpha 在 plane 1）一致。
- DPU 寄存器（`boot-dpu-*` 快照）：`vig0/vig1 fmt=000237ff unpack=03020001 stride=00003200 op=80000000`。
  用 stock `dpu_hw_sspp_setup_format` 的字段公式复算 `ARGB8888` 得 `0x000237ff`（完全一致），
  `XRGB2101010` 得 `0x000276ff`、unpack 不变；`XBGR2101010` 才需要 `unpack=0x03010002`。
- 镜像：kernel `3e06e971…`（payload 内 `912fe304…`，含 panic-screen patch）、payload `94cfdcf7…`、
  vendor_boot `1d0294cf…`、boot `0220bc5a…`（未变），release `uke-port/releases/2026-10-29-gpu-dpu-tenbit/`。
- 新日志：`UKE DPU: direct ready fmt=… unpack=… tenbit=000276ff`；
  前两帧 b3a8 打 `UKE DPU: tenbit sample p0=… p0c=… alpha=…/…`（颜色字与 alpha 实测值）；
  `UKE DPU: tenbit refused …` 只在 firmware 的 unpack 不是 `0x03020001` 时出现；
  `UKE DPU: direct slot… bytes=27340800` 表示 plane 0 前缀映射。
- 待判定：闪烁是否消失；颜色顺序是否正常（若 R/B 反了，改 `unpack=0x03010002` 即可）；
  b3a8 帧是否持续走 `direct slot` 而不是 CPU fallback。

## 11. 2026-10-29：gpucomp26 日志结论与 gpucomp27（vsync + 计数）

- gpucomp26 日志（recovery 读 userdata 窗口，t=16..917 s）：
  `direct ready fmt=000237ff/000237ff unpack=03020001 tenbit=000276ff`；
  4 个 b3a8 帧按 plane 0 前缀映射（`bytes=27340800`）；
  `tenbit sample p0=18060180 p0c=37edfb7e alpha=ff/ff` → **alpha 全 0xFF，帧不透明**，颜色顺序正确。
  全程只有一次 stall 快照（t=78.4 s），之后无 GPUFLT；`G commit-to-complete` 平均 50.9 ms、
  8884 次/893 s（约 9.9 次/s），max 1.11 s。
- 因此剩余闪烁不是 b3a8 被跳过；按上游 `33e941a` 补 vsync（P3）并加 P0 计数。
- gpucomp27：移植 `startVsync/vsyncTick/stopVsync/armVsync/getRefreshAttributes/registerService`
  的刷新属性发布；builder 绑定 `_native_legacy_vsync_notify`/`_native_legacy_set_refresh_properties`
  （带原始字节校验）；board `sheng-vsync=120`（去掉 `armcpu-diagnostics=1`，命令行 987 B）。
  新增 `UKE DPU: flip n=… tb=… skip=… fb=… latch=…us max=…us vsync=…`（每 120 帧）与
  `UKE DPU: skip …`（前 8 次），用于区分“合成器没交帧”和“显示路径丢帧”。
- 镜像：kernel `c93aaea6…`、payload `925826a7…`、vendor_boot `cc2b7503…`、boot `0220bc5a…`。

## 12. 2026-10-29：gpucomp27 日志与 gpucomp28（槽位纪律 + 延迟）

- gpucomp27：`flip` 10–28 fps、`skip=0`、`fb=0`（显示路径不丢帧）；`latch` 平均 4.1 ms / 最大 8.4 ms
  （等帧边界，flip 本身 tear-free）；合成器每帧 1 pass / 60 draw / 6.68 Mpx，`ctc` 27–41 ms，
  commit 间隔 85–100 ms（空闲）/12–30 ms（突发）；`X v0/0` 说明 perf vote 已接受（950 MHz）。
- **闪烁根因**：`dpuSlot` 轮转可能换掉正在扫描的槽，重映射同一 IOVA 的页 → 一帧混合画面；
  只在新 surface 频繁出现（转场/模糊）时触发。上游 `directSlot` 明确排除 shown 槽。
- gpucomp28：`dpuSlot` 改为 LRU + 跳过 `dpuShownSlot`；删除一帧延迟（CA 提交前已完成命令缓冲，
  延迟只增加最多一整帧延迟）。
- 待查：`SR 5 … 3200x2136 pf80` + `OutOfMemory(Device)`（整屏 surface 导入纹理偶发失败）、
  `J surface cache 220 GB`（shadow 回拷）是否影响合成路径成本。

## 13. 2026-10-29：立即翻帧的实测结论（gpucomp28 → gpucomp29）

- gpucomp28 去掉了直扫的一帧延迟，结果“屏幕一变就闪”：CA 的 swap 提交早于其 GPU 工作完成（运行时 `WC0`
  `waitUntilCompleted` 只有 29 次/320 s），DPU 扫到在飞 surface。**一帧延迟是必需的。**
- gpucomp29 恢复延迟，保留 gpucomp28 的槽位纪律修复（LRU + 不换正在扫描的槽），并新增 GPU 空闲采样：
  `UKE DPU: flip … idle=空转/忙碌`（`native_gpu_idle()` = CP ring RPTR==WPTR，由 GpuPowerProbe 导出）。
  该数据决定是否要把延迟改成真正的 completion fence。

## 14. 2026-10-29：真正的完成栅栏（gpucomp30）

- gpucomp29 的 `idle=22/1320` 证明“翻上一帧”并不保证该帧已完成（98% 的 flip 时 GPU ring 非空），
  这解释了转场/模糊处的持续闪烁与开机初期的闪黑。
- gpucomp30：`dpuPrepareFrame`（可睡眠：槽映射/格式）＋`dpuFlipPrepared`（纯寄存器）分离；
  work-loop 定时器 500 µs 轮询 `native_gpu_idle()`（CP ring 排空）后才翻页，30 ms 超时强制翻并计数
  `fence=<排空>/<强制>`；`dpuSlot` 同时避开 shown 与 pending 槽；新帧替换未翻的旧帧。
- CPU 频率进日志：`UKE CPU: CPUFREQ d<0-2>:e… c… p<perf> f<MHz> m<best>@<MHz>`（只读 EPSS 解码，
  GPU 轨默认开启）。

## 15. 2026-10-29：完成栅栏改由插件提供（gpucomp31）

- `CP_RB_RPTR==WPTR` 只表示 CP 读完命令，不是 GPU 完成；gpucomp30 因此退化成无栅栏（“动的都闪”）。
- gpucomp31：`NGP_DIRECT` 模式——display 把帧 offer 给 scanout 客户端，插件用 `NBWaitInFlight(offer时刻)`
  等该帧的 command buffer 完成再 `COMPLETE_SWAP`，display 收到回报才翻 DPU（不拷贝）。
  100 ms 看门狗丢帧保底；旧客户端拒绝该模式 → 回退到延迟一帧的旧路径。
- 插件需重建（CDHash 变化 + owner trust 重生成），本地 Linux 构建修正 `-fuse-ld=lld` 与 `--ld-path` 冲突。
- 待判定：转场/模糊/开机初期是否还闪；`fence=` 计数（本轮 idle 位=客户端回报数，busy 位=看门狗丢弃数）。

## 16. 2026-10-29：gpucomp31 判定与 gpucomp32（顶档重投 + CPU 频率周期化）

- gpucomp31：用户确认“基本不闪，只有零星地方”；日志 `fence=1283/165`（89% 由插件栅栏放行，11% 看门狗丢弃），
  `latch` 1.26 ms（原 4.2 ms），`skip=0`、`fb=0`。
- 帧率仍 ~30：瓶颈在合成器每帧 1 pass/60 draw/6.68 Mpx 的 GPU 工作（27–41 ms）。
- gpucomp32：心跳每 10 s 重投 {@1,9,6}（对抗 GMU 自身 governor 的帧间降档），每 30 s 重打 `UKE CPU:` 行
  （启动时那行会被 msgbuf 尾巴挤掉）。判定项：revote 状态、CPU 三簇频率、帧时间是否下降。

## 17. 2026-10-29：CPU/GPU 频率结论与 gpucomp33

- CPU（EPSS 只读解码）：d0 4×A520 = **1593 MHz**（上限 1900）、d1 A720 = **2611 MHz（满）**、
  d2 X4 = **2803 MHz（满）** → CPU 不是瓶颈。
- GPU：`UKE GPU: revote 1`（周期重投被接受）但帧率不变 → 也不是降频。
- 合成器每帧 32 draw / 1.65 全屏 pass / 6.68 Mpx，`commit-to-complete` 均值 ~51 ms（部分单帧 82–88 ms），
  `J surface cache` 498 GB、`L bk` 100 GB 的阴影回拷 → **30 fps 是合成器自身的 GPU 成本**；
  sheng 的 90–120 建立在 A750 + 一半像素上。
- gpucomp33：看门狗 100→500 ms（模糊场景少丢帧）+ 新增 `gpu=<mean>/<max>` 量 offer→完成延迟。

## 18. 2026-10-29：上游 feat 分支与 gpucomp34

- `origin/feat/ipados27-gpu-parity`（今日 02:41）两个提交：
  (1) half-float 'RGhA' 帧 offer + 客户端 compute 转换（iPadOS 27 需要，我们 17.7.11 不需要）；
  (2) 保留每 surface 的客户端映射（id 在 `NGPSwap.reserved`）——提交说明：每帧映射 ~12000 散页 + 重新 import
      占了 22 ms 提交的大部分。
- gpucomp34 采纳其结论且更彻底：`NGP_DIRECT` 下客户端不需要页，`waitSwap` 直接跳过 `createMappingInTask`。

## 19. 2026-10-29：切到上游拷贝路径（sheng-fb-gpu=2，gpucomp35）

- `90b64ef`（保留每 surface 的客户端映射）已 cherry-pick（`2e28975`）；direct 模式仍额外跳过映射。
- 新 `sheng-fb-gpu=2`：presenter 用 `NGP_BGRX8888`，客户端把帧 GPU-blit 进固件 scanout，DPU 继续扫那块内存
  （`dpuShowBoot()` 在每次完成后把 VIG 指回保存的 GOP 寄存器）。b3a8 帧仍 DPU 直扫。
- 固有代价：blit 与 DPU 扫同一缓冲 → 撕裂风险。此版用于 A/B 对比上游架构与 DPU 直扫。

## 20. 2026-10-29：拷贝路径不可用，回到直扫（gpucomp36）

- `sheng-fb-gpu=2`（客户端 blit 进固件 scanout）实测**直接卡死**：客户端没回报完成，DPU 扫旧内存；
  疑点是无条件 `NBWaitInFlight`（`nmb_command_buffer_wait(handle,0)` 永久等待）与“合成器等 swap”形成的死锁。
- 回到 `sheng-fb-gpu=1`（DPU 直扫），并给 copy 模式加“连续 8 次 offer 无完成 → 自动退回直扫”的保险。
- 上游 90b64ef 的映射缓存与 copy 路径代码保留，待客户端提供有界/非阻塞完成信号后再评估。

## 21. 2026-10-29：gpucomp35 卡死的真实原因（recovery 日志）

- `UKE MEM`：free 2944 MiB(t=30) → 162(t=84) → 38(t=143) → **7.6 MiB(t=253)**，wire 467k→563k 页；
  t=385 jetsam 释放约 1.1 GB（wire→292k）。`flip` 在 t=313.5 停止，触摸/遥测仍在 → 合成器被杀，画面定格。
- 直扫版对照：t=915 时 free 322 MiB、wire 293k 页。
- 拷贝路径（客户端 import 每帧 surface + 保留映射）在这台 4.5 GiB VM 上不可行，已回到 `sheng-fb-gpu=1`。

## 22. 2026-10-29 更正：sheng 的机型与像素数

- sheng = **Xiaomi Pad 6S Pro 12.4，SM8550（Adreno 740），3048×2032（6.19 Mpx）**；我们 3200×2136（6.84 Mpx），
  仅差约 10%；两边 `owned_memory_size` 同为 0x130000000。
- 之前“像素减半/硬件代差”的说法作废。真实差异：GPU 档位（A740 vs A732）、`sheng-gpu-dcvs=1`、
  `sheng-cpufreq=4`，以及他们的拷贝式 scanout（同 VM 大小不 OOM ⇒ 我们 copy 模式的额外 wire 需要定位）。

## 23. 2026-10-29：gpucomp37 先测后调

- 新增 `UKE GPU: clk …`（GPUCC PLL0/PLL1/GFX3D·GMU·HUB RCG 原始寄存器，每 30 s）与插件一次性吞吐基准行
  `B fill=…MB/s copy=…MB/s`（2×27 MB buffer 的 fill/copy 各 4 轮）。
- 目的：把“GPU 慢”从推断变成测量；据此决定下一步压渲染工作还是查栈效率。DCVS 暂不做（投票已是顶档）。

## 24. 2026-10-29：吞吐实测 + 上游 feat 对齐进度

- `B fill=1758MB/s copy=3745MB/s`：27 MB 填充 14.8 ms。每帧约 67 MB 阴影回拷 → ~18 ms，加上渲染
  ≈ 观测的 30–50 ms。**瓶颈是 GPU↔主机内存带宽**。`gfx` RCG 读不到（0x9000），频率仍需另测。
- 上游 `feat/ipados27-gpu-parity`（14 提交）正打这块：cache 维护分阶段计数 + 跳过未缓存导入（注释：1.5 GiB pool
  的维护 41 ms）、pool 存在时不搜连续内存、BO pool 分块增长、按需页表、hangcheck/重启重放。
- 合并：本地 A732 9 个提交已 rebase 到 feat（冲突已解），剩 GMU 池 adopt 与上游新 `GmuSizes/MemoryLayout` 的适配。
- 本地构建：补回被过滤的两条 `-Wno-*`（否则 clang 在错误后断言崩溃）。

## 25. 2026-10-29：gpucomp38（上游 feat 全量 + GMU 池适配，未刷）

- 驱动 rebase 到 `origin/feat/ipados27-gpu-parity`（14 提交）并完成适配：GMU 池只 adopt 5 个 payload、
  页表按需（`NativeAdrenoTablePages`）、GPU 池只 adopt 5 个 buffer、`entries_`→`at(i)`、
  `dropViewsLocked`+`dropViews` 双形态、`native_pmap_user_mappings` 弱桩。
- SMMU kit = feat `30ec63b`；构建需 `--local-module kit:ArmSmmu4K=…/.cache/kit-smmu-feat-git`。
- probe 新增 `UKE GPU: maint pool=…/KiB alloc=… map=… sub=… unmap=… cmp=… tab=…`（上游分阶段计数）。
- 镜像：kernel `f050cf35…`、payload `be3f7cc8…`、vendor_boot `29ddb27b…`、boot `0220bc5a…`（release
  `2026-10-29-gpu-upstream-feat/`），**按要求未刷入**，设备停在 TWRP。

## 26. 2026-10-29：gpucomp38 变 CPU 渲染的根因与 gpucomp39

- 日志：`M … o8/24/4 … d00:0/263`（daemon 未起、24 次 open 被拒），无驱动错误行，显示侧正常。
- 根因：feat 的**每进程地址空间**（`createVm`）依赖 `ttbr0_switched`；本板未就绪 → 用户连接全被拒 → 插件不工作。
- 修复：`mount` 在 `createVm` 失败时回退共享内核 VM（feat 前行为），计数 `mount_fail[2]`；
  新增 `UKE GPU: conn switch=… cp=… mode=… mount=… shared=… tables=…` 遥测。
- 镜像 gpucomp39：kernel `80483408…`、payload `89f348f2…`、vendor_boot `91b8c352…`（已刷）。

## 27. 2026-10-29：feat 版连接路径未通，设备回到 gpucomp37

- gpucomp39 真实 M 行 `o0/4/0`：0 个客户端成功打开；`conn switch=0 cp=0 mode=1 mount=…/NotReady shared=1 tables=12/0`。
- 表切换/CP 检查成功、createVm NotReady、共享回退触发，但连接仍失败 ⇒ feat 的每进程地址空间路径在本板未走通。
- 带宽测量（1.76/3.75 GB/s）来自 gpucomp37（老驱动），结论不受影响。
- 设备刷回 gpucomp37；feat 合并改为独立任务，先补细粒度失败点遥测（createVm/setScope/core->open/service）。

## 28. 2026-10-29 深夜：feat 版跑通但偶发踩坏内核内存（feat 轨暂停）

### feat 版确实跑起来了（gpucomp46，连续 918 s 无 panic）

```
UKE GPU: conn switch=0 cp=0 mode=1 mount=0/0 shared=0 tables=10/1 in=31 out=40
UKE GPU3: M a5.0/5.0 k0 s5/0 r1 o2/0/1 n2
UKE DPU: flip n=4920 … latch=1285us max=2093us fence=4892/28 gpu=12133us max=162318us
```

- `mount=0/0`、`shared=0`、`tables=10/1`：feat 的**每进程地址空间**在 31 位 aperture 下走通
  （`clientWindow()` 把窗口放到 `0x10000000` + 1.75 GiB，各客户端独立页表、可共用同一段）；
  `o2/0/1` = 2 次成功打开、0 次 busy（之前是 `o8/24/4`）。
- 栅栏放行 **99.4%**（4892/4920），看门狗丢弃降到 0.6%（旧驱动 11–12%）；`gpu=12.1 ms`（offer→GPU 完成均值）。
  空闲 flip 10 fps（CA 空闲出帧率，正常）。

### 但随后一次启动 panic：`">maxDepth"`

- 屏幕与寄存器：`panic(cpu 3 caller 0xfffffff007904540)`、`pc 0xfffffff007904548`、`lr 0xfffffff007904540`、
  `esr 0x56000004`（EC=0x15 SVC）、`far 0x0030000c08000041`、`x8 = NativeLegacyDisplay::getMetaClass()`。
- 符号解析（`llvm-nm`）：`0x…04540` = **`OSMetaClass::applyToInstances`+0xf4**；`caller 0x…9154b0` =
  **`OSKext::loadExecutable`**（显示 late probe 里有 `OSKextLoadKextWithIdentifier("com.apple.iokit.IOSurface")`）。
- 反汇编 + XNU 源码定位：`libkern/c++/OSMetaClass.cpp:1118` 的 `panic(">maxDepth")`——当元类的**父类链深度**
  超过 `sDeepestClass` 时断言。
- 结论：这是**内核数据结构被踩坏**的典型症状（元类继承链被写成非法值），不是 GPU 代码自己 panic；
  时间上只出现在 feat 驱动（每进程地址空间 + 按需页表 + CPU 侧 TTBR0 切换）之后，且是**偶发**
  （一次启动 918 s 无恙，下一次在 kext 加载时炸）。

### 处置与后续（feat 轨单独推进）

- 设备回到 **gpucomp37**（老驱动：GPU 渲染、无 panic），feat 的镜像链 gpucomp38–46 保留在
  `uke-port/releases/2026-10-29-gpu-upstream-feat|aperture|lowwin|paniclog/`。
- feat 轨下一步（按顺序，全部要能单独回退）：
  1. **隔离测试**：单客户端（只留 backboardd）、`sheng-gpu-ttbr=1/2` 对照、临时强制共享 VM
     （跳过 `createVm`），看 `>maxDepth` 是否随每进程地址空间消失；
  2. **TTBR0 切换时序**：feat 在 mode 1 下由 CPU 写 TTBR0（`writeTtbr0`），确认它与提交/完成、TLB 失效
     的先后关系（以及 `submit->setTableSwitching` 的 `rangeOutstanding` 条件）；
  3. **踩内存金丝雀**：在 probe 心跳里对一个已知内核结构（如元类链表头或自建哨兵页）做校验并计数，
     把「GPU 活动 ↔ 破坏」关联起来，再决定是否重试 feat。
- panic 捕获链的结论：pram 交接（`uke_panic_flush`）没有产出 `UKE PANIC` 行；从 panic 屏调用内核 `printf`
  只打出了空前缀（Darwin 变参走栈上约定）；可用方案是 panic 屏把文本拷进 probe 导出的
  `uke_panic_shadow`，由心跳线程打 `UKE PANIC: …`。屏幕侧已改为：进屏关控制台、字体 3×、只画消息本身。

## 29. 2026-10-29 收尾：回到可开机基线（gpucomp47）并把主分支对齐

### 结论

- feat 合并（每进程地址空间 + 按需页表 + 维护改动）在设备上**能跑且更快**（栅栏放行 99.4%、`gpu=12.1 ms`、
  918 s 无 panic），但**偶发踩坏内核元类链**（`OSMetaClass::applyToInstances` `>maxDepth`），
  无法作为可交付基线；feat 轨代码保留在驱动仓 `uke-a732-single` / `uke-a732-feat`，测试计划见 §28。
- 当前基线 = **gpucomp37 的代码**：驱动 `uke-a732-cache`（`f1a2aee`），ArmSmmu4K 钉 `3ba9245`。

### 用 git 还原时遇到的两个事实

1. gpucomp37 当时构建自**未提交工作区**：时钟行（`UKE GPU: clk …`）、每 10 s 的 perf `revote`、
   `native_gpu_idle` 与 feat 遥测一起被并进了同一个提交 `f4376f8`。因此没有“正好等于 gpucomp37”的提交；
   用 `git restore --source=f4376f8^` 退到 feat 之前，再把这三段**逐字**摘回（只依赖 GPUCC 窗口与 GMU 句柄）。
2. `native_gpu_idle` 是显示驱动直接取地址的符号（`weak_import` 在本仓的 kext 链接里仍要求解析），
   缺了它链接失败，所以它必须留在 probe 里。

同理，显示仓（copy 对照模式）和 Metal 仓（NGP_DIRECT 栅栏 + 吞吐基准）在 gpucomp37 之后也有未提交改动，
这次一起提交，工作树干净、可由提交复现。

### 相关提交

| 仓库 | 分支 | 提交 | 内容 |
| --- | --- | --- | --- |
| NativeAdreno | `uke-a732-cache` | `f1a2aee` | 可开机驱动（池 adopt 修复） |
| UniXNU-format2 | `uke-gpu-format2` | `27fdaf2` | ArmSmmu4K 钉回 `3ba9245` |
| UniXNU-uke-format2 | `uke-format2` | `3958bad` / `6316412` / `1621160` | probe 退回 feat 之前 + 补回 idle / clk / revote |
| NativeMobileDisplay | `uke-display-slot242` | `cd8a7b2` | `sheng-fb-gpu=2` 对照模式 |
| NativeMetal | `uke-gpu-metal` | `739de5b` | DIRECT 栅栏 + 吞吐基准 |

### 镜像（`uke-port/releases/2026-10-29-gpu-revert/`）

| 文件 | sha256 | 说明 |
| --- | --- | --- |
| `vendor_boot_uke_gpucomp47.img` | `8b493ab2…` | base = gpucomp37 的 vendor_boot（DTB/表不变） |
| `boot_uke_gpucomp47.img` | `0220bc5a…` | UEFI 未变 |
| `payload_uke_gpucomp47.img` | `654524ba…` | 内含补丁后内核 `af1c349f…`（构建内核 `a4e1f996…`） |

`check-gpu-addon-host.py` 全项 PASS：addon blob 21,179,400 B / 12 项，插件三份 CDHash
（`2d7300cd`、`61a38565`、`ee022a5f`）与 gpucomp37 完全一致；payload boot-args 987 B，
`fb-gpu 1`、`surface 1`、`iconservices on`。已刷 `boot_a`/`vendor_boot_a`/`boot_b`/`vendor_boot_b` + `set_active a`。

### 下一步

- 基线验证通过后，feat 轨按 §28 的三步隔离测试推进；性能结论（fill 1.76 GB/s、copy 3.75 GB/s）
  与显示链（DPU 直扫 + 插件栅栏）不受影响。
