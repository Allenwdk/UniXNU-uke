# 控制中心冻结与 CPU4 持续停转（2026-11-13）

用户在 GPU 诊断关闭、完整保留 AppSync 的 `a38e9e386b7be40acfd7ca3cb78e533be53d2e4f824e124d287322a7992b241f` 上仍遇到偶发冻结。本次为下拉控制中心，开机时长不详，随后持续无反应，用户未报告自行恢复或自动重启。用户认为与此前按电源键还能熄屏的表现不同，怀疑 CPU 或内存。操作描述保存在 user-observation.json。

沿既有取证流程从 fastboot 恢复原厂 boot/vendor_boot 四分区，切 A 槽并进入 TWRP。主机未写擦 userdata，分析期间设备留在 TWRP。

## 同次证据

- 完整读取 32 MiB 文本环，SHA-256 `01131770b36e3f6aad04d6c947e1e1f38b6619d76adb0a608a68c150c9370854`。对照上一份 AppSync panic 原始环，554 个 CRC 有效变化槽为 sequence 0–553，连续无缺口。全部槽位与本版 2048 槽几何匹配。747 B 参数、带 irqT/irqI 的新 CPU 行与唯一启动 UUID `420A3E5B-1468-4CD8-BB78-A950AE60514B` 确认是本版同次启动。
- 同 UUID 由 Data timesync 再确认。Data 精确回读 12,310,069,248 字节，122591 个差页写入主机 reflink，fsync 后 capture_complete 为 true。统一日志解析 2365467 条原始记录，保留同次相关记录 64607 条，以 53 条匹配内核锚对齐。有效对齐覆盖至 354.388 秒，文本末尾为 384.538414 秒。
- 统一日志 parser 返回 0，同时报告未知 chunk 和尾部不足的告警。保留原始日志、告警与覆盖清单，不能保证最后 UI 事件已落盘。pstore 目录为空，文本中没有新 panic。
- 本版关闭 GPU flight 和完整周期导出，旧区域未用于本次判读。回收工具直接解析完整 32 MiB 文本窗口。旧连续版 collector 固定仅解析下半区，未用于本次文本范围判定。

## CPU 与内存

| 项目 | 同次记录 |
|---|---|
| 最后一个仍正常增加的 CPU4 样本 | 83.644736 秒，timer 28618，IPI 95106 |
| 第一个停更样本 | 103.794964 秒，timer 40224，IPI 126436 |
| 最后的 CPU4 样本 | 384.334984 秒，仍为 40224 / 126436 |
| 停更期间状态 | state 6、priority 46、idle 0、pending signal 0x8 保持 |
| 定时等待计数 | waits 13、returns 13、rearms 0，始终保持 |
| 首次发现停更时的空闲内存 | 802913 个 4 KiB 页，约 3.063 GiB |
| 其他七核 | 定时器与 IPI 计数持续推进 |
| 最后 Data 进度 | 384.334956 秒，read 87338、write 56244、flush 2022、timer flush 59、errors 0 |

CPU4 的中断计数至少 280.540 秒保持固定。实际停转点位于 83.645–103.795 秒采样之间。原生 XNU 定义 0x8 为 SIGPast，表示请求 AST 调度检查。pending bit 本身只能证明请求尚未处理，尚缺 PC、IRQ 屏蔽状态和正在执行的回调，无法据此判定卡在 ast_check。

此次 waits 与 returns 相等，且 rearms 一直为 0。现有证据不支持上一轮“临近定时器 WFE 等待未返回”的推测，原修正没有解决本次停转。当前应沿 CPU 运行、关中断区间和 IPI/定时器回调继续定位。

首次停更时仍有 3.06 GiB 空闲页，单纯内存耗尽无法解释这个早期异常。后期空闲页曾降低，但仍有大量 speculative/inactive 页，不能仅凭 free 值判断耗尽。内存损坏、锁或调度等待等原因仍待现场证据。

## GPU 与界面边界

98.786427 秒仍记录一次常规 GPU 恢复，status 0，重放 10 项，FSR/FAR 为 0。它位于 CPU4 的停转采样窗口内，缺 CPU4 精确停转时刻，先后与因果尚未确定。最终控制中心冻结无法据此归因为 GPU，也不能用关闭 GPU 诊断作为排除 GPU 的依据。

存储、触控和其他核在 CPU4 停更后仍推进。统一日志中 Preferences 在约 340.259 秒有 ControlCenter RemoteService 状态请求，该请求不能替代用户最后下拉手势的确切时刻。末段缺冻结时 UI 主线程等待链。

同 UUID 4 份 IPS 分别为 WorldClockWidget、CollectionsPoster、DockFolderViewService 和 spotlightknowledged.updater。未发现可直接说明最终冻结的同次 kernel panic 或 backboardd 崩溃报告。已有 DockFolderViewService 的地址 0x10 异常与壁纸创建超时保留为独立事件。

## 留存与后续

原始文本、同启动筛选、CPU/内存时序、系统日志及 IPS 归档于 `uke-port/fixes/2026-11-13-gpu-logs-off-freeze/`。关键文件为 capture/collection.json、findings.json、key-events.json、unified-coverage.json、unified-parser-coverage.json、ips-summary.json 和 recovery-transition.json。

新 Data 捕获完成后按既有规则仅保留最新三份完整主机快照，为 notification-freeze、stage-manager-trace 和本次 gpu-logs-off-freeze。已删除两份较早且未挂载、无打开进程的 frame-path-safari-freeze 与 flight-two-freezes 镜像，其他原始日志和分析保留。逐文件清单为 snapshot-retention.json。

下一步需要 CPU4 最后执行位置、每核 IRQ 进入/返回阶段及关中断区间，再关联 UI 等待链。当前完成本轮取证和分析，根因尚未确认。
