# 触控（Novatek NT36532）移植记录 — uke

触摸控制器是 Novatek NT36532（SPI），面板 o82。驱动按上游 sheng（UniXNU-sheng）架构移植：`NativeNovatekTouch`（协议 + 纯算法，vendored 在设备仓 `drivers/NativeNovatekTouch`）+ `NativeGeniSpi`（GENI SPI 引擎，核心仓子模块）+ `package/TouchProbe.cpp`（板级探针/帧循环）+ `package/TouchBoard.h`（板级常量）+ `NativeHidInput`（HID 数字化源，核心仓子模块）。

## 现状（2026-10-25）

| 环节 | 状态 | 证据 |
| --- | --- | --- |
| 时钟（QUP wrap0 投票 + S4 RCG 编程 TCXO 19.2 MHz） | ✅ 实机通过 | `UKE TCH: clocks+pins ok` / `geni+rcg ok rcg… 19200000Hz d1` |
| 引脚（GPIO16-19 qup0_se4_l0..l3、6 mA） | ✅ | 同上 |
| GENI SPI 初始化 + 芯片识别 | ✅ | `identify ok id…`（NT36532） |
| 固件下载（o82 CSOT，16 分区，CRC 校验） | ✅ | `firmware downloaded crc ok`、`scanning v11 pid5993 60x40` |
| 帧循环（轮询模式） | ✅ 长跑稳定 | `f119123` 帧全有效，摸屏 ~58 fps |
| 触摸算法 → 接触点/坐标 | ✅ | `c1 c2`、坐标跟手指（如 `3017,11373`） |
| 边缘钩子（edge::install） | ✅ | `edge1 99/6`（99 次接触、6 次始于边缘） |
| HID 报告 → IOHIDEventDriver | ✅（5f3a37c 起） | `drv1 rpt7004 r0 hid7004` |
| 有 UI 时的长稳 + 真实手势 | ⏳ touch5b 判定中 | 之前 588c96f 在 UI 出现 ~1 分钟后触摸线程死锁 |

**关键结论：驱动管线（时钟→引脚→SPI→固件→帧→接触→HID 报告→事件驱动）已全部在真机打通；剩余判定只有"真实 UI 下持续滑动/上滑手势"。**

## 硬件事实（来源：`uke-port/data/uke-live.dts` + 小米内核 `gcc-cliffs.c`/`pinctrl-cliffs.c`）

- 触摸控制器挂 `qupv3_se4_spi: spi@a90000`（SE 0xa90000，wrapper `qcom,qupv3_0_geni_se@ac0000` = **QUP wrap 0**），`novatek@0`，SPI mode 0、19.2 MHz；`iommus = <&apps_smmu 0xa3 0>`（SMMU 0x15000000/0x100000）。
- **wrap0 分支**（vote 0x52008）：core_2x bit18 / core bit19 / m_ahb bit20 / s_ahb bit21 / s4 bit26；halt：0x23018 / 0x23008 / 0x23000 / 0x23004 / 0x183bc。
- **wrap0 S4 源时钟 RCG `cmd_rcgr 0x183c8` 是普通 freq_tbl RCG（非 DFS）**：探针直接编程 `F(19200000, P_BI_TCXO, 1, 0, 0)`（CFG + CMD_UPDATE 自清 + 回读解码校验），`GENI_CLK_SEL=0`。parent_map_0：TCXO=0、GPLL0 main=1（600 MHz）、GPLL0 even=6（300 MHz）。GPLL0 vote 0x52020 bit0（fixed Lucid OLE，常开，不做锁位门控）。
- **TLMM 基址 0x0f100000** = DT 节点 `pinctrl@f000000` + pinctrl-cliffs.c `REG_BASE 0x100000`（**每 GPIO 块起点**；写成 0x0f000000 会读未背线区域并挂总线 → 无 panic 复位）。ctl=`+0x1000*gpio`、io=`+4`、mux_bit 2、drv_bit 6、功能槽 0=gpio。
- 引脚：MISO=16（`qup0_se4_l0`）、MOSI=17（`l1`）、CLK=18（`l2`）、CS=19（`l3`），mux 值 1，6 mA（drive 编码 2）；`novatek,irq-gpio = <&tlmm 54 0x2001>`；`novatek,lcd_id-gpio = 26`。
- 触摸中断：TLMM 引脚中断（GPIO54）→ TLMM 摘要 SPI 208（`pinctrl@f000000 interrupts=<0 0xd0 4>`）→ **INTID 240**；`armcpu_spi_attach` 在核心仓 `drivers/ArmCpu/ArmCpu.cpp` 已导出（弱引用，缺失时自动回退轮询）。
- 矩阵 60×40（`novatek-mp-criteria-5993`：X_Channel 0x3c / Y_Channel 0x28），`NVT_SHORT_AXIS_MAX=20320`；Y 镜像沿用 sheng 的 20319，**待真实手势判定后再定**。

