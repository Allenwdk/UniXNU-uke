# DB5C 熄屏亮度与 Wi-Fi 启动回读（2026-11-11）

用户确认 `DB5C89823294AB4A` 的 Wi-Fi 仍扫不到，首次熄屏前调光正常，亮屏后滑块降到最低且无法调节。按既定取证流程刷回原厂 boot/vendor_boot 四分区、A 槽并进入 TWRP 3.7.1_14-0。32 MiB 尾环有 245 个新 CRC 有效槽，覆盖 20.007226–143.374116 秒。SHA-256 `61f395365c3ef0526f4f0577161ceb496aeec9afb468b27c57d055ef11ffdaee`。只读 Data 快照传输 12,310,069,248 B，统一日志按启动 UUID `CF83D268-F9CD-444C-954F-3D7A738F0CC7` 筛出 2,291,464 条。[尾环](../../reference/logs/ukelog/ukelog-20261111-070229-db5c-lock-wifi.bin) · [同次内核日志](../../reference/logs/boot/boot-20261111-070229-db5c-lock-wifi.log) · [回读目录](../../uke-port/fixes/2026-11-11-db5c-lock-wifi-readback/)

亮度请求在首次熄屏前能到驱动，约 120–125 秒可见目标随滑块变化，双片写入和读回成功。125.761 秒板级电源桥接关闭 BL_EN，保留 1575。第 178 个亮度请求对应系统提交 0 nits。第一次亮屏后，板级代码等到画面呈现后于 127.122 秒重新打开 BL_EN，仍用 1575。四次亮屏过程中驱动亮度请求计数保持 178，背光错误和超时计数都是 0。统一日志显示 backboardd 收到后续拖动和提交，系统也把亮度因子设回 1，但其亮度变化通知又回到 0，未见新的非零亮度提交到背光。驱动层的保存值没有被清零。IOMFB selector 12 的原调用仍报告 Unsupported。是否由显示状态或亮度事务未恢复引起，现有日志无法确定。[系统事件](../../uke-port/fixes/2026-11-11-db5c-lock-wifi-readback/brightness-wifi-events.json)

按 [UniXNU 亮度接口](https://unixnu.com/zh/porting/display#_5-亮度接口) 核对 24A446 的 `AppleARMBacklight` 服务身份、`backlight-control`、等级与毫尼特范围、marketing table、Lmin/Lmid/Lmax、校准字典、`brightness-nits` 与旧 `brightness`/`FadePeriod` 请求入口，均在本地实现。新候选按 sheng 的请求路径让 CoreBrightness 的亮度命令直接进入背光工作循环。删去 selector 12 到背光的本地桥接、GOP 帧回调和额外的唤醒亮度恢复。0 nits 关双片输出，后续非零亮度请求打开输出并应用目标。系统没有发出非零亮度请求时，面板可能在第一次唤醒后保持黑暗，需实机检验。

Wi-Fi 旧版关闭接口时的 `cancel_work_sync` 断言这次未再出现，关闭后仍可收到 INFO 响应。31.136 秒的首个 SET_KEY 删除请求触发了接口启动，35.239 秒才回复，约 4.10 秒，超过 supplicant 的 3 秒 RPC 等待。后续会话反复重连，日志可见 5 次设置扫描排队与多次 267 号事件发出，无 2 号扫描请求或 WMI 扫描完成。改为在接口关闭时直接完成有效的清钥匙请求，已打开接口上的清钥匙仍经 cfg80211。发布扫描事件前先在无线工作线程准备接口，保留会话和 generation。既有连接后的实际扫描与 beacon 缓存路径保留。修改已通过生产 cfg80211 映射主机回归。

新候选 `A6F640C2738F39FE`，vendor_boot SHA-256 `ec4a33f684698cb0dd27d1c190160e8d25ce3527814a78cdb258bad02e4c6452`。设备 `2db4656529384c562117794f85e955367609faef`，Ath12kWifi `2ddd4639f56958b13d3af9edf494f548b4beee84`，GopFramebuffer `eadd64adce942f0521a5f6f1985959d902092e4a`。正式 kernel、payload、bootimg 和 vendor_boot 验证通过。最终内核确认桥接符号已去除，扫描准备调用已链接。背光生产方法主机检查 132,695 项通过。签名资产比对通过，启动参数 721 B。未刷，设备留在 TWRP。[构建记录](../../uke-port/releases/2026-11-11-system-backlight-wifi-start/build-result.json)

实机先检查中档调光和一次熄屏亮屏。若面板仍亮，检查滑块位置和非零亮度请求。Wi-Fi 先验证首次扫描确实取得网络，再用附近热点开关验证重复刷新。


## 2026-11-11 系统控制背光与 Wi-Fi 启动修正版授权部署

用户授权刷入 `A6F640C2738F39FE`。同包 boot 和 vendor_boot 已写入四分区，A 槽读回，重启命令成功。75 项归档 SHA 与 13 区检查通过。回执 `uke-port/releases/2026-11-11-system-backlight-wifi-start/flash-result-20261111-071321.json`。Wi-Fi 首次扫描、列表反复刷新、连接中的扫描与亮度对照待实机验证。
