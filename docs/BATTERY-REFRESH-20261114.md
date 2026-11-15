# 电池长期刷新停更定位

2026-11-14 本轮只读取证及源码定位，无驱动改动或电池修复构建。根工作区报告为 `docs/status/battery-refresh-2026-11-14.md`。

同 boot `77C55B96-1D22-414D-9259-2EFD6C120559`，2048 CRC 有效槽 seq280–2327。112 次采样在184.280–2408.712秒 ans976/to1 不变，rx1063→1653、ignored25→615，events/drain继续推进，无环满丢包/发送拒绝。一次超时后的本地 poisoned 永久隔离、取消周期刷新和忽略通知导致停更；首条超时和具体字段已覆盖。原始尾环SHA256 `6815c8ef0a63ec7c64b12342acf4677421e66e40c6de97e7bb0e8486de94c00b`，证据 `uke-port/fixes/2026-11-14-battery-refresh/`。

已对照 sheng1108068、Uke OEM及生产代码；205+176 ASan/UBSan检查通过，现有检查覆盖的是隔离行为，恢复尚未实现。设备原厂四启动分区/A/TWRP；未写擦userdata。

## 2026-11-15 修复候选

1afe76bba460de52ab603377de236cf9f491e38d621270fe9c66241268fdb035 已完成正式kernel/payload和配对核验，未刷。恢复sheng/OEM按字段超时推进、5秒重新采样/订阅；超时判断先消费队列并核对当前deadline。3456项ASan/UBSan及旧隔离负对照通过，22字段各45分钟为主机模拟。保留CPU quiet配置/八核/TRACE/AppSync/732B参数和69个运行库/trust。无事务ID仍不能严格区分同字段跨重试迟到应答；实际断链隔离保持。有效未提交源码留release/source及device-source.diff。归档uke-port/releases/2026-11-14-battery-recovery，证据同名fixes。设备留原厂四分区/A/TWRP，本轮无设备操作，硬件恢复/长期刷新待验。

## 2026-11-15 授权部署

2026-11-15T14:23:55+08:00 用户授权刷入 `1afe76bb…`。四个启动分区写入成功，A 槽回读确认，重启命令已接受。已只读复核 SHNGROOT。实机启动、插拔和长期刷新待验。 回执 `uke-port/releases/2026-11-14-battery-recovery/flash-result-20261115-142348.json`。
