# uke DPU 直扫（DPU-PORT）

状态：**M1 ✅、M2 ✅、C3（1600×1068）✅、B2 stage-1 窗口 ✅、D1b BGRA 直扫 ✅（约 10 fps，部分场景花屏）；60/120 路线见 §8。**
任务清单：`docs/DPU-TASKS.md`；寄存器地图/考古：`docs/dts-reference/mdss-cliffs-map.md`；
台账：`out/migration-uke/MIGRATION-WORKLOG.md`（core 仓）。

## 1. 路线（官方文档 → 参考实现）

- unixnu.com/zh/porting/gpu：显示集成属于板级输入（扫描输出布局），先把“基础显示呈现”跑通；
  GPU 结果与桌面呈现分开记录。
- unixnu.com/zh/drivers/adreno740：GopFramebuffer 负责 swap 的提供/校验/回收和 **scanout 交接**。
- 上游实现：sheng 的 `GopFramebuffer@33e941a`（`sheng-dpu-scanout=2`，VIG0/1 + CTL0 flush +
  失败回退）作为结构样板；ShengUfs f2d6a4a 的 display bank stage-1 窗口作为 surface 寻址；
  寄存器按 cliffs DTB 考古（`mdss-cliffs-map.md`）。
- 关键考古：cliffs 的 DTS 块偏移减 sde-off(0x1000) 与 sheng DPU 9.x 锚点逐项相同
  （VIG0/1 0x4000/0x6000、CTL0 0x15000、INTF1/2 0x35000/0x36000、PP0 0x69000、LM0 0x44000）。

## 2. 实现

| 项 | 文件 | boot-arg | 说明 |
|---|---|---|---|
| A3 只读快照 | `package/DpuProbe.cpp` | `uke-dpu=<1\|2>` | 线程延时 8s（`uke-dpu-delay`）读 MDSS/MDP/VIG/DMA/CTL/INTF/PP/LM/GOP；level2 加 dispcc/GCC 与 apps_smmu |
| C2 管道受控 | `package/DpuProbe.cpp` | `uke-dpu-test=<1..4>` | 1=原值写回+latch；2=solid-fill；3=stage-1 窗口内 3200×2136 渐变；4=低 DRAM splash 尾部 2048×1024 渐变；全部存后写、5s 回滚 |
| B2 显示 bank 窗口 | `drivers/NativeUfs/NativeUfs.cpp` | `uke-dpu-scanout>=1` 或 `uke-dpu-test>=3` | ShengUfs `native_ufs_dpu_map` 原样移植：SID 0x1c00/0x1c02、bank stage-1、恒等 0x80000000..0xffffffff、4×32MiB @ 0x10000000 |
| M1 门禁 | `scripts/check-dpu-probe-host.py` | — | DTS 锚点/stock 寄存器/窗口符号/归档字符串四项 |

现有可用链不受影响：所有 DPU 代码默认关（参数缺省即不映射/不读写）；presentBase 的 CPU 拷贝、
`'b3a8'` 解码、触摸、GPU mode 9 保持不变。DPU 测试镜像只在启动参数里加 `uke-dpu-*`。

## 3. 测试镜像阶梯（`uke-port/releases/2026-10-28-dpu-a3c1c2/`）

| 镜像 | 参数 | 目的 |
|---|---|---|
| `vendor_boot_uke_dpu_a3.img` | `uke-dpu=1` | M1：只读快照（含 `window`/`smmu` 行），日志出 `UKE DPU:` 行 |
| `vendor_boot_uke_dpu_a3l2.img` | `uke-dpu=2` | M1+：加 dispcc/GCC 时钟与扩展管道字段 |
| `vendor_boot_uke_dpu_c2_1.img` | `uke-dpu=1 uke-dpu-test=1` | M2a：写通路 + latch 计时（画面不变） |
| `vendor_boot_uke_dpu_c2_2.img` | `uke-dpu=1 uke-dpu-test=2` | M2b：solid-fill 可见色块（5s 后恢复） |
| `vendor_boot_uke_dpu_c2_3.img` | `uke-dpu=1 uke-dpu-test=3`（NativeUfs 见 test>=3 自动开窗口） | M2：源地址改到窗口内渐变 buffer（5s 后恢复） |

