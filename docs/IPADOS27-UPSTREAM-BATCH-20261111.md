# iPadOS 27 上游批量同步与重载修复（2026-11-11）

用户要求继续修复34DE约462秒重载渲染完成链停顿，并尽量合入上游这一批macOS优化中可用于iPadOS的部分。工作仍在 `feat/uke-ipados27-dsp-audio-power`，原main保留。各参与仓已建立 `backup/before-uke-ipados27-upstream-batch-20261111`。

本轮上游固定到core3fd6a3c、Adreno740122a8f1、AdrenoMetald84093b、GopFramebufferb0d812d、ArmDTPlatforme8bd8d5、XnuPrelink8f4f386、Ath12kWifi5f7f5a8、ShengUfs34ff541与shengb7ffb57。完整SHA和原有效pin、实际解析路径、构建前本地配置存于 `uke-port/fixes/2026-11-11-ipados27-upstream-batch/`。源文件继续遵循各上游MIT、BSD、APSL及既有厂商引用许可证，UFS三方合并以99d8bcd为基准保留Uke板级输入。

## 已合入源码

- Adreno740完整合入c7bbc9c、c4dcd53、122a8f1。pool预准备为空时锁内走既有VM页回退，避免锁内再次临时映射/清零。默认关闭额外cache行audit，gpu-audit=1可显式开启，cache属性维护保留。记录每个job时长、stall kick时CP位置。viewed grant仅在内核device pager确认最后映射消失后回收，未提供通知的旧内核继续保守退休。
- AdrenoMetal完整合入28个新增提交。包括资源purgeable语义、内存压力源生命周期、parked/空块按时老化、退出guard、YCbCr双平面采样、shader/compiler缓存位置、缓存访问时间失败处理、设备limits、telemetry、调用者来源记录和64MiB trace上限。Uke的CoreImage资源加载、轻量模式2/4、exact19.2MHz记录与已选memoryless策略保留，现有sheng-*参数通过token兼容继续读取。
- Metal的28个提交中带有TARGET_OS_OSX边界的WindowServer入口、IOGPUMetalDevice subclass、signed-text framebuffer策略和system shader cache跨UID挂载仍由相应OS条件选择。iPadOS继续使用backboardd及已存在的同UIDshader缓存。系统缓存读写核心已合入runtime，当前macOS的UID88和专属挂载路径未直接套给iPadOS。
- GopFramebuffer完整合入上游格式/路径计数、half-float诊断及平台条件化代码，保留Uke fence IRQ、callout、blank/latch及请求扫描修正。冲突解决保留原Uke测试抽取段，并采用上游对嵌套条件的结束边界修正。
- ArmDTPlatform同步板级时钟注册和calendar启动回退。ArmCpu和DarwinBoot已经包含当前上游，无新增运行差异。CPU继续由SM7675三域硬件LUT决定，GPU九档和L3一次最高档投票保留。
- Ath12kWifi合入cached packet metadata、RX日志和supplicant通用改动。AHB编译发现新增nw_wifi_rx_state调用未在公共头声明，已在nw_abi.h补齐既有函数声明，保留WCN6750 transport/control。
- UFS引入最新多slot并发、cached store维护、直接/staged命令诊断、solid-state属性、completion IRQ和短完成等待策略。原厂DTS的ufshc@1d84000中断SPI0x109即265，对应INTID297，与上游接入一致。三方冲突保留Uke WLAN SID0x1400/mask1、IOVA0xb0000000、MSI门铃和原48MiB DMA池，未引入sheng WCN7850的128MiB/不同IOVA板级配置。
- XnuPrelink同步24A446 kernel imports和原有OS范围内的symbol fixtures，macOS import按目标kernelcache选择。core合入最新main后，组件gitlink明确指向已包含上游和Uke差异的merge，避免构建子模块回退。

## XNU 12377 共用补丁

上游macOS profile中可复用的代码按现有arm64e/SoC/OS/device补丁顺序接入设备series，详细来源为kernel-patch-provenance.json。

- device pager的最后映射关闭通知，与Adreno740122a8f1配套。
- dyld pager释放当前busy源页后再读取其它4KiB片段，避免完整16KiB验证块上的自等待。
- 16KiB签名页的4KiB哈希统计、并行认证、首次使用时延迟认证和小块答案缓存。仍以同一私有16KiB缓冲验证原CodeDirectory哈希后产生4KiB派生哈希，坏签名继续拒绝。cs-lazy=1显式启用。
- loader内存统计、image tail可回收范围、早期page radix root、kernel collection已使用段回收和booter统计下溢修正。boot-payload-release=1在Uke唯一addon安装调用及所有挂载之后，且stage5/全部文件成功时，调用原IODTFreeLoaderInfo归还GpuAddon整页。
- work interval与timebase trap统计、vnode pager无锁诊断名、签名digest耗时、union下面目录遍历的name-cache锁覆盖与计数。
- sheng修正后的Edge矩阵及跨类型steal/idle排队计数，armcpu-edge=1与self-IPI一并按既有接口启用。仍保留调度锁/中断hygiene与原断言，未采用上游全局validation_disables和关闭hygiene策略。
- timebase-user-scaled-read只服务timebase-hz=24000000的5/4缩放，uke当前19.2MHz原时基不需要该签名文本变换，此项保持目标条件。macOS机器配置、认证卷身份、firmlink、键盘、蓝牙、温度限频/热压力和SM8550专用寄存器配置不并入Uke运行策略。

两个补丁只移除无关macOS相邻context以适配iPadOS现有code-limit/unionlookup guard，功能修改来自上游。payload-addon-release保留共用machine layer，在板级单独接入iPadOS调用点。

## 构建与后续门禁

本轮先完成正式kernel、arm64e Metal/runtime、payload、boot/vendor_boot和最终归档输入核查。现有34DE是用户确认启动/桌面通过的参照，D4EF仍为电池UI阶段回退。重载根因与性能收益待新候选实机证明。

主机检查聚焦实际编译、translator pin+全补丁、prelink闭合、包内源/签名/ADT/bootargs/分区上限与13区记录。未追加可选模型/压力套件。构建错误和中间状态均存于本轮fixes目录，后续在本报告追加最终候选身份。刷写仍逐候选取得当次授权，当前设备保持TWRP。
