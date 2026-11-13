# 关闭 GPU 诊断日志并保留 AppSync（2026-11-13）

用户要求关闭 GPU 相关日志后构建，并保留另一个会话新增的 AppSync。本轮以已通过桌面反馈的 AppSync 重导出修正版 047a13e3 为运行库基线，core bd739324，设备日志实现 2ccbf5a。

## 变更

本地构建参数移除 sheng-diag=1、sheng-gpu-trace=1、gpu-perf=1、sheng-diag-export=60、uke-diag-repeat=30 和 uke-flight=1，保留 uke-progress=1。因此 Metal 前端帧文件、编码线程耗时采样、运行时阶段环、Turnip pass/submit 环、内核 GPU trace 与性能计数器采样、连续 FUA 记录器和周期诊断包导出均停止启用。

设备文本记录线程原来无条件输出 GPU 调频、任务、围栏、内存池、映射与恢复统计。本轮将这些周期输出归入已有 diag 开关。基础 Data 进度行使用独立格式，CPU 定时器记录直接读取每核 timer/IPI 计数，无需打开图形诊断。保留 CPU/VM/存储/电池基础记录及故障、panic 信息。

AppSync 运行库、四个系统服务覆盖、进程内签名器、Foundation 重导出修正和信任 bundle 由实测版完整复用。板级 appsync=1 和两份 XNU AppSync 补丁继续参与正式构建。八核 ADT、定时器等待修复、120 Hz 与双环渲染配置保留。

## 验证边界

GPU 为冻结原因的判断尚未结案。此次关闭日志由用户明确要求，性能变化、持续运行与 AppSync 安装/启动行为仍须实机验证。镜像不在本轮自动部署。

证据目录为 uke-port/fixes/2026-11-13-gpu-logs-off-appsync，配对镜像归档为 uke-port/releases/2026-11-13-gpu-logs-off-appsync。build-inputs.json 保存参数和运行库哈希，source.patch 保存日志门控实现。

## 构建与核验

配对镜像 vendor_boot SHA-256 `a38e9e386b7be40acfd7ca3cb78e533be53d2e4f824e124d287322a7992b241f`，boot SHA-256 `a5b9cf48b37bfe229b807582c82bbec60f5114c319106de0692bc49fc2a856fb`。正式 kernel、预链接、payload 及压缩回读通过。FAT 中实际启动参数为 747 字节，已核对 AppSync 开关、八核 ADT、Data seed、原厂 DTB/bootconfig/vbmeta 和配对固件。

完整 addon 的 69 个文件与 AppSync 047a13e3 逐项相同，owner trust bundle 逐字节相同。AppSync Foundation LC_REEXPORT_DYLIB、4 KiB 代码签名页和信任条目通过。完整内核保留 AppSync 用户应用信任入口及三项 CPU 定时等待计数。

logging-verification.json 确认日志源码与提交 `2ccbf5a362edf06e458d888f08f49b71fc35cb73`、正式编入的 source-storage archive 对齐。所有周期 GPU/DCVS 文本行均在 diag 条件中，CPU timer/IPI 记录独立工作。常规故障与 panic 输出继续记录。

候选尚未刷入。设备现状沿用另一会话的 AppSync 047a13e3 实刷与桌面反馈。GPU 诊断关闭后的行为尚无实机验收。
