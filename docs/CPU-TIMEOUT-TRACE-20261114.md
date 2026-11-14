# 桌面冻结与 CPU3 超时处理路径（2026-11-14）

复刷 `13fbe7af…` 后，用户确认已进桌面，随后完全冻结，短按电源键无反应。恢复原厂四个启动分区，A 槽回读通过。TWRP 经主机 ADB 重连后可读取。userdata 首 8 KiB 回读 SHA-256 仍为 `387c5c0d23d53365d4731835812a3b962738a963f90024cb706570934a97d93d`，与根清单恢复件一致。取证为只读，设备停在 TWRP。

同次 UUID `5063AAF1-ACA0-4DD9-B9E3-A6D7632DBB13`，763 字节参数和每核记录符合实刷包。32 MiB 日志环有 365 个 CRC 正确变化槽，序列 0–364，无缺口。pstore 为空，文本最后一条在 226.954 秒。Data 回读 12,310,069,248 字节，UUID 与 timesync 一致。统一日志保留同次相关记录 61,137 条，以 44 个锚点对齐至 206.940 秒。完整系统日志在最终冻结前可能尚未落盘。17 份同次 IPS 均为用户进程报告。宿主保留最新三份 Data 快照，按既有规则卸载后清理较早的通知中心快照，旧取证文本保留。

| 观测 | 同次结果 |
|---|---|
| 最后 IRQ | CPU3 约 110.580 秒。当时被中断的指令位于 `pmap_remove_pv+0x1c0`，保存 PSTATE 中 IRQ 屏蔽位为 0。 |
| 从 124.745 到 224.948 秒 | CPU3 IRQ 入口和返回均固定为 186822，phase 0。timer 46280、IPI 141684、待处理 SIGPast=8、临近定时等待 12/12、补发 0。其余七核与存储继续活动。 |
| 最后关中断调用 | 同一线程的返回地址为 `ml_io_check_for_mmio_overrides+0x38`，调用指令位于 +0x34。记录 `maskOpen=0`，说明经过接口记录的这段关中断已结束。 |
| 同次 GPU 恢复 | 102.533 秒恢复返回 0，重放 11 项。与 CPU3 停转及桌面冻结的因果待确认。 |

在该 24A446 XNU 中，`kern_timeout_try_panic` 的 fatal 分支调用 `ml_io_check_for_mmio_overrides` 查询允许的 MMIO 超时。无适用覆盖时可继续到 `panic_plain`。Uke 的 panic renderer 接管公共入口，可让触发核在 WFE 停驻。上述 CPU3 状态与这条路径相符。现有日志没有实际 timeout 类型、阈值或实时停驻 PC，`pmap_remove_pv` 也只是最后一次 IRQ 打断的位置，尚不能据此认定页表代码自身卡死。[XNU 补丁参考](https://unixnu.com/xnu/patch-reference)

设备实现 `c97bca260a1b831fce889889204dcacc4878c69d` 将抢占关闭时长和中断关闭时长的开发诊断默认值改为 XNU 的 TRACE 模式 1，保留计时和超时事件，避免这两项检测直接走 fatal 路径。此选择针对当前证据，区别于 sheng `1108068` 关闭计时诊断静态键的默认设置。八核、Edge、每核 IRQ 记录、AppSync 与 GPU 诊断关闭配置继续有效。一般 panic、页表校验及断言仍可触发。记录模式的运行时效果以及是否解决桌面冻结待实机验证。

候选 vendor_boot SHA-256 `3849c862faf8394ad3901484218244526bc03e82c6c9f14ccf9228c373ed4fb8`，763 字节命令行沿用上一版。正式内核、预链接、payload、镜像配对、最终 FAT 的八核 ADT、内置 TRACE 参数、AppSync 全部运行库、完整信任 bundle 和根策略通过核验。尚未刷。主机证据在 `uke-port/fixes/2026-11-14-cpu-stall-readback/`，配对镜像在 `uke-port/releases/2026-11-14-cpu-timeout-trace/`。实机首先确认启动，再核对开机约 110 秒后所有核的 IRQ 和定时器继续推进。