`boot_uke_dpu_a3.img` 与 framepresent4 的 boot.img 逐字节相同（仅 UEFI）；刷写时两者都可配。
framepresent4 的镜像与记录**不动**。

## 4. M1 判定（A3 快照）

刷 `vendor_boot_uke_dpu_a3.img`（+ boot），抓 `dmesg`/串口日志，确认：

1. 有一行 `UKE DPU: probe=1 stage=1 mdss=… mdp=…`，且 `mdp=` 非 0/非 ffffffff（DPU 在跑）。
2. `UKE DPU: gop base=… 3200x2136 rb=12800`（与 presentBase 的 GOP 一致）。
3. VIG0/VIG1 至少一条 `size=08570c7f`（3200×2136）或半宽；`src=` 的低 32 位等于
   `gop base`（或落在 0xe3940000..0xe6440000）⇒ **固件确实用 DPU 扫 GOP 显存**。
4. `UKE DPU: window state=…`（未开窗口为 0）与 `UKE DPU: smmu sid=1c00 … cbar=… sctlr=…`；
   `cbar` 高 16 位 type=1、`sctlr` bit0=0 时 B2 的窗口前提成立。
5. `UKE DPU: live frame=…->…` 两秒内帧计数增长 1..120（活扫描）。
6. `UKE DPU: probe done stage=3`。
7. 与 `mdss-cliffs-map.md` 的块偏移逐项对上（门禁已静态核过，此处核活值）。

判定结果回填 `porting.toml` display 区与台账；M1 绿后进入 M2。

## 5. M2 判定（管道受控）

按 3→1→2→3 顺序刷（每步一个镜像）：

- **c2_1**：`UKE DPU: test=1 pipe=… latch_us=…`，latch_us 应为 ms 级（sheng 实测 5.4 ms），
  且 `final=0`；画面不变化。
- **c2_2**：屏幕整屏（或双 pipe 各半）显示不透明红色约 5 秒，随后恢复；
  日志 `UKE DPU: test=2 pipe=… color=ffff0000 latch_us=…` 与 `restored=1`。
  若颜色偏蓝 = ABGR 通道序，记录即可（不影响“受控”结论）。
- **c2_3**：屏幕显示 3200×2136 渐变约 5 秒后恢复；日志
  `UKE DPU: test=3 iova=10000000 pages=… bytes=… latch_us=…` 与 `restored=1`。
  `iova` 必须是 0x10000000..0x17ffffff；`skip reason=window` 表示 SMMU 窗口没起来，
  转 c2_4（低 DRAM 尾部）或先按 A3 的 `UKE DPU: smmu …` 行核对 bank。
- **失败即回滚**：任何测试只影响当前屏幕最多 5 秒；不写 flash、不改时钟/GDSC/SMMU 配置
  （窗口失败自动 undo）。若面板卡死，重启；固件会重新配置 DPU。

## 6. 下一步（C3/C4/D/E）

- **C3**：在同一窗口/管线上换 stride/format（先满屏 BGRA，再非满屏），三图案色位验收。
- **C4**：双缓冲 flip 与 latch 时序（sheng 的 `native_legacy_direct` 计数器直接照搬）。
- **D1**：合成器 `'b3a8'` 双平面 surface——DPU 大概率不认，走 GPU 解码到线性 BGRA 再直扫
  （GPU 轨 GpuScanoutClient 合流点）；或固件管道 format 字段证明 DPU 认压缩格式。
- **D2 ✅ 初步**：`presentBase` 的 BGRA 路径已把 surface 页映射进 stage-1 窗口并 flip VIG0/1
  （`uke-dpu-scanout=3`），用户实测 **~10fps、明显变流畅**、大部分场景正常；部分场景花屏待查
  （疑似 surface 被 GPU 复用/未 fence，CPU 拷贝的 150ms 以前掩盖了它）。
- **E1/E2**：1h 长跑、panic/诊断页共存、porting.toml 证据、本文档收口。

## 7. 风险与已知限制

1. **面板时序私有性**：INTF/PHY 由固件配置，我们只在 C2 中改 VIG 源/格式，不动 INTF 时序；
   若 C2 后画面不恢复，重启即可（不写任何持久状态）。
2. **双 DSI**：主显示是 dual-DSI（INTF1+INTF2），A3 会确认 VIG0/VIG1 的左右半屏关系；
   C2 测试同时写两条 pipe。
