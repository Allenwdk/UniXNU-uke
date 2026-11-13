# 台前调度卡死与 CPU 定时唤醒停滞（2026-11-13）

实测 `4b898e0b…` 在台前调度中再次卡死。启动 UUID `59f53559-2908-41bb-9a88-3ccc2ca8a1dd` 由连续区、文本 VM 行和 Data timesync 三方确认。回收 533 个相较上一版有变化的文本槽、256 个连续区有效槽、60/90 秒两份完整导出，以及完整 Data 12,310,069,248 字节。设备留在原厂四启动分区、A 槽、TWRP。没有主机 userdata 写入。

- `frame-export` 在 118.209 秒进入睡眠，`frame-flight` 在 118.291 秒进入睡眠。两者到 366.454 秒仍处于 `TH_WAIT | TH_UNINT`，等待事件为 `clock_delay_until`，已符号化的栈均经过 `delay_for_interval → thread_block`。取到的栈指向超期睡眠，尚缺每个等待实际所属的 CPU 定时器队列。
- 物理 CPU4（affinity 0x400）的定时器与 IPI 在 105 秒采样还在增加，从 125.100 秒采样至 366.454 秒固定为 55625/165964，另外七核仍推进。两条线程在这个窗口同时停更，支持定时唤醒故障与单核中断停转有关。尚无该核最终 PC 或 IRQ 屏蔽状态。
- 同 UUID 统一日志共 214589 条，290 条内核锚对齐。353–354.669 秒 SpringBoard 有台前调度相关的 `ContinuousExposeIdentifiersChanged`。Data 上 Metal 帧记录最后完成 368.540 秒，最后提交 368.560 秒。368.457→370.461 秒，GPU 围栏 14634→14762、显示锁存 8626→8672、Data flush 1503→1507。末段无新 GPU 挂起或内核 panic 记录，最终界面卡死的准确时间及锁持有者未知。
- 366.454 秒空闲页 1661（约 6.5 MiB），另有 305063 投机页和 465961 非活跃页。空闲页很低，不能单靠这一项判断全局耗尽或此次死锁原因。
- 对照旧尾环，`6ecf3db3…` 通知中心运行末段 CPU6 的定时器和 IPI 固定，至少从 2193.506 秒持续到 2874.861 秒。更早 `b8ffe6b0…` 的 CPU3 和 CPU6 在末段也有同类现象。故障可能跨核，不能据此认定 CPU4 本身损坏。

## 八核修复候选

七核包 `715e8fed7ea26914153bc4a325fa107b713f1d098a9dfb413f818cbb020051e4` 从未刷入，已标记 `DO_NOT_FLASH`。设备树恢复 affinity 0x400。保留按实际拓扑映射的 IPI 自检和 EPSS 统计。

CPU4 的 WFI 返回次数在 84.557 至 105.225 秒保持 74458，同期定时器和 IPI 继续增加。118.283 秒的最后一份连续 CPU 记录仍把 CPU4 标为运行。两条记录线程超期睡眠，且 CPU4 的收发计数随后一起停更。现有日志缺停转瞬间的 PC，无法判定 CPU4 进入了哪条路径。

XNU 的临近定时器分支在 IRQ 屏蔽下进入 WFE。原逻辑只在 ISR 显示中断挂起时退出，等待期限为 `~0ULL`。虚拟定时器到期而 vGIC 尚未呈现中断时，核心会继续等，即使架构定时器事件流周期性唤醒 WFE。修复以原定时器到期点加 50 μs 为限。到期仍无挂起中断时，经现有 XNU 路径重设最小定时器间隔并返回，让正常 IRQ 服务队列。WFE、WFI、SPI IPI、定时器队列和 GPU 渲染路径均保持启用。[Arm Generic Timer](https://documentation-service.arm.com/static/651fbd69bc48b0381ce0e06c)

独立文本日志保存八核等待进入、返回与重设次数，以及调度状态和 pending signal。若再次冻结，应把这些计数与定时器/IPI 进度、记录线程栈及最后显示锁存对齐。该改动处理源码中无限等待的问题，其与本次 UI 冻结的因果关系仍待实机验证。

证据见 `uke-port/fixes/2026-11-13-stage-manager-trace/` 的 recovery-transition.json、capture、key-events.json、unified-coverage.json、data/capture.json 和构建记录。

设备实现提交 `b0bcdc3ae3b18c0683bdfd974d1e2f5ef2ef28cf`。新配对镜像在 `uke-port/releases/2026-11-13-idle-timer-rearm/`，vendor_boot SHA-256 `6e29d32c09c86430048ca60e005c67082ca9606508adb568cc75b15377cec598`。正式内核编译通过，远端 IPSW 索引请求停留后终止该进程，改用本地 24A446 IPSW 完成同一产物预链接。payload 与压缩回读、FAT 内容、签名与信任、原厂 DTB 和八核 ADT 核对通过，参数 832 字节。原生与打包内核均含等待、返回、重设三个计数。实机尚未部署。

50 μs 是本轮选择的到期宽限值，按到期时间计时，仅在等不到中断时触发恢复。事件流是否正常、vGIC 递送是否为实际触发条件，以及该路径与最终界面冻结的关系，都需要实机数据确认。验收须覆盖普通空闲唤醒、台前调度、八核定时器/IPI 持续推进和超过旧故障窗口的记录导出。主机构建与镜像校验记录见 `uke-port/fixes/2026-11-13-idle-timer-rearm/`。

最终实包反汇编确认 50000 ns 宽限、有限期限 WFE、ISR 重查与 ClearIdlePop 调用，证据为 fixes 目录的 packed-cpu-idle-disassembly.txt 和 code-verification.json。归档 21 项 SHA-256 校验与 porting-record 13 区检查通过。该记录为主机验证，实机仍待验。
