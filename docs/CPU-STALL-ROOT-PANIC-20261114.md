# UFS 根卷清单损坏与启动 panic（2026-11-14）

实刷 `13fbe7af…` 后，照片显示 `SourceStorage: UFS start failed (root step 3: root manifest rejected)`。按实刷内核符号表回查，第一帧为 `native_source_storage_start+0xc48`，随后为 bsd_init、kernel_bootstrap_thread。32 MiB 持久文本环与上次取证逐字节相同，2048 槽均无变化，pstore 为空。本次 panic 现场来自照片，TWRP 日志另用于分析磁盘内容。

GPT 头和分区表 CRC 均通过。从 `/dev/block/sda32` 与原始 LUN 0 的绝对 LBA `0x2d3000` 读取的首块一致。实读前八字节 `8b1ebc1783fb6df8`，原始首块应以 `SHNGROOT` 开头。生产 RootLayout 解析器回放设备首块返回状态 1，回放原始清单时取得两个根窗口和两个校验表对象。[UFS 移植文档](https://unixnu.com/zh/porting/ufs)

TWRP 解密映射偏移 0x400 和 0x1400 都有 F2FS superblock magic 0xf2f52010，分别落在两个变更块中。此轮 recovery 日志还记录自动 fsck.f2fs 因 checkpoint 无效退出。已确认被覆盖的内容类型，具体写入者和时间尚待确认。

完整回读 15,753,805,824 字节根卷前缀，OS、SystemOS 和两份 vroot 表的 SHA-256 全部匹配。两张表的摘要与实刷镜像内签名头匹配。仅起始两个 4 KiB 块不同。主机模拟恢复这 8 KiB 后，整个前缀哈希与原镜像一致。持久 Data 从 userdata 偏移 25,788,391,424 字节开始，头部与上次回读一致。

已备份设备原始 8 KiB 并生成 `restore-first-two-blocks.bin`，SHA-256 `387c5c0d23d53365d4731835812a3b962738a963f90024cb706570934a97d93d`。用户随后授权写回 userdata 字节 0–8191，执行结果见下方回执。既有签名和根卷校验用于后续启动验证。

取证完成后设备留在原厂四启动分区、A 槽、TWRP。该取证阶段主机未写入 userdata。旧候选已标记 DO_NOT_FLASH；根卷修复后仍待实机检查 CPU 取证版启动及原冻结问题。镜像中的根卷签名策略与前一实刷版逐字节相同。

[完整根卷回读](../../uke-port/fixes/2026-11-14-cpu-stall-root-panic/root-readback.json) · [恢复范围与备份哈希](../../uke-port/fixes/2026-11-14-cpu-stall-root-panic/repair-plan.json)

## 已写回并复读（2026-11-14）

2026-11-14T22:32:00.306996+08:00 完成用户授权的 userdata 字节 0–8191 写回。dd 报告两个 4096 字节块，fsync 成功。首次从整盘节点做普通缓冲读取时返回旧数据，随后分别通过 O_DIRECT 从分区和原始 LUN 复读，两个 8 KiB 的 SHA-256 均为 `387c5c0d23d53365d4731835812a3b962738a963f90024cb706570934a97d93d`。设备复读的 SHNGROOT 通过生产解析器，取得两个根窗口和两个校验表对象。首个 MiB 中修复范围外的内容、持久 Data 头部及 32 MiB 文本尾环均与写入前一致。

平板保持原厂四启动分区、A 槽、TWRP。iPadOS 根卷挂载以及 CPU 取证版的启动和复现仍待实机确认。[写回与校验回执](../../uke-port/fixes/2026-11-14-cpu-stall-root-panic/repair-result.json)。

## 根卷清单恢复后的复刷

2026-11-14T22:35:36.079952+08:00 从分区和整盘直接复读，确认此前修复的 SHNGROOT 仍然正确。用户授权复刷同一 13fbe7af 镜像。四个启动分区均写入成功，A 槽回读确认，重启命令成功。实机启动和冻结复测待反馈。[刷写回执](../../uke-port/releases/2026-11-13-cpu-stall-sheng-audit/flash-result-20261114-223528.json)。
