# uke iPadOS 27 DSP、音频与电源

本项按[官方流程](https://unixnu.com/zh/porting/power)分阶段实施，目标为
24A446 arm64e。第一阶段代码接通 ADSP 传输、电池只读服务和 AudioReach
APM 就绪查询，实机验收待进行。PCM 流、FS19xx 功放、WCD939x 采集和 HAL
服务接入仍在后续阶段，整项尚未完成。

## 输入与复用

- sheng 固定到 `6bc0ce1e8c3a3733110a80dd80ddfb2a1ca46555`，原始源码
  SHA-256 和固件来源在 `firmware/uke-adsp/manifest.json`。复用 MDT、
  SMEM、SMP2P、GLINK、QRTR 服务定位、GPR 和 AudioReach 编码。
- ADSP 的 Cliffs PAS 为1，独立 DTB PAS 为0x24。预留区域分别为
  0x9d400000/0x4500000 和 0xa1900000/0x80000。启动使用固件分区现有
  `adsp.bNN`、`adsp_dtb.bNN`，元数据经安全世界认证。版本及完整二进制
  哈希须实机补录，当前清单明确标为待回读。
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

当前主机没有原厂 FS19xx 预设和完整 CoreAudio HAL SDK 头文件。HAL
正式构建使用具有完整 Apple SDK 的 macOS 环境。每个端点均在自己的
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