## 固件

- 设备 ODM 镜像（`new-port/out/dna/odm/firmware/`）自带：`novatek_nt36532_o82_fw_csot.bin`（245,760 B，`a4f949fb…`，**默认**）、`novatek_nt36532_o82_fw_tm.bin`（`12ce2693…`，备用面板变体）；`…_mp_…` 为产测固件不用。
- 放到 `UniXNU-uke/.cache/firmware/novatek/`（不进 Git），`scripts/gen-touch-firmware.py` 生成 `build/native-touch/NvtFirmware.c` 嵌入内核（245,760 B 精确增加内核体积）。
- 协议主机测试可直接验证固件：`drivers/NativeNovatekTouch/Test/nvt-protocol.c <bin>` → `firmware: 245760 bytes, 16 partitions, cascade header 1, ILM 95447@0x0 DLM 82079@0x100000`。
- 下载/扫描失败时先换 TM 变体重编（分钟级）。

## 分阶段探针与 boot args

`sheng-touch=<n>`：1 只读快照（GCC/RCG/引脚）→ 2 投票+引脚+引擎 ID → 3 +RCG 编程+GENI init+芯片识别 → 4 +固件下载+帧循环。
配套参数：`sheng-touch-hid=1`（NativeHidInput 发布数字化报告，必需）、`sheng-touch-poll=1`（强制 10ms 轮询，诊断用；中断路径尚未实机验证）、`sheng-touch-hz`（≤19.2 MHz）、`sheng-touch-mode 0..3`（SPI 模式强制）。`sheng-touch-split=1` 可选独立数字化设备。

## 诊断输出（关键）

控制台行（阶段推进与失败定位）：
```
UKE TCH: thread start stage 4
UKE TCH: snapshot done st1 e0 vote… irq… rc…
UKE TCH: clocks+pins ok wr… fw…
UKE TCH: geni+rcg ok rcg… 19200000Hz d1
UKE TCH: identify ok id…
UKE TCH: firmware downloaded crc ok
UKE TCH: scanning v11 pid5993 60x40
UKE TCH: first valid frame type4
UKE TCH: hb f… ok… c… hid… x,y drv… rpt… r… edge… %u/%u irq…
UKE TCH: fail step <N> code <M>      # 任一步失败
```
心跳字段：`f` 帧数 / `ok` 有效帧 / `c` 接触数 / `hid` HID 报告数 / `x,y` 坐标 / `drv` 事件驱动挂载（1=好）/ `rpt,r` 送出报告数与结果 / `edge` 边缘钩子计数 / `irq` 中断数（轮询模式恒 0）。
`BootProgress` 版进度页在 GPU 轨 defconfig 下**未编译**（需 owner-root-audit 前置）；`BootProgress.c` 里的 `native_progress_touch_line()`（屏底固定一行直绘）已就绪，等该轨开启后可用（TouchProbe 侧为弱引用空桩）。

