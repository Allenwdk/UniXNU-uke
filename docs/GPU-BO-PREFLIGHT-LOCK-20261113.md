# GPU 大块分配预处理锁优化（2026-11-13）

候选 vendor_boot SHA-256 `6ecf3db32bfb53d827089ccf534368ce8d0afeb8eb1c35256035cccff6565e62`。基于 `5d9f3b30…`，保留其 Metal 回放句柄边界检查和采样停滞时的内核栈记录。新包已通过正式 kernel、payload、镜像和签名核验，待当次授权刷写和实机测量。

在实测 `b8ffe6b0…` 的同次日志中，backboardd PID 572 的编码批次 53729 历时 335.721 ms，其中编码线程占用 CPU 1.996 ms。最慢的索引绘制占 334.374 ms 墙钟时间。它开始时附近有一笔同 PID 的 80 MiB 分配，锁页 70.487 ms，前置限额检查仅 1 μs。这笔分配与长尾时间重叠，尚无足够记录解释剩余等待。

另一笔 80 MiB 分配在 463.810297 秒进入，前置限额检查 49.612 ms，锁页 93.955 ms，提交分配前等锁 8 μs。在 387.293964 秒也有一次前置检查 44.336 ms。保留的 37 笔 PID 572 大于等于 32 MiB 的原生预处理分配，前置检查中位数 1 μs，锁页中位数 50.327 ms。优化针对偶发的前置长尾，收益取决于同负载下的竞争。

Adreno740 `03ec4bc` 在连接建立时发布固定的 BO 大小上限和物理地址位宽。原生分配预处理用 acquire 读连接的 live 状态，再读这两项，直接开始准备页面。实际接收页面仍走原锁内的连接与分配校验，close 抢先时由原清理路径归还页面。构建对象反汇编确认预处理读取连接字段后调用 prepareSystem，未执行原先那次驱动锁获取。

全屏触控窗口中的 75 个超 8.33 ms pass 都走 GMEM，其中 73 个为 3200×2136、三颜色附件。后续应先对齐这些 pass 的 BR 计数、附件 load/store 与绘制特征。实际锁页、334 ms 调用剩余等待和 App 资源库最终冻结仍有待解决。对照建议沿用原场景，记录 `limit_us`、`wire_us`、`lock_us` 和持续触控翻帧分位数。[官方 GPU 帧诊断](https://unixnu.com/porting/gpu-diagnostics)

源码内核 SHA-256 `02349d54c3da3bbcf5c35afbce3f13e0c0b2a34a4e833e1def8274253c20ea98`。FAT 内核 SHA-256 `322aaec60fca56f6791c4c972b2f664685f614c51e5d8ef291441bf06717cc19`。实际启动参数 832 字节。已校验配对 boot、原厂 vendor_boot 元数据、prelink 闭包及与上一候选完全一致的 Metal/Turnip addon、信任缓存与 Data seed。

证据目录 `uke-port/fixes/2026-11-13-bo-preflight-lock/`，包含同次时间关联、全屏 pass 分类、预处理反汇编和构建核验。候选目录 `uke-port/releases/2026-11-13-bo-preflight-lock/`。
