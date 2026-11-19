# Uke 实机状态（2026-11-15）

用户实机反馈：“目前亮度和wifi都修好了，gpu和cpu也不会卡死了”。

| 子系统 | 本次确认 |
|---|---|
| 显示与背光 | 亮度调节正常 |
| Wi-Fi | 当前使用正常 |
| GPU | 当前使用未再出现此前的 GPU 卡死 |
| CPU | 当前使用未再出现此前的 CPU 卡死 |

最近一次刷写回执为 2026-11-15 14:23:55 +08:00，`vendor_boot` SHA-256 为 `1afe76bba460de52ab603377de236cf9f491e38d621270fe9c66241268fdb035`。回执记录四个启动分区写入成功、A 槽以及重启命令获接受。设备功能结论来自本次用户反馈，反馈未单独提供镜像哈希、运行时长或测试场景清单。该次部署及电池专项验证范围见[电池超时与恢复记录](BATTERY-REFRESH-20261114.md)。

较早的 [TRACE 同次日志](CPU-TRACE-READBACK-20261115.md)记录 184–2408 秒八核 IRQ、定时器和 IPI 计数持续增长。旧 CPU 停滞、GPU 冻结、背光和 Wi-Fi 失败记录继续对应各自镜像。长期压力、冷启动与唤醒覆盖、GPU 故障恢复、精确刷新率和电池长期刷新各有独立验收范围。

USB后续状态：全面诊断816a3e34仍未枚举，已恢复原厂四启动分区/A/TWRP只读回收。PM入队40/出队0，active修正候选609e7fb6已构建核验，未刷入；此前亮度/Wi-Fi验收不能替代恢复全局PM后的回归。详见USB2-20261115.md。

最新部署：USB PM active修正版 609e7fb696b1add1c8d108f6f1849cda1b8d0316bedf38d0f5c6bec1d933ec3a 已写入 boot/vendor_boot A/B 四分区，A槽回读确认，重启命令接受。系统启动和USB响应尚未由用户确认；回归亮度、Wi-Fi、电量及锁屏唤醒。刷前根清单只读核验通过，主机未写擦userdata。

609e实机后续：用户报告USB仍无反应。同次回读已确认PM队列恢复、USB电源4和上电/reset/事件DMA/EP0配置完成，IRQ/事件仍0。设备本轮恢复原厂四启动分区/A/TWRP只读取证；未写擦userdata。新session恢复候选ac456259已构建核验未刷；USB与共用PM回归仍待验证。

2026-11-15T22:50:41+08:00 用户当次授权刷入 `ac456259cd06e4045d705c3411d104b1490add7e71b4ac76f27c456d2aca56bb`。TWRP根清单O_DIRECT分区/LUN双读核验通过；boot_a/vendor_boot_a/boot_b/vendor_boot_b写入成功，A槽回读确认，重启命令接受。主机未写擦userdata；系统启动、USB及共享PM回归待用户反馈。回执：uke-port/releases/2026-11-15-usb2-session-restore/flash-result-20261115-225027.json。

USB2基本枚举实测：2026-11-15 22:51:36–37（Asia/Shanghai），主机内核在usb3-1枚举05ac:12ab、产品iPad、厂商Apple Inc.；22:54后sysfs直接读回speed480、bConfigurationValue1、bNumInterfaces1、authorized1。USB2设备枚举及选择配置已通过。未验证bulk传输、Trust/配对或lockdown；本机无usbmuxd服务/进程/socket，idevice_id报无法获取设备列表，不能把此结果归因设备端。序列号描述符为40个f，后续身份/配对仍需核验。没有额外设备写入/重启。vendor_boot `ac456259cd06e4045d705c3411d104b1490add7e71b4ac76f27c456d2aca56bb`；主机证据`uke-port/fixes/2026-11-15-usb2-session-host-enumeration`。

在线连接实测：ac456259维持USB枚举。已本地构建运行官方usbmuxd3ded00c，当前用户独立socket、无系统安装/自动配对；Mux20B v2.0版本回应和列表通过。lockdown62078及SSH22的36B SYN USB OUT完成status0，但无后续IN响应，info -s报Mux -8，日志不能启动；未pair/刷写/重启。入口uke-port/uke-usb；测试证据uke-port/fixes/2026-11-15-usb2-live-debug/，工作区说明docs/status/uke-usb-live-debug-2026-11-15.md。

2026-11-16完整设备回读：授权恢复原厂boot/vendor_boot A/B、A槽/TWRP，30秒后只读环949新CRC有效seq0–948（1099旧槽相同，0无效），raw205022fec472d3e01ebd8f41f20b3b151eb8e16afff1ed6879ae4c364829a00a；Data12,310,069,248B/537671变化页完整。统一日志UUID3437B877-684D-4F3B-9150-8F67DE37E077共4381078条，61唯一内核字符串对齐；4IPS不含lockdownd。实测BCR清HS bit20、候选恢复；地址5/配置1/Mux激活，PM58/58、irq147、faultbits0。lockdown62078已监听，主机SYN与设备IRQ回调间隔吻合但无连接回复，尚未唯一定位Bulk/Mux/TCP段，未构建新候选。主机未写擦userdata，设备留原厂四分区/A/TWRP。独立证据uke-port/fixes/2026-11-16-usb2-mux-readback/，工作区报告docs/status/uke-usb2-mux-readback-2026-11-16.md。

2026-11-16官方/sheng/Uke对照及未刷候选：Uke板级漏传`sheng-aks-sls=3`，KeyStore缺省锁状态0；已加无密码状态3，并为AppleUSBDeviceMux增加DebugLevel=7有界日志候选。正式kernel/payload与镜像核验通过，vendor_boot SHA `10757869b9b142066ef948fa8f90f213a164346a06fa9e521c5bddd8693b01c2`，硬件未验。主机16:51:32最后见序列b8a81f12在bootloader fastboot，尚未刷。[审计](../../docs/status/uke-usb2-mux-debug-2026-11-16.md)。

阶段状态（2026-11-16）：USB2外设尚缺TCP/lockdown、Trust/配对及服务会话验收；OEM DTS含SuperSpeed QMP资源，但Uke USB3初始化被固定USB2 build排除；共享host代码受固定模式绕过角色线程；Uke UCSI/VBUS/CC角色切换未接入。先完成USB2验收，再分别实现USB3和host，最后整合OTG。[路线和门槛](../../docs/status/uke-usb2-mux-debug-2026-11-16.md#阶段清单)。

2026-11-16T14:43:06+08:00 用户当次授权刷入 `ac456259cd06e4045d705c3411d104b1490add7e71b4ac76f27c456d2aca56bb`。TWRP根清单O_DIRECT分区/LUN双读核验通过；boot_a/vendor_boot_a/boot_b/vendor_boot_b写入成功，A槽回读确认，重启命令接受。主机未写擦userdata；系统启动、USB及共享PM回归待用户反馈。回执：uke-port/releases/2026-11-15-usb2-session-restore/flash-result-20261116-144252.json。
