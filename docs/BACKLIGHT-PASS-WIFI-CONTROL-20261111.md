# 熄亮动画通过、亮屏延迟与 Wi-Fi 控制分发

2026-11-11，用户确认 vendor_boot `a027ac1c…` 的熄屏、亮屏动画正常且不再花屏，亮屏有可感知延迟。Wi-Fi 表现为列表为空或后续扫描不更新。已按既定取证流程回到原厂四分区、A 槽、TWRP。32 MiB 尾环有 397 个新增 CRC 有效槽，SHA-256 `d638ee82721e610d48e9c3183a98e5acb3e9b50c7617aea2f6c9582f5d6d1ca1`。完整只读 Data 读取 12,310,069,248 B，同次 UUID `73CEAD6206E049718DE8A6919090C55B` 筛出 2,355,523 条统一事件。[尾环](../../reference/logs/ukelog/ukelog-20261111-080612-backlight-pass-wifi.bin) · [取证目录](../../uke-port/fixes/2026-11-11-backlight-pass-wifi-readback/)

## 亮屏延迟

五次可配对的唤醒中，按键到首个非零亮度请求为 1051–1094 ms，中位 1059.484 ms。首个请求到第二颗背光 BL_EN 启用回读为 0.865–1.833 ms。这些是软件日志间隔。

统一日志的墙钟换算与内核秒数不同，用 41 个按键/亮度事件校准对应关系，保留配对最大残差约 28 ms，过滤了日志交错造成的错误前缀。校准后示例为按键 136.179 秒，IOMFB 开屏约 136.202 秒，SpringBoard 发出 `CADisplayStateControl transitionToDisplayState=on` 约 137.209 秒，系统完成显示状态转换约 137.237 秒，再设亮度因子 1。驱动 137.238505 秒接受非零请求，137.239370 秒完成双片启用。延迟主要位于系统完成显示状态转换前，仍需定位约 1 秒等待的具体原因。此次候选只修 Wi-Fi 分发，动画路径没有叠加新的亮度时序修改。[时间线](../../uke-port/fixes/2026-11-11-backlight-pass-wifi-readback/wake-timeline.json) · [核对摘要](../../uke-port/fixes/2026-11-11-backlight-pass-wifi-readback/evidence-summary.json)

## Wi-Fi 扫描停滞

本次清钥匙初始化已能在关闭接口时快速完成。设置扫描排队后，51.050 秒发出扫描事件，但后续控制请求在无线工作线程的嵌套等待中无法执行，约 3 秒后 supplicant 断开。下一轮约 20 秒后才完成监管域工作并处理接口关闭。

本地 `nw_control.c` 在先前关闭接口回归修复中，将全部控制任务放进 `LX_WQ_TOPLEVEL`。Linux 监管域/11d 工作会等待完成，等待通过 `lx_pump` 推进其他任务。整体限制使扫描请求无法利用这些嵌套轮次，触发 RPC 超时与重复重连。上游的普通控制任务允许在互斥锁门禁条件下继续执行，但本地还要保留关闭接口时不取消祖先工作的约束。

修正为普通请求继续进入既有分发，owner close 单独使用最外层队列。每轮处理一个 RX 项目，下一项重新选择执行角色。若 TX-only 工作排队后收到 socket close，也先转到关闭角色再出队。既有单一所有者、会话 generation、请求擦除和有界队列继续生效。

一个生产控制端点主机检查覆盖嵌套等待中请求可推进、关闭时不能取消祖先任务，以及 TX 排队后出现关闭的角色切换。旧分发在同一用例的请求推进断言中止，新代码通过 ASan/UBSan 和原控制端点检查。实机扫描、关联、流量和第二次启动还待验证。[主机结果](../../uke-port/releases/2026-11-11-wifi-control-progress/control-host.log) · [旧代码负对照](../../uke-port/releases/2026-11-11-wifi-control-progress/control-negative.json)

## 候选

候选内核 `879437D238BB23B0`，vendor_boot SHA-256 `048ec300e7e0797ca069c8b50494eb2cc470db102353046fbf453f251297b9fd`，47,525,888 B。源码 core `d64cc269cdfd89b9b83098f69f7a45663b0268c3`、设备 `bdc9a9def4f21c8a4a438b46860882ab2933eb49`、Ath12kWifi `c93d26a75c368ad2c4c85bd4f482aa3807ae2d84`。同名功能分支，未推送。

正式 kernel、payload、bootimg、vendor_boot 通过，105/105 ath11k 对象，83 平台与 14 存储源哈希核验。平台源中仅 `nw_control.c` 与 a027ac1c 基线不同。最终内核的普通控制队列 flags=0，关闭队列 flags=`LX_WQ_TOPLEVEL`。签名运行包与已通过动画的 a027ac1c 逐字节相同，ADT 除随机种子一致，启动参数 721 B，2 missing / 19 stubbed 未增加。候选未刷，设备留 TWRP。[构建记录](../../uke-port/releases/2026-11-11-wifi-control-progress/build-result.json)

下一次先验 Wi-Fi 首次扫描，再改变附近热点并重复刷新，连接后验证流量和列表更新。回归熄屏、亮屏和滑块调光。亮屏约 1 秒的系统状态转换等待另查。


## 2026-11-11 Wi-Fi 控制分发修复候选授权部署

用户授权刷入 `879437D238BB23B0`。同包 boot 和 vendor_boot 已写入四分区，A 槽读回，重启命令成功。68 项归档 SHA 与 13 区检查通过。回执 `uke-port/releases/2026-11-11-wifi-control-progress/flash-result-20261111-081548.json`。Wi-Fi 首次扫描、列表反复刷新、连接中的扫描与亮度对照待实机验证。


## 8794 同次实机回读

用户确认熄屏、亮屏及 Wi-Fi 恢复，另报开机约四分钟时按电源键亮屏后锁屏卡死。内核尾环去重得 336 个新槽，统一日志 UUID `89DB480B-181F-4CDB-87DC-909E0DD25E4C`。六次 WMI 扫描成功，关联返回 0，有 DHCP ACK 和 en0 双向 TCP。约 198.868 秒 SpringBoard 报 0.35 秒未响应，随后亮屏转换完成，双背光芯片启用。Data 合成命令最后完成于 211.648 秒；2,563 个 phase 3 运行时入口有配对完成。约 220 秒 STExtractionService 的 PAC_EXCEPTION 缺少与卡死的因果证据。固定 GPU 槽与同次围栏序号、合成记录不配对，已排除。原厂四分区、A 槽和 TWRP 留作取证状态。主机未写擦 userdata。[详细回读](../../docs/status/wake-stall-readback-2026-11-11.md)
