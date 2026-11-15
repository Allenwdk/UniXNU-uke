# Uke CPU 停转与 sheng 平台对照（2026-11-14）

控制中心冻结的同次记录显示 CPU4 从 103.795 秒至 384.335 秒保持 timer 40224、IPI 126436 和待处理 SIGPast 0x8。其余七核与 Data 同期继续推进。临近定时器等待 13 次均已返回，补发 0 次。[原始取证](GPU-LOGS-OFF-FREEZE-20261113.md)

| 核对项 | Uke 实际构建与 sheng 当前源码 |
|---|---|
| CPU 与 GIC | Uke 编译后的 DTS 与 OEM 的 GICD 0x17100000、GICR 0x17180000 和步长 0x40000 一致。OEM 19.2 MHz 定时器描述含 PPI 27，ArmCpu 接入虚拟定时器 PPI 27。PSCI 入口采用 SMC。实包含八核 affinity 0x000 至 0x700。三核 E、五核 P 的分组与 OEM CPU 容量分档及 sheng E/P 分类吻合。[平台文档](https://unixnu.com/zh/porting/platform) |
| 组件 | Uke ArmCpu 8721288 包含上游 main d90e457。常规 SPI IPI 发送、定时器和接收路径一致。额外差异为 Wi-Fi MSI 的 64 个 SPI 槽、SGI 诊断门控和诊断自检按拓扑寻址。ArmDTPlatform Uke 638c39a 与官方 e8bd8d5 源码树一致。 |
| XNU 补丁 | sheng main 1108068 的 AMP、Edge 矩阵、CPU idle 统计和 SIGP 恢复断言补丁与 Uke 逐字节相同。iPadOS 27 bring-up 补丁只在日志和异常记录参数命名上有差异，CPU 部分相同。 |
| XNU 配置 | sheng 公开的 iPadOS 27 配置启用 SMP、SPI IPI 和自 IPI，Edge 矩阵开关位于 macOS 专用内置参数中。Uke iPadOS 27 内置 `armcpu-edge=1`，同次日志为 `sched init completed=1`。sheng 还默认关掉调度时长诊断及其静态键。本轮比较范围为公开配置和既有 Uke 实机证据，缺少同负载 sheng 实机日志。 |

SIGPast 由调度请求设置，处理器进入对应分支时会先清除它，再调用 ast_check。CPU4 留下的位表示有检查待处理，最后执行位置仍未知。Edge 配置和 GPU 恢复与停转的因果待现场确认。

新候选增加 `uke-cpu-stall=1`。每核在固定容量内存独立记录，现有持久文本每轮读取。

- `UKE CPU IRQ` 包含进入、返回、最近 IRQ 时间和阶段。0 为 IRQ 已走完 C 层退出记录，1 为入口，2 为启动进度回调，3 为平台 IRQ 处理，4 为熵采集或 IRQ 尾部。20 为 IPI 总处理，21–26 依次为 cross-call、维护、AST、本地定时队列、调试器请求和 kperf。30 为时钟入口，31 为定时队列。
- `UKE CPU site` 为最近一次 IRQ 入口保存的 PC、PSR、SP、FP 和线程标识。PC 指向被中断的代码。停转时的实时 PC 仍需其他现场证据。
- `UKE CPU mask` 记录通过 `ml_set_interrupts_enabled_with_debug` 关闭中断的调用点和线程，调用点按返回地址减四符号化。纯汇编修改 DAIF 绕过该接口。读者不持有目标核的锁，记录冲突时本轮跳过该核。热路径增加内存访问，实际开销待验。

正式 kernel、预链接、payload 和配对镜像通过。FAT 命令行实测 763 字节，新增 `uke-cpu-stall=1`。保留八核、既有调度策略、关闭 GPU 诊断和 AppSync 全部运行库及信任 bundle。最终反汇编核对 6 条路径，包含编译器内联至 IPI 函数的 4 个回调阶段写入。

设备源码 `02757b828aeec889db09ad8f99daa5465c29ccd5`，vendor_boot SHA-256 `13fbe7af6f4ede0a82464d0acbaa400e6fe86eec134d3f4e0cf927b33d34d0d7`。后续实测优先把停转核的 IRQ 阶段、关中断调用点与同次 UI 时间线对齐。构建及归档位于 `uke-port/releases/2026-11-13-cpu-stall-sheng-audit/` 和 `uke-port/fixes/2026-11-13-cpu-stall-sheng-audit/`。

## 本轮刷入

2026-11-14T22:17:04.210770+08:00 用户授权刷入 13fbe7af。四启动分区的发送和写入均成功，A 槽回读确认，重启命令成功。实机启动和冻结复测待反馈。[刷写回执](../../uke-port/releases/2026-11-13-cpu-stall-sheng-audit/flash-result-20261114-221656.json)。

实刷后启动在 UFS 根卷清单校验处 panic，已恢复物理分区首 8 KiB，iPadOS 启动待验。[回读与修复](CPU-STALL-ROOT-PANIC-20261114.md)。

## 根卷清单恢复后的复刷

2026-11-14T22:35:36.079952+08:00 从分区和整盘直接复读，确认此前修复的 SHNGROOT 仍然正确。用户授权复刷同一 13fbe7af 镜像。四个启动分区均写入成功，A 槽回读确认，重启命令成功。实机启动和冻结复测待反馈。[刷写回执](../../uke-port/releases/2026-11-13-cpu-stall-sheng-audit/flash-result-20261114-223528.json)。
