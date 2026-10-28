# cliffs(SM7675) MDSS/DPU 寄存器地图与初始化序列（A1/A2）

> DPU-TASKS A1/A2 产物。来源优先级：cliffs DTB dump（`reference/dts-reference/03_dtbdump_Qualcomm_Technologies,_Inc._Cliffs7P_SoC.dts`，
> mdss_mdp 自 :17653 起）+ stock 驱动（`reference/Xiaomi_Kernel_OpenSource-uke/drivers/gpu/drm/msm/`）+ mainline DPU 9.x
> catalog（`drivers/gpu/drm/msm/disp/dpu1/catalog/dpu_9_0_sm8550.h`，同一 IP 代的块锚点）。**活态固件配置以 A3 快照为准**：
> 固件正在扫 GOP 显存，任何“猜”都不如快照里的一行。

## 1. 顶层事实

| 项 | 值 | 出处 |
|---|---|---|
| MDSS/MDP 块 | `qcom,mdss_mdp@0xae00000`，reg 名 `mdp_phys 0xae00000+0x84000` | DTS :17653/:17839 |
| VBIF | `vbif_phys 0xaeb0000+0x2008` | 同上 |
| REGDMA | `regdma_phys 0xaf80000+0x7000` | 同上 |
| IPCC reg | `ipcc_reg 0x400000+0x2000` | 同上 |
| 中断 | GIC SPI 0x53(83)，`interrupt-controller`，DPU 子中断经 `interrupt-parent=<&mdss_mdp>` | DTS :17838 |
| MDP 顶层块（SDE regs） | `qcom,sde-off = <0x1000>` → **0xae01000** | DTS :17842 |
| DPU 版本 | DPU 9.x（mainline `dpu_9_0_sm8550` 同代；`qcom,sde-ubwc-version = 0x40030001` = UBWC 4.0） | DTS :17698 |
| 显示时钟控制器 | `dispcc@0xaf00000`，0x20000 字节，compatible `qcom,cliffs-dispcc` | DTS :11414 |
| 显示 GDSC | `disp_cc_mdss_core_gdsc@0xaf09000`、`disp_cc_mdss_core_int2_gdsc@0xaf0b000`（retain-regs、support-cfg-gdscr） | DTS :13484/:13500 |
| 显示 SMMU | `apps_smmu@0x15000000`（0x100000），`qcom,handoff-smrs = <0x1c00 0x02>` | DTS :13160 |
| 非安全显示流 | SID **0x1c00**，mask **0x02**（0x1c00/0x1c02 共用上下文） | DTS :18178 |
| 安全显示流 | SID 0x1c01（secure display，不用） | DTS :18187 |
| 面板 | O82 3200×2136，`..._o82_36_02_0a_dualdsi_dsc_vid`（**dual-DSI DSC video**） | `dtbo_full.dts:3244/4887` |
| DSI | dsi0 `@ae94000`，dsi1 `@ae96000`；PHY0 `@ae95500`，PHY1 `@ae97500` | DTS :17919/:17948/:17977/:18010 |
| 显示数据总线 | `interconnect-names = "qcom,sde-data-bus0","qcom,sde-ebi-bus","qcom,sde-reg-bus"`；`qcom,sde-ib-bw-vote = <0x2625a0 0x00 0xc3500>`；max 0x1945ba0 kbps | DTS :17655/:17657 |

## 2. 块偏移表（cliffs DTS 原值 vs MDP-相对值）

`qcom,sde-*-off` 是相对 **0xae00000** 的字节偏移；DPU 代码把 **0xae01000**（sde-off）作为映射基址，
所以代码偏移 = DTS 值 − 0x1000。sheng(SM8550, DPU 9.x) 的已知可用锚点与 cliffs 的 MDP-相对值**逐项相同**——
这就是“结构照抄 sheng、寄存器按 cliffs 考古”能成立的原因（A3 只需再核一次活值）。