## 排查史（按时间）

1. **板级常量错配**（2026-10-24）：`TouchBoard.h` 是 sheng（SM8550）逐字拷贝 → 按 uke 硬件重写（wrap0/RCG/TLMM/引脚/GPLL0）。提交 `15838fa`。
2. **TLMM 基址 bug**（2026-10-25）：误用 DT 节点基址 0x0f000000，阶段 1 读未背线区域挂总线 → `IOSurfaceRoot::installMemoryRegions` 处无 panic 复位（CI 内核同现象排除本地内核）。修正为 0x0f100000（提交 `6f203a5`）。此前的复位**与触摸无关是假象**——v3 从未真正跑过触摸代码。
3. **固件缺失**：默认名 `n81a` 不存在；ODM 镜像里实为 o82 双变体。修正默认名（`8726186`），协议模型全过。
4. **无 UI 长跑稳定**（touch4e，无呈现）：34 分钟+ 触摸正常，心跳 `f119123 ok119123 c1 hid7004 … drv1 rpt7004 r0 edge1 99/6`。
5. **有 UI 后触摸线程死锁**（touch5）：画面出现 ~1 分钟后触摸死、系统活。根因：`NativeHidInput 588c96f` 在**触摸线程**直接 `handleReport`，真实消费端（SpringBoard HID user client）挂上后 workloop 上下文死锁。上游 `5f3a37c` 已改 `inputLoop->runAction()`（并带多指集合分发/按 ID 抬指/非手掌判定），核心仓 pin 已升至 `5f3a37c`（core `uke-gpu` `48b45aa`），touch5b `bfde5e22…` 判定中。

## 烧写与镜像

- 归档在 `uke-port/releases/`：`2026-10-24-touch-boardfix/`（touch1，CI 内核，只读阶段）、`2026-10-25-touch4/`（touch4，本地内核含固件，阶段 4 闭环）、以及后续迭代镜像（touch4c/4e/5/5b，见 `uke-port/data/vendor_boot_uke_touch*.img`）。
- 常用组合：有画面实测 = v3 参数 + `offline-legacy-base-present=1 offline-legacy-noconsole=1` + `sheng-touch=4 sheng-touch-hid=1`；纯控制台诊断 = 去掉 base-present/noconsole + `sheng-touch-poll=1`（心跳行可读，画面不呈现）。
- payload 构建：`./unixnu build payload -D ../UniXNU-uke --kernel <macho> --trust-cache … --data-ramdisk … --unique-chip-id 0x7252a8b07f8cb8f9 --kernel-arg …`，再 `scripts/patch-vendor-boot.py --base vendor_boot_uke_full.img`；刷前必须 `mcopy -i <img>@@4096` 抽内核核对标记与尺寸。

## 已知坑 / 注意

- **构建 worktree 的 NativeHidInput pin 会被并行会话重置**：本地重建前确认 `UniXNU-linux/drivers/NativeHidInput` 在 `5f3a37c`（`git -C drivers/NativeHidInput checkout 5f3a37c`），否则死锁回归。
- 本工具链对未定义弱引用不容忍：跨归档的弱符号一律用**弱定义桩**（`__attribute__((weak))` 空函数），不要依赖 `weak_import` 未定义。
- TCH 心跳/阶段行是 IOLog→msgbuf，`offline-legacy-noconsole` 只静音屏幕输出；配合 `c2f0582`（msgbuf 镜像进 userdata）可从主机侧读全量日志。
- `unixnu.toml` 1121 B 正常（已验证 v3 镜像 1148 B 工作正常，"~1102 B 丢参数"的坑不适用于这条 payload 路径）。
- Y 镜像常量 `TB_TOUCH_AXIS_MAX_Y=20319` 与可能的 X 镜像/XY 交换：等真实手势判定，一次改一个。
