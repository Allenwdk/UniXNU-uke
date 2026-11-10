# A5E3 调度锁超时与持锁调用点取证（2026-11-11）

用户报告候选 `A5E3AA6C0356E4BF` panic，提供 `IMG_20261111_035410.jpg` 并要求取日志继续修复。该包已标 DO_NOT_FLASH。本轮先对照最新上游和实际已刷内核，再进入只读取证。完整根因仍未确定，当前新增代码用于取得持锁调用点。

## 已确认现场

已刷最终内核 SHA-256 为 `0cdc80131a41bf38fff4c7afb045867fb121ddefe19f9f55cc98e154b8b7d7d0`。照片锁地址 `0xfffffff00b882700`，实际符号 `pset_array_actual=0xfffffff00b882600`，偏移 `0x100` 对应 pset0.sched_lock。按 LR-4 校对调用指令，等待链为 processor_idle → thread_select → lck_ticket_contended → hw_lck_ticket_contended → hw_spin_should_keep_spinning → lck_ticket_timeout_panic。

照片显示 cticket=f5、nticket=fa、waiting=f7，持锁线程 `0xfffffff5a7648398` 在 CPU0，当前与初始持锁者相同。start=1942857129、now=1942869925，差值12796 ticks，timeout=12000、irq time=0。在19.2MHz时基下，等待约666.46µs。XNU hw_spin_compute_timeout 在关中断且 hygiene 为 PANIC 时会选择较小的 interrupt_masked_timeout。该照片只能定位等待与持锁线程身份，尚缺持锁者的获取锁调用点和运行栈。

## 日志边界

取证目录为 `uke-port/fixes/2026-11-11-a5e3-ticket-panic/`。

- 尾环 `reference/logs/ukelog/ukelog-20261111-035737-a5e3-ticket-panic.bin`，33,554,432B，SHA-256 `caf833baaa2377934c6b383718ac1fcb3d79a88ee08ecce9ebdd8a68271d1036`。
- 与紧邻 CED0 原始尾环逐槽对照，99个新增槽全部CRC有效，排除1949个旧槽。新增记录19.936337–100.488238s。配对文本与 current-ring-clean.log 保留。
- 同次 GPU 已于63.367409s发布并启用 preflight。81.076973s pool 为9/9范围、5,198,080KiB、alive0，围栏IRQ6、completion pass35。该采样早于panic，不能推断panic瞬间尚未分配池内存。
- L3同次level10、请求1,593,600kHz、vote读回10。电池100.001955s仍为99%、4277mV、-1254mA、external1/charging1。
- Data只读reflink快照完整读取12,310,069,248B，较CED0基准2448页变化。第一次中断的宿主快照标为Data-interrupted.img并保留；第二次从同一不可变基准完整重读。主机没有写擦设备userdata。
- timesync新增启动UUID为5A21FB0C-173E-4A0E-AF11-F04F862E3D16。解析4,531,949条统一日志均属于此前三个boot，本次UUID记录为0，全部排除。pstore为空，panic正文和CPU0持锁栈未落盘。
- 取证后设备保持原厂四boot/vendor_boot分区、A槽、TWRP。FUSE已卸载。

## 上游核对

本轮重新fetch的core为874c8bd5845da097cbcd8f98e9e73c63b3d06e93，sheng为43f6f010114d66554b26710252bde5f37418e95a，ArmCpu为d90e457，Adreno740为5282bd9。

ArmCpu相对上游保留Uke的SPI模式SGI扫描门控和64槽/64位共享中断路由。CPU governor保留sheng算法和Uke三域硬件LUT，L3沿用已确认资源和最高档投票。Adreno已包含最新5282bd9按run临时映射，并保留Uke显式WC描述符和secure_memset。sheng XNU的cpu-idle helper与当前文件相同。未找到能直接证明本次pset0超时由上述差异导致的证据。

core新增82a8e54是macOS profile的page radix root启动初始化，sheng43f6f01改动macOS签名framebuffer策略和参数。当前24A446没有启用boot-payload-release，此轮未引入这些macOS改动。来源和许可证继续沿用各组件记录，XNU诊断改动遵循原文件APSL-2.0。

## 下一候选的范围

设备提交4982e71加入 `uke-pset-lock-owner.patch`，仅为pset0保留成功加锁的时间、线程、ticket和三层有界本线程栈。覆盖正常/非抢占锁及成功try-lock入口。记录操作不分配内存，不打印，不进入调度器，不访问其它线程栈。超时时用sequence、thread和ticket检查快照是否仍匹配当前持锁者，失配显示valid0。

该实现复用XNU原锁接口、原子操作和内核栈边界契约。锁阈值、排队、抢占及解锁算法保持原实现。候选的用途是补齐CPU0持锁现场，尚无根因修复或实机通过结论。正式kernel/payload及最终链接核验完成后在本报告追加身份。后续部署仍需该候选当次授权。