| 块 | DTS 偏移（相对 0xae00000） | 代码偏移（相对 0xae01000） | 绝对地址 | sheng 锚点 |
|---|---|---|---|---|
| MDP top（HW_VERSION/IRQ） | 0x1000（sde-off） | 0x0 | 0xae01000 | 0x0 |
| SSPP VIG0 | 0x5000 | 0x4000 | 0xae05000 | 0x4000 |
| SSPP VIG1 | 0x7000 | 0x6000 | 0xae07000 | 0x6000 |
| SSPP DMA0..4 | 0x25000/0x27000/0x29000/0x2b000/0x2d000 | 0x24000../0x2c000 | 0xae26000.. | 0x24000.. |
| CTL0..3 | 0x16000/0x17000/0x18000/0x19000（size 0x1000） | 0x15000..0x18000 | 0xae17000.. | 0x15000 |
| INTF0..3 | 0x35000(dp)/0x36000(dsi0)/0x37000(dsi1)/0x38000(dp)，size 0x300 | 0x34000/0x35000/0x36000/0x37000 | 0xae36000/0xae37000/0xae38000/0xae39000 | 0x35000(INTF1)/0x36000(INTF2) |
| LM0..3（mixer） | 0x45000/0x46000/0x47000/0x48000，size 0x400 | 0x44000..0x47000 | 0xae46000.. | 0x44000 |
| PP0..7 | 0x6a000/0x6b000/0x6c000/0x6d000/0x67000/0x67400/0x7f000/0x7f400 | 0x69000/0x6a000/0x6b000/0x6c000/0x66000/0x66400/0x7e000/0x7e400 | 0xae6a000.. | 0x69000 |
| DSPP0..2 | 0x55000/0x57000/0x59000 | 0x54000/0x56000/0x58000 | 0xae55000.. | — |
| WB1/2 | 0x65000/0x66000 | 0x64000/0x65000 | 0xae66000.. | — |
| DSC | 0x81000/0x81000/0x82000 | 0x80000/0x80000/0x81000 | — | — |
| CDM | 0x7a200 | 0x79200 | — | — |
| MERGE3D | 0x4f000/0x50000/0x67700/0x7f700 | 0x4e000/0x4f000/0x66700/0x7e700 | — | — |

管道能力（DTS）：7 条 SSPP = VIG0/VIG1 + DMA0..4；VIG 有 qseed 缩放（`sde-vig-qseed-off=0xa00`）、
CSC（0x1a00）、IGC（0x1d00）、top 0x700。`sde-sspp-xin-id = <0 4 1 5 9 13 14>`。
`qcom,sde-mixer-display-pref = "primary","primary",...`：LM0/LM1 是主显示。

## 3. SSPP 寄存器（VIG 通用，stock `dpu_hw_sspp.c` 定义）

| 寄存器 | 偏移 | 说明 |
|---|---|---|
| `SSPP_SRC_SIZE` | 0x00 | (height−1)<<16 ǀ (width−1) |
| `SSPP_SRC_XY` | 0x08 | (y)<<16 ǀ x |
| `SSPP_OUT_SIZE` | 0x0c | 输出矩形 |
| `SSPP_OUT_XY` | 0x10 | 输出位置 |
| `SSPP_SRC0_ADDR` | 0x14 | 平面 0 源地址（**32 位**） |
| `SSPP_SRC1..3_ADDR` | 0x18/0x1c/0x20 | 其它平面 |
| `SSPP_SRC_YSTRIDE0/1` | 0x24/0x28 | 每行字节（低位 16 位） |
| `SSPP_SRC_FORMAT` | 0x30 | 位 31:30 fetch mode（0=linear）；位 22 solid-fill；位 15 YUV；位 11 ROT90；bpp 位 9.. |
| `SSPP_SRC_UNPACK_PATTERN` | 0x34 | 分量 unpack 顺序 |
| `SSPP_SRC_OP_MODE` | 0x38 | 位 13/14 flip，位 0 BWC(UBWC) |
| `SSPP_SRC_CONSTANT_COLOR` | 0x3c | solid-fill 颜色 |
| `SSPP_FETCH_CONFIG` | 0x48 | UBWC fetch 配置 |
| `SSPP_UBWC_STATIC_CTRL` | 0x44 | UBWC static/swizzle/highest-bank-bit |
| `SSPP_DANGER/SAFE/CREQ_LUT` | 0x60/0x64/0x68 | QoS |
| `SSPP_SRC_ADDR_SW_STATUS` | 0x70 | 地址切换状态（vblank 切换用） |

## 4. CTL / INTF / PP / LM 关键寄存器

