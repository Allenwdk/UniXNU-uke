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
- **P2 b3a8 → 线性 BGRA 的 GPU 解码（D1-b 合流）**：合成器上 GPU 后主 surface 是 `'b3a8'`
  （RGB10A8_2P_XR10 双平面），当前 D1b 只直扫 `'BGRA'`，b3a8 仍走 CPU 解码 150–300 ms/帧。
  在 AdrenoMetal 的 GpuScanoutClient/插件里加一个“两平面 XR10 → 线性 BGRA”的 GPU pass，
  输出到 `NativeLegacyDisplay` 的直扫 buffer；或先验证 DPU 对 `RGB10A8_2P_XR10` 的直接支持。
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
