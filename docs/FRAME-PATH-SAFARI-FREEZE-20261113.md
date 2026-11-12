# 正常日志版 Safari 网页加载后冻结（2026-11-13）

用户确认 `f33ab396…` 已部署，打开 Safari、网页加载出来后整机完全冻结，怀疑 GPU 高负载有关。本轮按既有授权恢复原厂四个启动分区、A 槽、TWRP 并只读取证。没有新开发镜像部署，也没有主机 userdata 写入。当前证据**尚不能确认 GPU 满载是最终冻结原因**。

## 身份与回收完整性

- 同次 boot UUID `92E994D1-852D-4B2E-8776-012A30F0DDD7`。尾环有 332 个新 CRC 有效槽，原始 SHA-256 `3c88345295a8d3de1b5572db3a42850d2d08d0a852f351236cf68269f7b7fb64`。导出的实际 bootargs 包含正常双环、diag1/gpu-trace1/gpu-perf1、60 秒首包及 30 秒重复设置。
- Data 只读回收 12,310,069,248 B，3,005,388 个已存在页逐页比较；68,897 页变化，2,936,491 页相同。完整完成后使用只读 APFS 挂载。挂载器提示 snapshot meta tree 初始化失败；当前根目录可读取，诊断结果仍以逐文件/容器验证范围为限。
- 两个原始周期诊断包均可完整解析：180 秒半区 0、210 秒半区 32768，分别为 65,998,485 B 和 76,171,276 B。保留 export.bin 与逐文件 SHA manifest。完整包不是冻结后的现场快照。
- 统一日志按 UUID 筛选 65,402 条。系统校时造成两个时间段，使用 181 条唯一内核文本锚分别校准；有效正 uptime 记录止于 207.596 秒，内核统一日志止于 205.379 秒。不能用校时前后单一墙钟偏移判断冻结先后。

## 时间线与能确认的行为

| 内核 uptime | 证据 |
|---|---|
| 122.609–122.616 s | GPU 切环超时，`NativeAdreno hang`；恢复 status=0，重放 6 项，FSR=0。之后同一 backboardd 继续出帧。这是早于最终 Safari 加载的已恢复故障。 |
| 208.388 / 208.865 s | DockFolderViewService PID449/451 分别 SIGSEGV，FAR=0x10，PC相同。原 cache UUID及slide核对后的栈指向 DesktopServicesPriv `TNode::GetNodeFromPath` → DocumentManagerCore 启动。不是 Metal/Turnip 崩溃栈，与最终冻结的因果未证明。 |
| 210.410 / 210.436 s | 最新完整包的 GPU / latency 环末端；显示 request3880 已 retire。它不包含 211 秒后新 WebKit 进程的冻结栈。 |
| 约210 s | Safari 保存一份 lightweight `ExcUserFault`，WEBKIT code0、flags518。原24A446 cache UUID匹配；顶部 `_os_fault_with_payload+8`，下层为 ObjC `+initialize`、Safari 导航策略及 WebKit IPC。它是用户故障报告，不能据此宣称 Safari 被杀或 GPU 驱动崩溃。报告墙钟经历校时，以约数记录。 |
| 211.583 / 211.683 / 213.179 s | WebKit.GPU454、Networking455、WebContent466 原启动检查通过。前两者另有 Mach-port EXC_GUARD AST，缺其最终退出状态，不能当成退出证据。随后有外网443连接。 |
| 215.951 s | 最后一条有效 backboardd command completion。此前210–215秒191个完成命令，commit→completion中位13.096ms、最大52.262ms，体现长帧；命令数不是屏幕FPS。 |
| 217.401→219.406 s | GPU submit7224→7326、fence7217→7318；显示latch4175→4277、retire4175→4277；Data flush1056→1080、errors=0。最后持久记录前仍在渲染、锁存及同步。submit/fence存在正常基数偏差，不能把计数差直接当待完成数量。 |

尾环没有保存 Safari 最终停住时的新 hang/recovery、SMMU fault 或 panic；这只限定已保存的记录。早期122秒的超时确实存在，不能写成“本次没有GPU挂起”。GPU性能计数器多数窗口未通过有效性检查，210包中最后可用区间止于120.898秒，不能用它推断219秒的满载率或实际频率。

## 帧文件尾段及下一步

`frames.previous.trace` 12,582,864 B，174,762 条记录全部非零，有效事件到215.961秒。当前 `frames.trace` 1,605,096 B，只有前57条非零（头部及少量215.962秒事件），后22,236条均为零；216.220秒是新段创建时间，不是最后有效GPU完成时间。原始文件留档，不把零填充当新段头或有效事件。零尾的具体原因尚未证明，不能反推215.95秒就是GPU停止时间，因尾环219.4秒仍推进。

源码复核发现此前“每两秒同步”表述过强：writer 实际每8个有数据的循环 fsync，空循环在计数前continue，且上游环读取/写入耗时会延长周期。因此这不是严格墙钟两秒的耐久保证。此缺口是后续持久日志修正项，尚无证据证明它造成系统冻结。周期包每30秒保存一次，最终约9秒的线程/硬件状态没有落入完整包。

下一步应保留正常渲染配置，先修正日志同步的时间条件和尾段有效性校验，并取得停止推进前后的独立线程与GPU状态；同时独立核查本次可符号化的文件节点启动崩溃。当前证据不支持更改频率、memoryless、并发分箱或切成单环来声称修复。

## 证据入口

所有新材料在 `uke-port/fixes/2026-11-13-frame-path-safari-freeze/`：`readback.json`、`data/capture.json`、`frame-path/captures.json`、两份export/manifest、`boot.json`、`time-anchors.json`、`unified-coverage.json`、`same-boot-ips/`、`safari-ips-symbols.json`、`dock-fault-symbols.json`、`final-frames/`、`final-frame-summary.json`、`findings.json`、`recovery-transition.json`。Safari/Doc栈匹配原SystemOS cache UUID `ce33b2ba-2ebb-3b58-a04e-1953e12112ae`，同时读本地符号和public exports；最近符号匹配本身不证明函数边界。