| 块 | 寄存器 | 偏移 | 说明 |
|---|---|---|---|
| CTL | `CTL_LAYER(lm)` | 0x00 + 4·lm（LM5 特殊 0x24） | 层 mixer 配置 |
| CTL | `CTL_TOP` | 0x14 | intf 选择、3D 模式、stream sel |
| CTL | `CTL_FLUSH` | 0x18 | flush 位：VIG0=bit0，VIG1=bit1，…… |
| CTL | `CTL_START` | 0x1c | start（视频模式通常不写） |
| CTL | `CTL_PREPARE` | 0xd0 | prepare |
| CTL | `CTL_FETCH_PIPE_ACTIVE` | 0xfc | 活跃 fetch 管道 |
| CTL | `CTL_INTF_FLUSH` | 0x110 | INTF flush 位 |
| CTL | `CTL_INTF_MASTER` | 0x134 | intf master 选择 |
| INTF | `INTF_TIMING_ENGINE_EN` | 0x000 | 时序引擎使能（DPU 9 用 `INTF_STATUS` bit0 判活跃） |
| INTF | `INTF_CONFIG` | 0x004 | 接口配置 |
| INTF | `INTF_HSYNC_CTL` | 0x008 | hsync |
| INTF | `INTF_VSYNC_PERIOD_F0` | 0x00c | 帧周期（行） |
| INTF | `INTF_DISPLAY_V_START/END_F0` | 0x01c/0x024 | 时序 |
| INTF | `INTF_ACTIVE_V_START/END_F0` | 0x02c/0x034 | 有效区 |
| INTF | `INTF_DISPLAY_HCTL/ACTIVE_HCTL` | 0x03c/0x040 | 水平时序 |
| INTF | `INTF_FRAME_LINE_COUNT_EN` | 0x0a8 | 帧/行计数使能（写 0x3） |
| INTF | `INTF_FRAME_COUNT` | 0x0ac | 帧计数（活态证明） |
| INTF | `INTF_LINE_COUNT` | 0x0b0 | 行计数 |
| INTF | `INTF_STATUS` | 0x26c | bit0 = 活跃 |
| INTF | `INTF_TEAR_TEAR_CHECK_EN` | 0x284 | tear check |
| INTF | `INTF_TEAR_AUTOREFRESH_CONFIG` | 0x2b4 | auto refresh |
| PP | `PP_TEAR_CHECK_EN` | 0x000 | |
| PP | `PP_SYNC_CONFIG_VSYNC` | 0x004 | |
| PP | `PP_OUT_LINE_COUNT` | 0x028 | |
| PP | `PP_LINE_COUNT` | 0x02c | |
| PP | `PP_AUTOREFRESH_CONFIG` | 0x030 | |
| LM | `LM_OP_MODE` | 0x000 | |
| LM | `LM_OUT_SIZE` | 0x004 | |
| LM | blend stage 0 | 0x020 | `qcom,sde-mixer-blend-op-off = <0x20 ...>` |

## 5. 显示时钟树（dispcc@0xaf00000 + gcc）——B1 用

驱动：`drivers/clk/qcom/dispcc-pineapple.c`（compatible `qcom,cliffs-dispcc`），GCC 分支在 `gcc-cliffs.c`。
DT 的 `clock-names` → 驱动时钟 → 寄存器（off 为相对 dispcc/gcc 基址）：

| DT 名 | 驱动时钟 | 使能寄存器 | 备注 |
|---|---|---|---|
| `gcc_bus` | `gcc_disp_hf_axi_clk` | GCC **0x2700c bit0** | AXI |
| `iface_clk` | `disp_cc_mdss_ahb_clk` | dispcc **0x80a4 bit0** | AHB |
| `branch_clk` | `disp_cc_mdss_mdp_clk` | dispcc **0x800c bit0** | MDP 分支 |
| `core_clk` | `disp_cc_mdss_mdp_clk_src` | RCG **0x80d8** | 514 MHz（DT clock-rate 0x1ea30480；max 600 MHz） |
| `vsync_clk` | `disp_cc_mdss_vsync_clk` | dispcc **0x8024 bit0** | 19.2 MHz |
| `lut_clk` | `disp_cc_mdss_mdp_lut_clk` | dispcc **0x8018 bit0** | 514 MHz |
| `iface`（dispcc 自身） | — | — | 来自 GCC，dispcc 节点的 `clocks` |

RCG 编程按 `docs/TOUCH-PORT.md` 的 GCC 写法（RCG + CMD_UPDATE 轮询 + 回读校验）；**B1 只读校验，不改频率**。
GDSC `disp_cc_mdss_core_gdsc@0xaf09000` / `core_int2@0xaf0b000`：GDSCR bit31 = PWR_ON（与 UFS/GCC 同款判断）。

