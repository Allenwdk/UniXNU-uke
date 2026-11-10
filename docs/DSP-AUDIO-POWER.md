# uke iPadOS 27 DSP、音频与电源

本项按[官方流程](https://unixnu.com/zh/porting/power)分阶段实施，目标为
24A446 arm64e。第一阶段代码接通 ADSP 传输、电池只读服务和 AudioReach
APM 就绪查询。259E 同次启动已确认 ADSP、三个 GLINK 通道和 APM 回复。电池服务未发布，用户看不到电量与充电图标。PCM 流、FS19xx 功放、WCD939x 采集和 HAL
服务接入仍在后续阶段，整项尚未完成。

## 输入与复用

- sheng 固定到 `6bc0ce1e8c3a3733110a80dd80ddfb2a1ca46555`，原始源码
  SHA-256 和固件来源在 `firmware/uke-adsp/manifest.json`。复用 MDT、
  SMEM、SMP2P、GLINK、QRTR 服务定位、GPR 和 AudioReach 编码。
- ADSP 的 Cliffs PAS 为1，独立 DTB PAS 为0x24。预留区域分别为
  0x9d400000/0x4500000 和 0xa1900000/0x80000。启动使用固件分区现有
  `adsp.bNN`、`adsp_dtb.bNN`，元数据经安全世界认证。版本及完整二进制
  哈希已通过2026-11-11只读取证补齐到清单，元数据与同次 PAS 日志一致。
- 设备树的 AP IPCC 为0x406000/SPI229，AOSS 为0xc300000/0x400，
  AP RSC DRV2 为0x17a20000，SPI5，TCS 块偏移0xd00。传输核心从
  SM7675 共用仓复用，共用一个 IRQ 和 QMP 状态机。
- UEFI 中两段 ADSP 内存均为 Reserv。原厂 DT 的42个保留区域对照
  77个 UEFI 描述符，静态错误和缺失厂商输入均为零。启动后内存和
  固件所有权仍须回读。
- LCX/LMX 请求 OEM TURBO 384，由 cmd-db 解析到 ARC 索引，XO 请求3。
  WPSS 和 ADSP 按资源地址取各自投票最大值。ADSP handover 只撤销自身
  投票，WPSS 原有持续投票由其调用者继续持有。

## 第一阶段接口与异常处理

`adsp=1` 只读调查，`adsp=2` 认证并启动 ADSP，随后协商 GLINK。
`battery=1` 显式启用电池服务，`audio=1` 只查询 APM 就绪。
ADSP 启动时先登记 IPCRTR 服务定位通道，电池使用
PMIC_RTR_ADSP_APPS，APM 使用 adsp_apps。保留 ShengAdspClient 接口，
回调线程的电池和音频响应复制到有界队列，消费者线程处理并等待。

电池只发送基础属性 GET 和通知订阅。OEM 属性12是恒流，温度为13，
容量单位0.01%，电压/电流微单位，时间秒。发布时转为百分比、mV/mA、
分钟，电流按参考协议转换为充电正值，实机需与原厂读数比较。
板级扩展电量计属性尚未确认，因此不会请求 sheng 的扩展 ID。

首批必需状态、容量、电压、电流、温度和 USB online 读数完整后才发布
IOPMPowerSource。超时按绝对 deadline 检查，旧值标为过期并设置
ErrorCondition，当前传输代次隔离。链路关闭清除队列与状态，重新打开
同一代次不能解除隔离。当前版本 fatal 或损坏传输停止消费者，干净重启
后再重新建立服务。DSP 原地重启和自动恢复仍须实现及验证。

QMP 超时保持未确认邮箱，后续请求先等原消息确认。RPMh 超时保留
不确定状态并禁止继续编程。FAT 卷缓存和 SMEM 分配均增加 AP 软件锁，
后者仍使用原硬件互斥锁。

## 后续端点

依据 Senemos `fd33b9174872395fc215bfad5127e525eb423467` 的 uke 路由。

| 端点 | 板级输入 | 后续门禁 |
|---|---|---|
| 内置扬声器 | QUP1 SE3 I2C 0x88c000，四颗 FS19xx 0x34–0x37，复位 GPIO30/149/124/123，VA TDM GPIO6–9，48kHz 四个32位时隙 | 读取芯片身份、原厂预设、保护参数与 ACDB 拓扑后接入播放流 |
| 内置麦克风 | WCD939x，TX SoundWire 0x6d30000，48kHz，ADC1/3/4/2 映射四路输入 | 核实供电、复位、枚举 ID 与 Uke 图，逐路录制已知输入 |
| iPadOS HAL | sheng 音频服务及 ShengAudio.driver，arm64e-apple-ios27.0 | 直接 PCM 通过后验证24A446 ABI、时基、签名和系统应用 |

2026-11-11已只读取回原厂 FS19xx 预设及 Forte ACDB，源文件保留 ignored 缓存并记录哈希，预设结构校验通过。主机仍缺完整 CoreAudio HAL SDK 头文件，HAL 正式构建使用具有完整 Apple SDK 的 macOS 环境。每个端点均在自己的
直接流通过后发布，格式、声道和路由由已完成的硬件路径决定。

## 验证与取证

主机入口为 `python3 scripts/check-uke-dsp-host.py`，覆盖实际电池解码器
和实际共享资源运行时代码的寄存器模型。上游 ADSP 和 AudioReach 主机
检查分别为4184项及182项，实际电池解码205项和生产通道状态10项通过。共享资源测试覆盖 RSC v2/v3、混合门铃、
两个线程各100条 QMP 请求、超时和投票撤销。以上均为主机证据。

第一阶段上机读取 UKE ADSP、UKE BATTERY、UKE AUDIO 日志，检查
PAS ready、通道开闭、元数据哈希、APM 响应字节和电池快照。插拔电源，
核对原始值与 OS 更新，同时回归 Wi-Fi、触摸、显示、调光及 CPU/GPU
调频。每个候选取得当次授权后部署，按 tag 配对日志。

## 第一阶段构建记录

候选 `259E2019012F832B` 已归档到
`uke-port/releases/2026-11-10-adsp-battery-apm-v3/`，vendor_boot SHA-256
`806135761adb615d3bdfc327ed8961035c0f257572b733b90bf3597e02bf36c7`，47,411,200B。
721B 参数和18项 ADT 参数，正式 kernel/payload 与源码/链接/镜像检查通过。
源码为 core cc87305、设备239e407、SM7675 2f12b23，均在本项新分支。
完整记录见 `docs/releases/2026-11-10-adsp-battery-apm.json`。
2026-11-11 用户本轮授权后，四分区写入、A 槽回读和重启命令成功，回执 `uke-port/releases/2026-11-10-adsp-battery-apm-v3/flash-result-20261111-001649.json`。ADSP、电池、APM 和 Wi-Fi 共存待用户反馈及同次日志。

## 2026-11-11 电池发布顺序修复

同次启动 UUID `77F87F49-A9ED-41CE-9345-EC65848E888A`，ADSP ready370ms，
IPCRTR/PMIC/GPR 通道打开，APM 收到token2状态1回复。powerd 记录没有
电池包，SpringBoard 缺 BatteryInstalledKey。原始同次日志没有请求进度
计数，具体电池刷新阶段仍待修复版的新增日志。

实际解码器测试复现了通知插入一轮刷新后，finish 在返回 BmCycleDone
前调用下一轮 refresh，使完整快照的有效位变为000e0000，必需位bf全失。
修正为由 next 延后启动排队刷新，消费者先发布，回放有效位为03fe1fff。
慢刷新请求保留较高优先级。新增176项通知风暴/排队/快照/超时检查通过，
205项电池、10项生产通道状态和RSCv2/v3各38项仍通过，旧代码负对照失败。
同时新增有界回复前缀、订阅发送结果、工作循环和发布有效位日志。

这项缺陷的主机因果已证实，尚不能以现有实机日志确认它是 UI 症状的
全部原因。新候选待当次刷写授权和实机发布、单位、插拔充电验收。
取证在 `uke-port/fixes/2026-11-11-adsp-battery-readback/`，完整报告在根
docs/status/adsp-battery-readback-2026-11-11.md。设备当前为原厂四分区、
A槽和TWRP。

修复版正式候选为 `0847D1BCA4BBE9BD`，vendor_boot SHA-256
`b8b691d06fb46155a8e53922e4cb7ab2d6d8b1e1b4eb500d572c998cc3c6caf2`，源码2c43669，721B参数和18项ADT。
镜像记录见 docs/releases/2026-11-11-battery-publication.json。2026-11-11 用户本轮授权后，四分区写入、A 槽回读和重启命令成功，回执 `uke-port/releases/2026-11-11-battery-publication/flash-result-20261111-004514.json`。电池服务发布、读数和充电图标待用户反馈及同次日志。

## 2026-11-11 消费者投递回读

0847实机仍无电量和充电图标。同次UUID96711F13-AF9E-43D8-8FD2-38865F39B3DC，
376个新尾环槽、2224095条统一日志。ADSP ready372ms，PMIC/GPR通道打开且
APM状态1，但电池新增subscribe/inbox/response/refresh日志均为零，OS无电池服务。
上一轮刷新顺序修复未覆盖实际更早的消费者投递阻塞点。

复用本项目DA49背光已验证的thread_call加IOWorkLoop runAction路径，
状态和接收回调继续只更新有界队列并唤醒，协议处理仍在原work-loop gate。
加入六阶段投递计数，并由独立UkeLastLog线程每20秒记录一次，避免消费者
停住后诊断也无法输出。63项实际生产回调/队列/gate/并发到达检查通过，
205电池、10通道、176快照和RSCv2/v3各38项仍通过。

旧链接代码中0xf8事件源调用槽已核验，但缺回调进入/返回计数，当前不能
确定具体哪一步停止。新路径硬件效果和充电图标待新候选实机验证。
根报告 docs/status/battery-dispatch-readback-2026-11-11.md，完整只读取证
位于uke-port/fixes/2026-11-11-battery-publication-readback。

投递修复版 `E0FE5A52E8C1EAD7` 已正式构建，vendor_boot SHA-256
`cbbc566e504a094bfbc3cfcc91859fe13e71f5c531f09dd7eb4fed7b24d5dfc3`，47,411,200B，源码5d9e511，721B参数。
运行时改动为ShengBattery.cpp和UkeLastLog.c，最终PAC及直接thread_call
检查通过。记录在docs/releases/2026-11-11-battery-dispatch.json。2026-11-11 用户本轮授权后，四分区写入、A 槽回读和重启命令成功，回执 `uke-port/releases/2026-11-11-battery-dispatch/flash-result-20261111-010412.json`。电池服务发布、读数和充电图标待用户反馈及同次日志。

## 2026-11-11 E0FE panic与上游差异复核

E0FE实机panic，原包已标DO_NOT_FLASH。照片按同版最终kernel及LR-4
检查，异常指令在memcpy/memmove+0x4c，调用链为IOMemoryDescriptor::writeBytes、
copypv、bcopy_phys_internal。缺少故障目标物理地址和更上层调用帧。
272条新尾环到183.299045秒显示，ADSP在DTB PAS36 INIT_IMAGE失败，
scm=2/1。七次独立投递采样state/rx/signal/returned/events/drain全0，
电池callout没有实际执行。Data仅1页变化，最新持久化UUID仍为0847，
旧统一日志不能用于解释本次panic。

已重新fetch并更新参考sheng到0215f34a6d5307dab53e9a81dff9ae0fd047f3be，
ShengBattery.cpp/AdspProbe.cpp/ShengBattmgr.c相对6bc0ce1e无变化。
电池投递恢复sheng的IOInterruptEventSource创建、addEventSource和
interruptOccurred路径，删除本地thread_call/runAction替代路径，保留
独立计数。具体软件事件源调度阶段仍待实机计数。

认证元数据核对到uke OEM5712080a的mdt_loader.c，其要求独立4KiB、
non-cacheable coherent页，失败时先PAS shutdown确认，再free。通用sheng
在这部分使用缓存的连续缓冲并在错误后直接释放，本地也继承了这个风险。
本轮按目标OEM契约分配向上取整4KiB的normal-uncached连续元数据，
INIT_IMAGE前标记安全世界可能持有，即使调用失败也先确认shutdown，
拒绝时隔离元数据和电源引用；成功启动后才释放临时元数据。PAS运行期
所有权独立保存，GLINK后续失败仍能停固件。增加PA/跨度及回收日志。

实际release/stop/unwind生产函数的27项ASan/UBSan测试通过，覆盖INIT失败、
shutdown拒绝、双PAS次序、认证前失败、成功后释放及GLINK失败；旧release
函数在同一测试中提前释放，按预期失败。205电池/10通道/176刷新/63事件
回调/RSCv2-v3各38项仍通过。

这项元数据生命周期缺陷已由目标源码和主机负对照证实。受保护页重用与
照片中的拷贝panic相符，但缺少目标PA，完整硬件因果尚未证明。只读取证
和原始照片在uke-port/fixes/2026-11-11-battery-dispatch-panic，根报告
docs/status/adsp-metadata-panic-2026-11-11.md。设备为原厂四分区/A/TWRP。

## SCM WAITQ：同次返回值的直接协议差异

进一步对照 uke OEM `drivers/firmware/qcom_scm-smc.c` 及 `qcom_scm.c`：
本次 ADSP x0=2 是 WAITQ_SLEEP，WPSS x0=3 是 WAITQ_WAKE，x1=1 是等待队列
上下文，均不是认证终结失败。本地与通用sheng旧胶水仅处理 interrupted1 和
busy-12，漏掉 WAITQ，且两个 PAS 路径没有共用普通调用锁。它们会在安全世界
尚未结束调用时进入加载失败及清理路径，这是已证实的协议缺陷。

原厂恢复系统实际 `/proc/interrupts` 和 dmesg 报告 contexts=2、GIC intid962、
Edge qcom-scm，已保存 recovery-scm-waitq.txt。新共享 Sm7675Scm.cpp 按目标
OEM 的普通调用 mutex、QCOM_A6、IRQ worker 原子 GET_WQ_CTX、标准
RESUME/ACK 和 EBUSY 后恢复原始请求实现，保留原版权及 GPL-2.0-only。
ADSP/WPSS 均使用该入口；WPSS 同样补齐4KiB非缓存元数据及shutdown-before-free。
XNU等待有10秒上限，超时或协议异常后隔离后续调用，保留安全世界可能持有的
元数据和电源，直到干净重启。这里与OEM无限等待的差异已明确记录。

真实共享SCM代码268项ASan/UBSan检查通过，包括睡眠→IRQ→GET→RESUME、
WAKE→ACK、连续WAITQ、wake_one/wake_all、A6保持、ACK返回busy后重发原请求、
超时隔离、畸形回复与双调用者串行。主机通过不能证明实机IRQ及PAS已经成功。

## 新候选与验证状态

新候选 `3EAE3F4E0AC9DE5B` 已正式完成kernel、payload、boot及vendor_boot构建，
vendor_boot SHA-256 `af011efef7268bdc54d9c11ec568530c15ec9226f198d5223ced53c51c76f9d1`，47,415,296B，小于96MiB分区上限。
归档 `uke-port/releases/2026-11-11-adsp-scm-waitq/`，源码快照core
`cc87305c2030e495e6131ad25b449d734fcb3804`、device `a94da50a5ce23df0d7949f260d4ab47e4361ecb6`、common `e5c8d35c32381613e72d37b5dfdc3d65c923b39e`，
运行时代码提交device `eb29ddd`、common `e5c8d35`。三个仓均保留同名新分支。

75项显示/平台源码与14项存储源码及实际归档SHA核验通过；最终arm64e
代码中ADSP/WPSS两处factory均为0x413，电池事件源PAC调用和共享SCM链接
已核对。721B参数、18项ADT、缺失2/桩24及Data/addon/trust签名资产与E0FE
对照一致。主机SCM268、元数据27及原有检查、core11、结构化13区通过。

候选未刷、未做硬件验收，设备留在原厂四分区/A/TWRP。下一步须当次
授权部署，确认SCM962中断与RESUME/ACK的终结响应、ADSP/WPSS共同启动、
无panic和电池投递/发布，再检查电量/充电图标及Wi-Fi、显示、背光和调频。
PCM、四路采集、HAL和自动DSP恢复仍未完成，不登记新硬件里程碑。
