# Safari 两次冻结与 GPU 性能回读（2026-11-13）

实测镜像 vendor_boot `b8ffe6b003cdc18dec39bef975edc03d2a62d89dac81ff462635042ac355c0e2`。用户报告先在 Safari 打开新标签页，卡住后短暂黑屏或转圈并返回锁屏或桌面。后来在复杂网页上划 Dock 打开 App 资源库，界面完全卡住。取证后设备处于原厂四分区、A 槽和 TWRP。主机只读 userdata，回收完整 Data 快照。

内核启动 UUID 为 `f3b76113-a96b-4b44-b1e7-add7f3165534`。连续区有效槽 5748–6003，覆盖 461.112–515.626 秒，无缺槽、来源丢失和 FUA 写入错误。原始连续区 SHA-256 为 `1ebf631aeb25f831e8972b3773857cab8951e471ea00c1a11d7a7214bf176c5d`。完整包为第 481 和 511 秒两份。文本尾环有 864 个对比上一启动有变化的有效槽，统一日志保留同 UUID 155090 条，并以 303 条内核文本锚对齐时间。两次故障属于同一次内核启动。

第一次在 305.028 秒。backboardd PID 62 被 PAC 异常终止，reason namespace 28、code 0x105。随后 backboardd PID 572 启动，SpringBoard PID 30 被 RunningBoard 以 backboardd respawn 为由终止，重建为 PID 577。现场 fault #1067 的插件 UUID `4daf08d0-3ebd-3eaa-8b59-dc0cbb2b6afb` 与实测包一致。栈经过 Rust panic 路径、`render::replay::render`、`RenderCommandEncoder::set_render_pipeline_state`、`nmb_render_set_pipeline` 和 Metal 编码线程。插件偏移 0xeaa3d4 调用 `panic_bounds_check`，共享失败分支的源位置记录为 `replay.rs:118:19` 的 `Handles::get`。同步回放调用点位于 `deferral.rs`。可定位到回放句柄越界及其后的 PAC 异常，尚未查明句柄和列表失配的形成路径。

第二次在约 7 分 45 秒停止更新。独立采样器最后完成 463.229789 秒，随后的一轮于 463.480811 秒进入进程遍历后停更。详细采样最后一条 CPU 记录为 463.338013 秒。463.280177 秒 SpringBoard 添加 AppLibrary，463.299562 秒 iconservicesagent PID 591 超过 15 MiB 后台硬上限。该日志确认了单进程限额事件，尚无证据表明它造成后续阻塞。GPU 最后提交 fence 17574 于 465.299621 秒，465.323289 秒完成，显示请求 11845 在 465.329632 秒锁存并于 465.329647 秒 retire。连续区可见的 194 笔提交均有完成记录。随后 GPU BR 有效采样仍到 515.626 秒，着色器忙计数增量为零。最后提交后仍有 406 条触控记录，最晚 501.269224 秒。Data flush 从 465 秒附近的 2130 多次增至 2252 次，错误仍为 0。证据将停点缩小到 UI 停止产生新帧，仍缺采样线程实际等待位置和锁持有者。旧记录器在进程遍历停住后无法再取得这一栈。

## 性能报告

按 [UniXNU GPU 帧诊断](https://unixnu.com/porting/gpu-diagnostics) 的脚本和触控窗口口径，合并每个包的两个前端段再统计。481 和 511 秒包在最终冻结前保存了相同的有效帧范围，不作为两次独立运行。

| 指标 | 旧包 210 秒 | 本次 511 秒 |
| --- | ---: | ---: |
| 持续触控翻帧间隔中位数 | 15.675 ms | 17.150 ms |
| 持续触控翻帧间隔 P95 | 50.212 ms | 46.409 ms |
| 持续触控翻帧间隔 P99 | 77.056 ms | 90.843 ms |
| 全屏 GPU pass P95 / 最大 | 5.970 / 7.675 ms | 6.700 / 12.571 ms |
| 全屏 pass 超过 8.33 ms | 0 / 3620 | 75 / 3668 |
| 大块锁页归一到每 64 MiB 中位数 | 27.8 ms | 40.3 ms |
| 最慢编码调用 | 78.668 ms | 334.374 ms |

耗时长尾见于全屏 pass、大块锁页和编码阶段。453.849 秒最慢编码调用的源码行为是 `drawIndexedPrimitives`，记录为墙钟用时。463.810 秒的一笔 80 MiB GPU 分配记录约 49.6 ms 限额锁等待和约 94.0 ms 锁页。第二次最后一条 Metal 命令从 commit 到完成约 35.48 ms，其中最后 GPU 作业执行约 23.61 ms。旧版和本次场景及采样范围不同，这些数据支持优化方向，无法量化单一代码改动的回归幅度。

`UKE GPU BO owner` 的 `bytes=` 旧标签实际是 KiB。新 backboardd 在 325、386、406、426、446、466 秒分别持有约 347、550、1021、956、1071、1141 MiB，峰值约 1294 MiB。分配量有增长也有回落。运行时长与冻结概率仍需同负载、多次运行才能量化。本次约 305 秒的其他进程限额日志仍报告约 2.35 GiB 可用页，缺最终停点的全局 VM 页数。

## 源码与产物

Metal 回放修正将 render pipeline 句柄查找改为有界访问。同步回放遇到无效句柄时走现有命令缓冲区失败路径，并留下错误原因。有效句柄仍按原先的顺序设置，普通延迟回放和首轮检查也有边界检查。此修正针对已观测的越界崩溃路径，失配源头待进一步定位。

连续落盘线程现在每约一秒检查两条采样工作线程的进度。超过一秒时，通过预先持有的线程引用和调度锁 try-lock，记录等待原因、锁状态和安全范围内的内核栈。该读取不依赖进程遍历或用户页，旨在缩小第二次冻结的现场缺口。

证据保存在 `uke-port/fixes/2026-11-13-flight-two-freezes/`。可从 `key-events.json`、`first-pac-fault.json`、`latency-report-*.json`、`unified-coverage.json` 和 `recovery-transition.json` 复核。报告以匹配 UUID 和原始连续记录为准。

候选 vendor_boot SHA-256 `5d9f3b30b7356769fb1da354c0c28c18e73b3d671c7a30d5dabe8d9383a93a32`。正式 kernel、Metal 插件、签名、FAT、配对 vendor_boot 和回读校验通过。启动参数 832 字节，实际图形参数、Turnip 和 Data seed 与实测包一致。此候选待刷写与实机验证。