## 6. A2 · stock 初始化序列 × 寄存器 × 源码行号

stock 树的 DPU 驱动只到 DPU 7.x（`dpu_hw_catalog.c` 到 `DPU_HW_VER_720`），**vendor 的 SDE techpack 不在这次开源的树里**；
因此下表以 stock 的公共序列 + mainline DPU 9.x 块模型为准，cliffs 的实际寄存器值由 DTB 提供，实际在跑的值由 A3 抄。
（这也是 DPU-TASKS 把 A3 定为“黄金材料”的原因。）

| # | 步骤 | 做什么 | 寄存器/锚点 | 源码 |
|---|---|---|---|---|
| 1 | 互连最小带宽 | `msm_mdss_icc_request_bw(MIN_IB_BW=400 MB/s)` | interconnect path `mdp0-mem`/`mdp1-mem` | `msm_mdss.c:179-188` |
| 2 | 时钟批量开 | `clk_bulk_prepare_enable`：gcc_disp_hf_axi + dispcc ahb/mdp/lut/vsync | 见 §5 | `msm_mdss.c:190` |
| 3 | 读 HW_REV | 读 `0xae00000+0x0`（MDSS 顶） | `HW_REV 0x0` | `msm_mdss.c:44/205` |
| 4 | UBWC 静态配置 | 按版本写 `UBWC_STATIC(0x144)` 等（DPU 9 由 SDE 初始化做） | 0x144/0x150/0x154 | `msm_mdss.c:210-228` |
| 5 | DPU 资源初始化 | `pm_runtime_resume_and_get` → `dpu_hw_*_init`（SSPP/LM/PP/INTF/CTL） | 块偏移见 §2 | `dpu_kms.c:1016-1066`、`dpu_rm.c:125/160/184/219` |
| 6 | 管道编程 | `setup_format`（SRC_FORMAT/UNPACK/OP_MODE）、`setup_sourceaddress`（SRC0..3/YSTRIDE） | SSPP 0x30/0x34/0x38/0x14../0x24 | `dpu_hw_sspp.c:242-352/451-530` |
| 7 | solid-fill（可选） | `setup_solidfill` 写 `SRC_CONSTANT_COLOR`，format bit22 | 0x3c + 0x30 bit22 | `dpu_hw_sspp.c:558-573` |
| 8 | mixer/PP | `setup_blendstage`、PP tear/sync | LM 0x20..；PP 0x0/0x4/0x28/0x30 | `dpu_hw_ctl.c:380-...`、`dpu_hw_pingpong.c` |
| 9 | INTF 时序 | `setup_timing_gen` → `INTF_TIMING_ENGINE_EN`、HSYNC/VSYNC/ACTIVE 区、计数使能 | §4 | `dpu_hw_intf.c:239-341`、`dpu_encoder_phys_vid.c:404-438` |
| 10 | CTL intf 绑定 | `setup_intf_cfg` 写 `CTL_TOP`（intf 编号、stream sel） | CTL 0x14 | `dpu_hw_ctl.c:558-588` |
| 11 | flush/latch | `CTL_FLUSH` 写挂起位（VIG0/1 = bit0/1）；视频模式面板自行刷新，flush 在下一次帧边界生效（sheng 实测 5.4 ms） | CTL 0x18 | `dpu_hw_ctl.c:131-151`、sheng `NativeLegacyDisplay.cpp` 注释 i562 |
| 12 | 关断 | `clk_bulk_disable_unprepare` + 带宽 0 | — | `msm_mdss.c:231-236` |

## 7. A4 · 面板连接结论

- 主显示 `qcom,dsi-display-primary` 同时引用 **dsi0 + dsi1**（`qcom,dsi-ctrl = <&mdss_dsi0 &mdss_dsi1>`，
  `qcom,dsi-phy = <&mdss_dsi_phy0 &mdss_dsi_phy1>`），面板节点是 **dualdsi DSC video**：
  **面板 = INTF1(DSI0) + INTF2(DSI1)，双链路一帧左右半屏**，DPU 侧对应 VIG0/VIG1 两条管道 + 同一个 CTL（pp-split / single-flush）。
