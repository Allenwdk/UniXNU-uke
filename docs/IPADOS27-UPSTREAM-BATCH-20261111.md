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

## 最终候选与验证边界

候选 `6D31A137E8F25F63` 已归档至 `uke-port/releases/2026-11-11-ipados27-upstream-batch/`，未刷。boot SHA-256 `e8cbc39f13b40300d930915b7da3b9ab3ec2c5b63af03292c7ae88215cf9b289`，vendor_boot SHA-256 `3b18aecefc024409635da2714afdc1678c4d5abd16c51ce61797a6df30354875`，47,525,888B，小于96MiB。最终内核SHA-256 `5145915df7c3db2cc03ca9b0b68ef1afdf6733fb7daefa92565f50a21f274522`。配对使用此包boot/vendor_boot。

core源码 `b3b1a0d1a7af707a284f4ade4635ac3374978b60`，设备归档源码 `132b81b7d7cf105fa572cec7249d393922a888d1`。Adreno740936c138、AdrenoMetalf984b08、GOPf0c18e0、ArmDTPlatform638c39a、Prelink6442513、Ath12kWifie6abc94，完整SHA在source-revisions.json。11仓的已选远端main均为最终本地HEAD祖先，source-merged记录逐项核验。原Uke差异和main基点保留，未推送。

正式kernel、用户态Metal/runtime、payload、bootimg、vendor_boot通过。translator仍用UniXNU配方963f0e64固定pin，加32个记录补丁，checker逐字树核对通过。它相对译码器自己development分支落后1805提交，本轮没有把未经UniXNU适配的新译码器代码混进配方。arm64e runtime已从更新后的Rust与Objective-C实现重新生成。

插件CDHash a39a0dcfbab9571f6035dd12d6690fe5d2dee70c，Turnip CDHash035dc50f53ab10a3886aa7e041822ab2fd1c8162。其基础Turnip源码/固件pin保持原Uke；重链接/flat-import处理和签名后的实际库随新插件成套封入。15个addon文件只替换图形插件、Turnip库和gpu-build.json。6个owner trust模块中只在既有图形模块替换对应两个CDHash条目，原系统/外设签名与其余addon字节保留。两个二进制均为arm64e，CodeDirectory按4096B代码页逐页复算通过。

83项平台/显示源与14项存储源和实际archive哈希通过。prelink缺失仍2处，桩从24减为19，现有回归policy通过，未增加白名单。最终链接代码确认device-pager通知、lazy-auth、loader释放、Edge计数、UFS完成IRQ、GPUjob/kick/pool视图和原pset取证符号。原WC范围授权与secure_memset、16个outside ADT槽、721B启动参数及18项输入/显示ADT保留。ADT除random-seed以外相同。FD/DarwinBoot沿用已核验E8AF固件，SHA匹配，正式bootimg产物与34DE boot相同。

归档SHA覆盖70文件，13区结构化检查通过。本轮只运行必要构建、译码器pin/实际补丁树和产物/签名门禁，未追加可选宿主压力套件。失败记录保留，包含iPadOS补丁context、AHB函数声明/不支持的HTT请求门控及用户态链接工具环境修正。

本轮主要处理的是已证实存在的锁内pool回退、viewed grant无法回收和通用缓存/内存生命周期问题。34DE的462秒停顿是否由这些路径触发，以及新候选的性能与稳定性收益，仍待实机。新启用cs-lazy、Edge和loader回收扩大了本轮变更面，硬件需同时回归启动、二次冷启动、超过8分钟台前调度切换、GPU工作退休、pool tracked/closed/returned计数、长job/CPkick与电池/Wi-Fi/触摸/背光。

温度限频和热压力策略继续按既有用户范围排除。macOS机器身份、签名framebuffer地址、跨UID系统缓存挂载和5/4时基修正保留其OS/目标条件。24A446仍用19.2MHz原时基、Uke频率/LUT与L3一次最高档。电池field22超时后的重新建立链路与文件节点TNode/WebKit异常保留独立待办，本候选没有将它们标为修复。

当前设备保持原厂四分区/A槽/TWRP。本轮没有设备写入。取得本候选当次授权后按四分区/A槽部署。34DE配套包作为启动/桌面参照，D4EF配套包作为电池UI阶段回退。


## 2026-11-11 iPadOS上游批量同步版授权部署

用户授权刷入 `6D31A137E8F25F63`。同包 boot 和 vendor_boot 已写入四分区，A 槽读回，重启命令成功。70 项归档 SHA 与 13 区检查通过。回执 `uke-port/releases/2026-11-11-ipados27-upstream-batch/flash-result-20261111-053119.json`。启动、超过8分钟台前调度重载、pool回收与外设回归待实机验证。
