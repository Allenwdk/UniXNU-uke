# 1994 亮度与 Wi-Fi 回读（2026-11-11）

用户反馈 `19948EA4D7EDD158` GPU 使用无明显问题，控制中心滑块会移动但面板亮度不变，Wi-Fi 扫描失效；此前另有列表仅开机刷新一次的问题。按已授权恢复流程留在原厂 boot/vendor_boot 四分区、A 槽、TWRP 3.7.1_14-0。主机未写擦 userdata。

同次尾环 32 MiB，518 个新增 CRC 有效槽，排除 1,530 个旧槽，覆盖 19.958813–279.481019 秒。尾环 SHA-256 `86a49facab45f94d4240d225b92b44e78aff63e339811ff53ba3cf5017d8a5d6`。Data 只读捕获传输 12,310,069,248 B，按 boot UUID `A5BA2B55-0BE3-41DA-B7A5-977AF5D41865` 过滤统一日志；同次内核构建标识和 Wi-Fi 事件互证。统一日志解析器的墙钟显示与尾环时钟不同，本文的启动秒数使用尾环。

- **显示**：`vsync hz=120 start=1 refresh=1` 多次出现，说明参数入口已生效。用户反馈覆盖本次使用；长时重载、实际帧节奏与掉帧仍单列。
- **亮度**：`BacklightMode` 选中了物理模式，CoreBrightness 找到主屏和亮度范围。119.179 秒写入 1366，双片读回 1366；245–257 秒熄屏和恢复时双片依次回到 0/1366，错误和超时计数为 0。统一日志没有看到对应控制中心拖动的新 `DisplayBrightness` 请求。`CBBrightnessBroadcaster init failed` 与 `UseSyncBrightnessTransactionForDisplay: 1` 也见于旧背光回读，不能据此归因。当前保留 GENI、背光覆盖与 GPU 显示实现。24A446 的控制中心模块使用 `BKSDisplayBrightnessTransactionCreate` / `BKSDisplayBrightnessSet`；下一次用设置页和控制中心分别复测，定位请求停在 BKS 还是 CoreBrightness。
- **Wi-Fi 停滞**：第一次 INFO 往返超过 supplicant 的 3 秒等待。首会话断开后启动自检扫描取消，vdev 已删除；紧接着出现 `lx_rt.c:266`。归档 `e6abc94` 的对应行是 `lx_cancel_work_sync` 对正在执行的任务的检查。嵌套执行接口关闭使外层任务不能正常退出，后续 INFO 只入队，最后出现附着忙。原分发器的同一关闭顺序在主机负对照上中止；新的控制队列只在外层分发运行，保留原来的固件完成回调、接口关闭及会话所有权。INFO 和缓存读取不再启动 vdev，移除板级启动自检扫描；扫描由系统通过原 supplicant/cfg80211/mac80211 路径发起。
- **Wi-Fi 列表**：sheng `7e23f27` 的 `WifiNet.cpp` 也在连接后长期返回缓存。现在认证阶段延后扫描，连好后可执行真实扫描。设置页在 200/221 秒还发出了此前不支持的 390 号 beacon cache 查询。对照 24A446 原始 `IO80211` `_Apple80211GetBeaconCache`：长度 2280，年龄字段 0x84，IE 长度 0x8a，IE 从 0x8c 开始，逐条读取。实现独立游标、零填充扩展区、真实年龄和 30 秒过期过滤；缓存查询可每 15 秒合并触发一次真实刷新。关联状态、上游频道与固件驻留策略保留。上游 Ath12kWifi `5f7f5a8`，core `3fd6a3c` 已重新核对，无可直接复用的该问题修复。

候选 **`8F3473F97672F212`**，vendor_boot SHA-256 `04203d2afb8156f02a4cb762c8911cc2fd779959aac0c292a7c51eb9031ab328`（47,525,888 B）。同名功能分支 core `fee09cd17107cd8d471ed740f787806a5cb4b597`、设备 `508d24ddb9388d046dc87ff8aa66796b3fda868f`、Ath12kWifi `129cddcfe1595f368b118551dd66cb2781f32017`，未推送。正式 kernel、payload、bootimg、vendor_boot 通过，105/105 ath11k 对象，83 平台与 14 存储源哈希，FAT 内签名资产与 1994 完全一致，ADT 仅随机种子可变，启动参数 721 B，2 missing/19 stubbed 未增加。ASan/UBSan 控制队列、运行时、cfg80211 映射通过；额外的全栈主机夹具受现有 AHB/crypto 组合编译限制未通过，未计入验证。现有 GPU/电池功能、连接中的扫描、应用流量和亮度仍待实机回归。候选未刷，须当次授权。

下一次验收：首次进设置扫描；不重启，开关另一台设备的热点，间隔至少 15 秒重进 Wi-Fi 页面，检查新网络出现、旧网络过期；连接后再重复并保持连续流量。分别用设置→显示与亮度和控制中心调整低、中、高档，记录面板变化及大致时刻。

[尾环](../../reference/logs/ukelog/ukelog-20261111-055745-backlight-wifi-refresh.bin) · [同次启动日志](../../reference/logs/boot/boot-20261111-055745-backlight-wifi-refresh.log) · [取证清单](../../uke-port/fixes/2026-11-11-backlight-wifi-refresh/readback.json) · [镜像与构建验证](../../uke-port/releases/2026-11-11-backlight-wifi-refresh/build-result.json)
