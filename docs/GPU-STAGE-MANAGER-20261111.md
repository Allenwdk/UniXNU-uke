# 台前调度重载停滞与GPU运行状态（2026-11-11）

当前候选34DE79C17FE8A795已获用户确认启动无panic、进入桌面及上划不卡死。用户另报开机约7分钟、台前调度切换应用时疑似GPU卡死，重载后感觉变卡，并推测可能属于此前问题。是否同一故障、GPU是否停止推进以及频率/恢复状态，当前都待同次日志确认。

取证目录为 `uke-port/fixes/2026-11-11-gpu-stage-manager-readback/`。先记录用户观察，再按已授权原厂四分区/A槽/TWRP流程读取尾环及诊断数据。按当次boot与紧邻A5E3基准去重，重点检查360–480秒附近的提交/退休、fault、hangcheck/restart、HFI频率请求与读回、pool回退、内存压力和CPU调频。运行模式按实际已刷输入及硬件读数核查。

本轮先调查并记录，启动行为通过尚未覆盖重载和长时间稳定性。

## 同次启动与取证

镜像34DE79C17FE8A795，最终内核SHA-256 `2c84518515c1dbd4d4e5ef4c26e7993efb58731812e74b2ee4f9ee1516262848`，vendor_boot SHA-256 `4e6564741832644ee3f2f4449b3df7d5bb62465c79122deaab6004a76e0e778a`。同次boot UUID为 `2B3AA199-0C6A-4E07-A90D-FD75EB21BB1F`。

原始尾环 `reference/logs/ukelog/ukelog-20261111-043320-gpu-stage-manager.bin`，33,554,432B，SHA-256 `b53d93c7eb17855d12cdca4562e67d1d460ba082dacab9d64283ff1d9e34a6f0`。837个新CRC有效槽，1211个旧槽排除，范围20.105372–469.908234秒。正文在配对boot文本和current-ring-clean.log。

完整Data只读reflink读取12,310,069,248B，相比A5E3基准72516页变化。解析同次统一日志2,390,571条，5份IPS匹配boot UUID，另外5份新增/重写IPS属于其它boot而排除。统一日志的1970壁钟不用于定位开机秒数，秒数取尾环与renderer trace，再按UUID、PID和逐字事件配对。

诊断export在201.045716秒完成，51,489,328B，SHA-256 `80d9b575a41638f2cc190272970d7abaebd412314df21244af04f0ff2af9b84f`，18文件。该快照的内核GPU细粒度数据只覆盖早段。另从Data取回持续写入的backboardd frames.trace，15,695,784B、217997个72B记录，包含后段渲染数据。源码确认只有backboardd写此文件，以O_TRUNC创建，避免把其它进程/旧文件合成一次捕获。

取证后原厂四boot/vendor_boot分区、A槽、TWRP保留，FUSE已卸载，主机没有写擦userdata。未生成新的运行候选。

## 七分钟附近的进度与最终停点

| 开机秒数 | 内核submit累计 | GPU围栏中断累计 | 完成pass累计 | 围栏睡眠超时累计 | ring切换计数 |
|---|---|---|---|---|---|
| 402.041 | 8459 | 8464 | 10293 | 0 | 328 |
| 422.088 | 9431 | 9436 | 11419 | 0 | 363 |
| 442.132 | 10064 | 10069 | 12076 | 0 | 363 |
| 462.179 | 10669 | 10666 | 12695 | 0 | 365 |

此前GPU仍有进度。后段renderer完成记录在462.078321秒止于commit8961，前8961条完成序号连续。随后71个commit8962–9032在462.117182–463.268447秒到达runtime提交入口，未见对应完成记录。最后writer事件463.421288秒，内核/Wi-Fi日志继续至469.908234秒。此处是疑似渲染完成链停顿的具体现场。

完成记录使用32槽CommitRecord并要求回调时index仍匹配，积压超过32个时会丢失被覆盖的完成记录。71条缺失记录不能直接等同71条GPU未退休工作。后段还缺ring/CP/SMMU现场及持有提交锁的线程栈，尚无法区分GPU硬件停转、提交锁阻塞、runtime/回调积压或记录线程阻塞。当前尾环没有NativeAdreno recovery行；462.179秒的timeout0采样早于上述链路停点，不能据此排除稍后故障。

## 重载后的延迟

取持续写入的renderer command_summary，比较的是command buffer提交至完成回调耗时。各窗口工作量不同，这些数据用于定位本次延迟，不作为相同场景的性能A/B或屏幕FPS。

| command创建所在开机窗口 | command数量 | 提交至完成P50 | P90 | 最长 |
|---|---|---|---|---|
| 350–360秒 | 213 | 13.220ms | 14.187ms | 16.163ms |
| 370–380秒 | 445 | 26.571ms | 46.091ms | 98.972ms |
| 420–430秒 | 273 | 27.156ms | 46.607ms | 198.715ms |
| 430–440秒 | 296 | 29.600ms | 47.611ms | 58.832ms |
| 440–450秒 | 237 | 22.695ms | 47.626ms | 109.867ms |

完成链停点之前已存在明显长延迟，符合用户重载后变卡的观察。该耗时包含排队、GPU执行和回调，不能全归为GPU执行时间。

## GPU运行模式与频率