3. **stage-1 窗口 bank 形状**：若 cliffs 的 display bank 不是 CBAR type 1 / SCTLR 0xe0，
   `native_ufs_dpu_state=0xdead0001`；A3 的 `smmu` 行给出实际值，再按 sheng 的分支调整。
4. **DPU 版本差异**：stock 树只到 DPU 7.x，mainline 9.x 与 sheng 实测是当前依据；A3 的
   `mdp=`/寄存器回读是最终裁决。
5. **带宽**：C2 只扫测试图案，不评估 1.6 GB/s 持续读；B3 在 D2 前用长跑/花屏检查。

## 8. 走向 60/120 fps（2026-10-29 实测后）

### 8.1 现状

D1b（`uke-dpu-scanout=3`）已让 `presentBase` 直接映射 `'BGRA'` IOSurface 的物理页到 stage-1 窗口并
flip VIG0/VIG1，**无 CPU 拷贝**；用户实测约 **10 fps**、大部分场景正常、部分场景花屏。
DPU 本身不是瓶颈：每帧只有地址写 + CTL0 flush（test=3 latch 1196 µs；A3 的 INTF 帧计数 120/s）。
10 fps 的证据是合成器：`UKE GPU3: M a5.0/5.0 k0 s5/0 r1 o0/0/0`——插件挂载但**没有客户端打开
AGX/Metal 设备**，UI 仍是软件渲染。

### 8.2 改动清单（按贡献排序）

| 优先级 | 改动 | 位置 | 验收 |
|---|---|---|---|
| P0 | 每帧计数 + 每秒 `UKE DPU: flip n fps=`；区分合成器提交/显示路径 | `NativeLegacyDisplay.cpp` `presentBase/dpuPresentBgra` | 能判断 10 fps 是提交还是丢帧 |
| P1 | **让合成器走 GPU（最大头）**：`o>0`、`K68xx` | `drivers/NativeMetal`、`add-gpu-plugin-data.py --publish/--clients`、addon blob/trust；`sheng-gpu-metal=1 sheng-gpu-addon=1 sheng-iconservices=1 sheng-fb-gpu=1` | `UKE GPU3` 的 `o` >0，插件 `K68xx`，backboardd/CARenderServer 用原生 Metal |
| P2 | **b3a8 → 线性 BGRA 的 GPU 解码（D1-b）** | AdrenoMetal `GpuScanoutClient`/插件加两平面 XR10→BGRA pass；或验证 DPU 直读 `RGB10A8_2P_XR10` | b3a8 不再 CPU 解码；颜色与 CPU 解码抽样一致 |
| P3 | **vsync 120**：`startVsync/vsyncTick/getRefreshAttributes` | 从 `GopFramebuffer@33e941a` 移植进 `NativeMobileDisplay`；`sheng-vsync=120` | render server 按 120 Hz `SwapWait`，无 busy-wait |
| P4 | **D3 非阻塞 flip**：`CTL_FLUSH` 轮询改 workloop timer，异步 retire | `NativeLegacyDisplay.cpp`（照 sheng `latchTick`） | presentBase 返回 <1 ms；无 swap 积压 |
| P5 | **B3 带宽投票**：60 Hz 1.64 GB/s / 120 Hz 3.28 GB/s；`max-bw-low` 2.99 / `max-bw-high` 3.31 GB/s | `ShengPlatformPower`/`ShengGmuVotes` 或显示驱动；`qcom,sde-data-bus0`/`ebi` | 120 Hz high 档；长跑无 underrun/花屏 |
| P6 | **B1 显示时钟**：`mdp_clk` 514→600 MHz 或确认余量；GDSC on | `dispcc` 0x800c/0x80d8 | 120 Hz 读数稳定 |

### 8.3 不做的事

- 不改 INTF/PHY 面板时序（固件已 120 Hz，A3 已证明）。
- 不改 DPU 的已有 stage-1 表/带宽外的东西；P5/P6 是新增投票，不是重配。
- b3a8 若 DPU 不认，按 P2 走 GPU 解码，不在 CPU 解码上继续优化。

### 8.4 判定数字

- 3200×2136×4 B = **27.34 MB/帧**；
- 60 Hz = **1.64 GB/s**，120 Hz = **3.28 GB/s**；
- DPU latch 目标 < **8.3 ms/帧**（120 Hz 周期）；当前 test=3 为 1.2 ms。
