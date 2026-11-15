# 连续卡死记录版（2026-11-13）

用户要求尽量在下一次复现完成卡死日志采集，减少反复测试。本轮完成持续落盘候选 `b8ffe6b0…`：正常双环、直扫、固定120Hz及当前默认Metal/Turnip策略沿用 `f33ab396…`，额外启用 `uke-flight=1`。正式kernel、arm64e插件、payload、压缩回解及实包核验通过。**尚未刷入；最终卡死现场尚未采集，硬件记录覆盖未验收。**

## 持续记录与故障隔离

- 独立 `frame-flight` 内核线程目标每250ms运行一次，将新增GPU提交/完成/显示事件、异常与退出、IOKit阶段、线程等待栈、调频和进度写入既有UFS日志保留区。使用 `native_ufs_log_write` 的FUA写入，不经过VFS/APFS，也不等待30秒完整包。
- 既有32MiB文本尾环的前16MiB继续存文本，后16MiB改作256个64KiB flight槽，未扩大保留区或修改GPT。每槽包含boot session UUID、序号、uptime、开始时间、有效长度和覆盖头部/正文的CRC。写入失败保留当前缓冲重试；断写/覆盖中的槽在回读时拒绝，其余已完成槽可读。普通256MiB诊断导出区和双128MiB完整包不变。
- GPU和latency环新增独立有界游标，不消耗普通快照；覆盖与忙槽记录计入lost。每来源每轮最多8个32760B批次，防止一条高频来源饿死心跳。原始GPU/latency都使用原ABI解码。保留窗口取决于实际产量，不能承诺固定保留秒数；回读manifest报告真实首尾时间和缺槽。
- 独立 `frame-probe` 目标每250ms查backboardd、SpringBoard、Safari、WebKit、Dock和设置进程。只用task/scheduler try-lock复制保存的用户PC/LR、等待原因和安全范围内的等待线程内核栈；不读用户页、不查Mach端口，不发送IPI。每进程最多16条，主线程与GPU render/encode/completion预留槽。超额和锁忙有coverage记录。
- 旧详细采样继续保留。它在用户页读取阻塞时，独立探针和flight线程仍可工作；proc_iterate若阻塞，flight仍记录其开始/完成时间、阶段、PID/TID。每次心跳保存CPU状态、pending signal、GPU围栏、显示锁存和Data同步计数，以及写错/丢失计数。全局调度、存储或断电停止时仍无法保证最后瞬间落盘，目标是保留停止前最近的有效状态。
- Metal文件同步改为按经过时间检查，阈值一秒，空循环也检查待同步数据；阻塞的写线程仍可能延长实际间隔。零填充或非法帧头由解码器报告trace_gap，不再伪装成有效段头。

## 构建与验证

vendor_boot SHA-256 `b8ffe6b003cdc18dec39bef975edc03d2a62d89dac81ff462635042ac355c0e2`，配对boot `a5b9cf48b37bfe229b807582c82bbec60f5114c319106de0692bc49fc2a856fb`，FAT完整内核 `457ec3bef6358ebb524d6b52b5710cc55d73052e9559434577488e379b3a8177`，正式源内核 `83ba06e8a542e533f4ac55c800a895ec8588b87dd386d84f7557a777d963e203`。启动参数832B；只比前版增加 `uke-flight=1`，ADT除随机seed外及显示驱动参数相同。

组件：Adreno740 `5e1ef36`、AdrenoMetal `71e97c6`、GOP `eadd64a`；Turnip/metal2vulkan、Wi-Fi、背光和Data seed字节保持。插件CDHash `9543800d7e5934b34a61c7d3aa2b5bf8beb8f846`，匹配owner trust并逐4KiB页核验。prelink missing/stubbed集合与前版完全一致。新flight/probe/cursor/bootUUID符号在实际FAT内核确认。

主机检查覆盖：FUA失败后保留缓冲、256槽回绕、头部和正文CRC断写拒绝、忙槽/覆盖计数和有界游标、现有GPU并发环ASan/UBSan、短写/EINTR、两半区中断后上一完整包、空闲尾段按时间同步及失败重试、零帧尾缺口识别。证据见 `uke-port/fixes/2026-11-13-continuous-flight-recorder/` 和release `verification.json`。首次插件构建缺用户态SDK环境、首次payload命令工作目录错误的日志保留；已使用正确SDK/工具和路径重新成功构建，最终实包核验通过。

## 部署与一次复现的回收安排

新候选部署仍需当次明确授权。沿既有四启动分区、A槽流程，不刷写或擦除userdata；回退保留前版配对包。刷入后用户按原操作打开Safari并加载网页，无需切换参数或做一组对照。

冻结后避免重新启动iPadOS覆盖窗口，进入fastboot后沿已授权的原厂四分区/A/TWRP恢复流程取证。优先读16MiB连续区，再读完整诊断包和32MiB原始尾环；先不等待12GiB Data回读，只有需要报告/文件细节再补Data。

```sh
python3 UniXNU-uke/tools/collect-freeze.py --serial b8a81f12 \
  --out /absolute/new/capture \
  --baseline-log reference/logs/ukelog/ukelog-20261113-033455-frame-path-safari.bin
```

collector只读；保留flight-region.bin、boot分组manifest、GPU/latency/CPU/heartbeat、完整export/manifest及ukelog.bin。文本槽因跨启动残留仍需按baseline/boot匹配；工具不自动把所有有效文本当本次。flight每槽UUID可直接区分不同启动。也可用 `read-flight.py --image <16MiB.bin> --out <new-directory>` 离线解析。

验收边界：下一次冻结回收须先确认本次boot UUID、最后成功FUA槽、心跳/探针更新时间、每来源lost及覆盖窗口，再用GPU最后提交/完成与等待栈判定停止环节。当前仅证明主机代码、格式和镜像门禁通过，不能宣称已取得卡死现场或修复卡死。

## 本轮刷写

2026-11-13T04:50:52.498996+08:00，四分区Sending/Writing均OKAY，set_active a/current-slot a/reboot均成功；未刷写或擦除userdata。回执 `uke-port/releases/2026-11-13-continuous-flight-recorder/flash-result-20261113-045045.json`。刷后不轮询，等待一次原Safari操作复现。