- 待 A3 快照确认：两条 INTF 的 `INTF_STATUS` bit0、`INTF_FRAME_COUNT` 是否都在走；VIG0/VIG1 的 `SRC_XY`
  是否一左一右（x=0 / x=1600）；CTL0 `CTL_LAYER`/`CTL_INTF_MASTER` 是否只有 CTL0 在用。

## 8. A3 快照读表（探针按此 dump）

MDP 映射基址 **0xae01000**，长度 **0x8f000**（sheng 同款，覆盖到 0xae90000；cliffs mdp_phys 0x84000 也在内）。

1. MDSS 顶：`0xae00000 + 0x0`（HW_VERSION）、`+0x10`（HW_INTR_STATUS）。
2. MDP 顶：`0xae01000 + 0x0`（MDP HW_VERSION）、`+0x10`（INTR_EN）、`+0x14`（INTR_STATUS）。
3. 7 条 SSPP：`size/xy/out_size/out_xy/src0..3/ystride0/1/format/unpack/opmode/fetch/ubwc/qos/const_color/addr_status`。
4. CTL0..3：`LAYER0..3/TOP/FLUSH/START/PREPARE/FETCH_PIPE_ACTIVE/INTF_FLUSH/INTF_MASTER`。
5. INTF1/2：§4 的时序与计数寄存器全部。
6. PP0..3：`TEAR_CHECK/SYNC_CONFIG/OUT_LINE/LINE/AUTOREFRESH`。
7. LM0/1：`OP_MODE/OUT_SIZE/BLEND0`。
8. 两次采样（间隔 1 s）的 `FRAME_COUNT/LINE_COUNT`，证明活扫描。
9. SMMU：sCR0/IDR0/IDR1 + SID 0x1c00 的 SMR/S2CR/CBAR/CBA2R/SCTLR/TTBR0/TCR（只读，B2 依据）。
10. GOP：`PE_current_console` 的物理基址/长度/宽高/rowBytes/format（与管道源地址对照 = 抄固件的关键）。

## 9. B2 · surface 寻址结论（代码考古）

- DPU 的管道地址寄存器是 **32 位**；XNU 的 VM 全在 4 GiB 以上（board.toml `dram_base=0x8C0000000`），
  所以直扫合成器 surface 必须给显示 SMMU 的上下文建 **stage-1 窗口**，把高页映射到 32 位 IOVA。
- sheng 的 `ShengUfs native_ufs_dpu_map`（commit f2d6a4a）就是上游实现：在显示 bank（SID 0x1c00/0x1c02，
  CBAR type 1、SCTLR 0xe0=翻译关）上建 L1/L2/L3 表，保留 `0x80000000..0xffffffff` 两个 1 GiB 恒等块
  （固件 GOP 扫描不变），并加 4 个 32 MiB 槽在 IOVA `0x10000000..0x17ffffff`；`native_ufs_dpu_map(slot,pages,count)`
  填 L3 + TLBIALL/TLBSYNC；打开翻译后 50 ms 内看 FSR，第一笔 fault 就整体回退。
- uke 的 `drivers/NativeUfs` 具备同一套 `mapPhys/read32/write32` 基础设施；B2 = 把该窗口原样移植进
  `NativeUfs`，ABI 名保持 `native_ufs_dpu_map/native_ufs_dpu_state/native_ufs_dpu_smmu/native_ufs_dpu_check`，
  显示侧（GopFramebuffer fork）无需改调用点。
- cliffs 与 sheng 的 SID 都是 0x1c00/0x1c02，`handoff-smrs` 也在 DTB 里给出——这是 B2 可行的直接证据；
  A3 的 SMMU 行负责最后确认 bank 形状（CBAR/SCTLR/FSR）。

## 10. 待办与门禁

- A3 探针：`package/DpuProbe.cpp`，boot-arg `uke-dpu=<n>`，只读；`UKE DPU:` 行。
- C1 门禁：`scripts/check-dpu-probe-host.py`（源码常量 ↔ DTS 对照 + 归档符号/字符串）。
- C2：`uke-dpu-test=<n>`（1=原地写回+latch，2=solid-fill，3=窗口内测试图案，4=低 DRAM 尾图案）；
  全部保存原值、定时回滚、失败只影响一帧/一屏。
- M1/M2 判定项见 `docs/DPU-PORT.md`（E2 收口时建立），当前先在 `MIGRATION-WORKLOG.md` 记录。
