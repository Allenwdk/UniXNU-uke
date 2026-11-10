# E8AF GPU 池清零 panic（2026-11-11）

用户在 E8AF2F935F600E17 报告 panic。故障包已标记 DO_NOT_FLASH，保留原镜像和部署回执。设备通过既有取证流程恢复原厂四分区、切 A 并进入 TWRP。主机通过只读方式取回 userdata 尾环。

原始尾环 SHA-256 为 `788a5766a43bbd769f07e8f42af75a8a66929775918c7973f091b20f76b19774`。对比先前 3EAE 尾环，407 条 CRC 有效变化槽的序号为 0–406，排除 1641 个相同槽。最后记录时间 254.949011 秒。照片、同版反汇编和恢复回执见 `uke-port/fixes/2026-11-11-cpu-gpu-sync-panic/`。原始尾环及配对 boot 文件路径见 current-ring-proof.json。

照片的 PC `0xfffffff00b7c04e8` 对应 `bzero+0x48` 的 `dc zva, x3`。还原的 LR `0xfffffff00b60a8f8` 位于 NativeAdrenoPoolScrub，LR−4 为调用 bzero 的 BL。ESR `0x96000061` 的 DFSC 为对齐故障。FAR 等于 x3 的 `0xfffffff8b3a00040`，其低六位为 0。该指令之前的四次对齐 STP 已完成，现场可还原清零长度为 0x5000。尾环也记录了 GPU 池开始划出 20 KiB。当前没有故障地址的物理页和现场 PTE 原值，20 KiB 只能作为互相吻合的线索。

## 上游对照与修正

Adreno740 upstream `9d99240` 的池清零沿用 bzero，默认其 WC 映射具有 Normal 内存属性。E8AF 原样继承了这一假设。当前 XNU 的 wimg_to_pte 对 WC 只检查 is_dram_addr 或 pa_valid。DarwinBoot 提供的窗口外池范围不在 VM 管理窗口，窗口外低位 DRAM 也无法靠单一 gDramBase 范围识别，WC 请求会落到 Device 属性分支。这解释了为何已对齐的地址在 DC ZVA 仍触发对齐异常。XNU 自身在可能遇到 Device 内存的物理清零分支调用 secure_memset，汇编使用普通存储。

修正将 DarwinBoot 最终内存图提供的 conventional DRAM 范围复制进 XNU 的启动只读表。表受页对齐、溢出、排序重叠和 XNU 窗口重叠检查约束，GPU 池注册需要完整范围命中。仅对应物理页的 WC 映射选 Normal，VM 不接管这些页。CPU 的池描述符与用户池视图均显式请求 WC，池清零复用 XNU 的 secure_memset 并保留 DSB。其余 GPU 池分配、释放、视图生命周期继续用上游实现。修正适用范围是 24A446 的 Uke XNU 补丁和 Adreno740 池入口。

E8AF 同次日志记录 L3 第 10 档、请求 1,593,600 kHz、寄存器读回 10。GPU 围栏中断已到达完成处理。电池有 42 次快照和外接状态变化，ADSP ready 371 ms，APM 返回 ready。池录入 8 个范围共 741,696 KiB，另一个高位范围报 NoMemory。L3 读回是投票索引，实际频率需独立测量。此 boot 因 panic 终止，修复后的 GPU 池和整机回归待新候选验证。
