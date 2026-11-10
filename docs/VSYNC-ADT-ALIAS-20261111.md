# 6D31刷新参数被ADT入口拒绝（2026-11-11）

用户在6D31A137E8F25F63报告显示有撕裂感，并询问vsync是否打开。已刷包的ADT确实包含 `sheng-vsync=120 sheng-vsync-fixed=1 sheng-dpu-scanout=2`，但本次运行日志确认120Hz软件刷新定时器没有启动，DPU直扫也没有工作。

## 已证实的遗漏与运行证据

上游GopFramebuffer把PE_parse_boot_argn查询名称改成 `vsync`、`vsync-fixed`、`dpu-scanout`。Uke在内部参数解析器中已经兼容新旧名称，但 `uke_parse_adt_driver_arg` 先使用allowlist检查查询key，该allowlist只比较 `sheng-*` 原名称。三个canonical查询先被拒绝，因而根本没有进入内部alias解析。最终调用方沿用零默认值。

此前34DE的GOP仍查询旧名称，因此这处兼容遗漏随本轮GOP合并暴露。上轮仅核对ADT字节未变和内部alias代码，遗漏了入口allowlist与新驱动查询名称的端到端检查。

同次原始尾环 `reference/logs/ukelog/ukelog-20261111-054014-vsync-adt-alias.bin`，33,554,432B，SHA-256 `6c0cc8c0d95376d3c6387bb4fdbd44f41e5c95b6ae68a2542924ea3bc56021ba`。与紧邻34DE原环逐槽对照，772个新增CRC有效槽、1276旧槽排除，覆盖19.952694–404.940538秒。

35.375579、37.083560、39.654118、93.799586及105.009626秒均打印 `UKE DISP vsync hz=0 sent=0 arms=0 idle=0 late=0 start=0 refresh=0`。

200.899150秒成功导出57,104,734B，SHA-256 `9f569a1c8783042fda6c3fb05c4689e216260787dd9d5dcb375e1fb77bf4f1f5`。kernel.rings.txt记录 `copy 2876、flips 0、latches 0、fence waits 0`，fence IRQ观察已接入但wake/irq_retries/timer_retries/gated_entries均0。与新版startDirect的mode默认0及参数读取失败相符。

降回GPU拷贝到现有扫描缓冲的路径具有扫描中写入的条件，与用户撕裂观感相符。该证据证明刷新配置与显示路径回归，修正版是否消除实际撕裂仍须设备验收。vsync定时消息本身也不能替代DPU换帧与latch时序的验证。

## 修正与实际入口检查

设备d9f483a在allowlist入口对查询与白名单项使用同一sheng-前缀归一化，继续返回同一key索引，因此别名重复同样被拒绝。core8d7872f同步payload验证与报告归一化，命令行覆盖ADT的原有优先级保留。旧版ADT内容仍有效，无需增加重复CLI参数。

将6D31准备源码的完整bootargs.c与ADT头编入实际入口控制，canonical查询全部失败且输出保持0，旧查询得到120/1/2。结果在 `negative-control/canonical-query.log`。

设备9a98570扩充既有check-adt-driver-args.py，对原生产parser执行canonical查询、旧ADT、新ADT、命令行覆盖、别名重复及畸形值检查。旧prepared头的负对照失败，修正后同一生产parser检查332项通过，ASan/UBSan无错误。该检查覆盖了上轮缺失的实际新驱动key入口。

本轮运行源码仅更改参数入口/验证/报告及既有回归检查。新版GPU/Metal/UFS/XNU通用修复继续沿用6D31，原图形addon、trust、CPU/GPU频率与显示寄存器配置保留。正式kernel/payload及配对boot/vendor_boot构建后在此追加候选身份。

## 设备与后续门禁

取证按既有原厂四分区/A槽/TWRP流程执行，主机未写擦userdata。当前设备留TWRP。本轮没有读取整份Data快照，现有尾环和诊断export已能证明参数/直扫回归。

6D31包已标DO_NOT_FLASH，后续使用修正版。实机需确认刷新日志hz120/start1/refresh1、DPU直扫flip/latch恢复、桌面与台前调度无撕裂，再回归按键/背光、电池、Wi-Fi及>8分钟重载稳定性。新的候选仍逐次授权部署。