- 实际参数为gpu-preempt=2、gpu-dcvs=1，输入的九档为255/353/500/633/684/736/835/900/950MHz，保底500MHz，触摸boost默认1000ms，DCVS span=1。63.539224秒日志确认9档、floor500/top950。软件升降档及触摸升档持续更新到462秒，失败投票累计为0。
- 初始GPU trace确认preemption enabled、priority_mode2，两个ring都有提交。切换计数至462秒仍增加。LLCC61.466961秒结果0，四bank，GPU9/HTW11均active0→f，GBIF路由2500/1294a529。
- 420–450秒实际8755个render pass中，7873个走GMEM，882个为SYSMEM强制/必要路径，适用encoder的transient策略始终为ASCII103即g。未见重载后统一切换渲染策略的记录。单个pass模式由附件与负载决定，GMEM数量不单独证明最优性能。
- 169.314297–200.793081秒的293个有效、同epoch、同投票窗口中，CP_ALWAYS_COUNT差分/实测间隔得到约949.99MHz中位数，对应档位9。此结果仅覆盖早段。
- 后段的longest=...@950字段来自软件cur对应的OPP表，HFI成功也表示请求获响应。350秒以后缺实际GFX时钟测量，因此不能确认重载后实际频率一直满足请求，尤其是462秒完成链停点之后。
- CPU侧也有相关负载证据。402秒附近繁忙窗口，A720/X4请求已在最高档，而statePct平均为86%/81%；422秒附近为84%/79%，462秒附近恢复至96%/93%。这些是繁忙窗口的硬件domain-state与请求比值，不能据此断定具体固件/温度限制原因。L3仍为一次最高档投票level10、请求1,593,600kHz、读回10，实频尚未测量。

可以确认优化路径和动态投票在停点前仍工作。“始终处于最佳模式”需要后段实际时钟、工作量与等待栈证据，当前不足以作该结论。

## 同次应用服务异常

5份同boot IPS包括DockFolderViewService两份、Files、Preferences与MobileSafari。

- DockFolderViewService pid423/527在约200/370秒报告EXC_BAD_ACCESS，FAR=0x10。两份栈均从DesktopServicesPriv的TNode::GetNodeFromPath进入，随后为GetNodeFromURL、FINode和DOCNodeStartupManager。Files pid447同次也在文件节点处理路径访问0x10。这是已定位的应用服务崩溃，完整底层原因尚待目标框架/文件系统契约核对。
- 426.581802秒WebKit.Networking pid587、426.584812秒WebKit.GPU pid588记录EXC_GUARD。MobileSafari pid579同boot430秒报告为WEBKIT命名空间的用户fault。该时间接近用户描述的七分钟。
- Preferences pid401同boot有scene-update watchdog超时10秒报告。尾环445.632559秒另有pid455的EXC_GUARD。
- 上述事件可影响台前调度、应用切换和观感，与最终462秒渲染完成链停点的直接因果尚未证明。同签名文件节点崩溃也见于其它boot IPS，但未配对到具体旧镜像，暂不据此判定本轮回归或旧问题。

## 最新上游差异

本轮fetch到Adreno740 `122a8f169f1c068d011318d6739f501821683259`、core `3fd6a3cf0e6f2eba5d3de3a4d1912e2db23a0fc4`、sheng `b7ffb57`。当前运行Adreno仍为8dae4b5，包含此前5282bd9按run临时映射。以下是本轮新发现、当前包尚未包含的非温度改动。

| 提交 | 作用 | 与本次关联边界 |
|---|---|---|
| c7bbc9c | 预准备为空时，锁内不再次调用pool分配/临时映射/清零，保留既有VM页回退 | 源码确认当前仍有锁内二次尝试路径，可能造成全客户端submit/map/wait竞争，本次停点尚缺持锁栈 |
| c4dcd53 | 缓存行audit改为gpu-audit=1才执行，并记录每个job时长和stall kick的CP位置 | 可降低诊断开销并补充后段等待证据，实际收益未在uke验证 |
| 122a8f1 | 内核确认物理descriptor最后映射消失后才把viewed grant归还池 | 需配套gpupool-device-pager-close内核通知；当前上游配方只在macOS profile启用，24A446需要核ABI与链接后适配 |

当前默认gpu-pool=1对被用户态映射过的释放grant采用保守退休策略。462秒采样pool总5,198,080KiB、free1,337,868KiB、活跃1,565,572KiB，按差值估算退休约2,294,640KiB（2.19GiB），free约1.28GiB。相比361秒的退休约1.05GiB明显增加。池尚有空闲，当前证据不能归为耗尽；长期回收和碎片状态值得在配套上游最后映射通知路径中继续核对。保持原映射生命周期安全门禁。

上述三个提交消息均标记尚未实机运行，当前不将其写作本次故障的已证实修复。来源为[锁外pool修正](https://github.com/UniXNU/Adreno740/commit/c7bbc9cd675494c7464bb83a82bf92a8aeb5dde3)、[audit与job取证](https://github.com/UniXNU/Adreno740/commit/c4dcd5391f0dfc8548a008e0751db0ddcb99e8ab)、[最后映射回收](https://github.com/UniXNU/Adreno740/commit/122a8f169f1c068d011318d6739f501821683259)。

## 其它同次发现与下一步

379.451238秒电池field22超时，进入generation1 quarantine，后续answer总数停在1177。本轮启动与桌面通过没有覆盖电池长期刷新。传输超时后的隔离行为符合现有无事务ID协议策略，重新建立链路后恢复刷新仍待适配与验收。

后续优先对照上游c7bbc9c的锁内pool回退路径，结合c4dcd53的job/CP停点数据取得462秒之后的等待现场。122a8f1必须与内核最后映射通知一起核验。GPU后段实际频率、CPU domain-state限制原因与文件节点服务崩溃分别记录，当前不把重载停顿宣布结案。

主要证据为evidence-summary.json、timeline-metrics.json、completion-stop.json、frames-summary.json、modes-summary.json、same-boot-crashes.json、paired-unified-events.json、source-comparison.json与原始日志/快照。
