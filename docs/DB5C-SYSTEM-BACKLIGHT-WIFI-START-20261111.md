# DB5C 熄屏亮度与 Wi-Fi 启动回读（2026-11-11）

用户确认 `DB5C89823294AB4A` 的 Wi-Fi 仍扫不到，首次熄屏前调光正常，亮屏后滑块降到最低且无法调节。按既定取证流程刷回原厂 boot/vendor_boot 四分区、A 槽并进入 TWRP 3.7.1_14-0。32 MiB 尾环有 245 个新 CRC 有效槽，覆盖 20.007226–143.374116 秒。SHA-256 `61f395365c3ef0526f4f0577161ceb496aeec9afb468b27c57d055ef11ffdaee`。只读 Data 快照传输 12,310,069,248 B，统一日志按启动 UUID `CF83D268-F9CD-444C-954F-3D7A738F0CC7` 筛出 2,291,464 条。[尾环](../../reference/logs/ukelog/ukelog-20261111-070229-db5c-lock-wifi.bin) · [同次内核日志](../../reference/logs/boot/boot-20261111-070229-db5c-lock-wifi.log) · [回读目录](../../uke-port/fixes/2026-11-11-db5c-lock-wifi-readback/)

亮度请求在首次熄屏前能到驱动，约 120–125 秒可见目标随滑块变化，双片写入和读回成功。125.761 秒板级电源桥接关闭 BL_EN，保留 1575。第 178 个亮度请求对应系统提交 0 nits。第一次亮屏后，板级代码等到画面呈现后于 127.122 秒重新打开 BL_EN，仍用 1575。四次亮屏过程中驱动亮度请求计数保持 178，背光错误和超时计数都是 0。统一日志显示 backboardd 收到后续拖动和提交，系统也把亮度因子设回 1，但其亮度变化通知又回到 0，未见新的非零亮度提交到背光。驱动层的保存值没有被清零。IOMFB selector 12 的原调用仍报告 Unsupported。是否由显示状态或亮度事务未恢复引起，现有日志无法确定。[系统事件](../../uke-port/fixes/2026-11-11-db5c-lock-wifi-readback/brightness-wifi-events.json)

按 [UniXNU 亮度接口](https://unixnu.com/zh/porting/display#_5-亮度接口) 核对 24A446 的 `AppleARMBacklight` 服务身份、`backlight-control`、等级与毫尼特范围、marketing table、Lmin/Lmid/Lmax、校准字典、`brightness-nits` 与旧 `brightness`/`FadePeriod` 请求入口，均在本地实现。新候选按 sheng 的请求路径让 CoreBrightness 的亮度命令直接进入背光工作循环。删去 selector 12 到背光的本地桥接、GOP 帧回调和额外的唤醒亮度恢复。0 nits 关双片输出，后续非零亮度请求打开输出并应用目标。系统没有发出非零亮度请求时，面板可能在第一次唤醒后保持黑暗，需实机检验。

Wi-Fi 旧版关闭接口时的 `cancel_work_sync` 断言这次未再出现，关闭后仍可收到 INFO 响应。31.136 秒的首个 SET_KEY 删除请求触发了接口启动，35.239 秒才回复，约 4.10 秒，超过 supplicant 的 3 秒 RPC 等待。后续会话反复重连，日志可见 5 次设置扫描排队与多次 267 号事件发出，无 2 号扫描请求或 WMI 扫描完成。改为在接口关闭时直接完成有效的清钥匙请求，已打开接口上的清钥匙仍经 cfg80211。发布扫描事件前先在无线工作线程准备接口，保留会话和 generation。既有连接后的实际扫描与 beacon 缓存路径保留。修改已通过生产 cfg80211 映射主机回归。

新候选 `A6F640C2738F39FE`，vendor_boot SHA-256 `ec4a33f684698cb0dd27d1c190160e8d25ce3527814a78cdb258bad02e4c6452`。设备 `2db4656529384c562117794f85e955367609faef`，Ath12kWifi `2ddd4639f56958b13d3af9edf494f548b4beee84`，GopFramebuffer `eadd64adce942f0521a5f6f1985959d902092e4a`。正式 kernel、payload、bootimg 和 vendor_boot 验证通过。最终内核确认桥接符号已去除，扫描准备调用已链接。背光生产方法主机检查 132,695 项通过。签名资产比对通过，启动参数 721 B。未刷，设备留在 TWRP。[构建记录](../../uke-port/releases/2026-11-11-system-backlight-wifi-start/build-result.json)

实机先检查中档调光和一次熄屏亮屏。若面板仍亮，检查滑块位置和非零亮度请求。Wi-Fi 先验证首次扫描确实取得网络，再用附近热点开关验证重复刷新。


## 2026-11-11 系统控制背光与 Wi-Fi 启动修正版授权部署

用户授权刷入 `A6F640C2738F39FE`。同包 boot 和 vendor_boot 已写入四分区，A 槽读回，重启命令成功。75 项归档 SHA 与 13 区检查通过。回执 `uke-port/releases/2026-11-11-system-backlight-wifi-start/flash-result-20261111-071321.json`。Wi-Fi 首次扫描、列表反复刷新、连接中的扫描与亮度对照待实机验证。


## A6F6 实机反馈与 BacklightMode 核对

用户确认 `A6F640C2738F39FE` 的 Wi-Fi 已恢复，熄屏后背光仍不能恢复。用户反馈未细分连接后的重复扫描和长时流量。

[虚拟平台章节](https://unixnu.com/zh/porting/display#ipados-27-上的虚拟平台) 的 BacklightMode 已接入。Uke 通过亮度运行时覆盖给 backboardd 加载 `UniXNUBacklight.dylib`，将 `initBLControl:` 的虚拟模式 3 映射到真机模式 1。实包启动参数为 `uke-backlight-runtime=1`，从 FAT 提取的 addon 与 DB5C 的 SHA-256 相同，均为 `42aed4228abfaa609d4ce46dabd3c58534996f4cbd10c30c0d325eb33644bdb8`。DB5C 同次 backboardd 日志有 `physical brightness control selected`，首次熄屏前调光也已确认。A6F6 的同次运行日志待回收，唤醒后的亮度事务仍待定位。


## A6F6 熄屏与唤醒日志

用户观察到熄屏先闪过花屏，然后直接变黑，按键亮屏时背光未恢复。按既定流程回到原厂四分区、A 槽和 TWRP，主机只读提取日志。32 MiB 尾环有 178 个新 CRC 有效槽，结束于开机 110.635335 秒，SHA-256 `9f218f3eb9c8357e328aff5849e2dce474a54d697c8a4f3b23ec58ab003830a6`。完整 Data 快照读取 12,310,069,248 B，统一日志筛出同次启动 `721D5DEB-908E-458B-A574-1EC291BAFF39` 的 2,252,402 条。其记录覆盖首次关屏，包含尾环结束后的事件。[尾环](../../reference/logs/ukelog/ukelog-20261111-072036-a6f6-wake.bin) · [内核日志](../../reference/logs/boot/boot-20261111-072036-a6f6-wake.log) · [系统事件](../../uke-port/fixes/2026-11-11-a6f6-wake-readback/display-events.json)

同次 backboardd 记录 `physical brightness control selected`，主屏选择同步亮度事务。关屏前双片写入和读回 1366，`BL_EN=0x7f`，错误及超时为 0。约开机 170 秒，日志捕获电源键按下和松开，SpringBoard 进入锁屏，backboardd 请求 0.185 秒的渐暗。GOP 随后耗时 323.903 ms 填充并锁存第一张黑帧，随后又耗时 342.095 ms 填充当前已选中的 GOP 缓冲区（`flip=0`）。之后同次亮度事务从 333.615 nits 跳到 0，驱动只收到目标 0，期间未见中间亮度请求。说明黑帧先于背光归零提交，符合用户看到的直接变黑。黑帧输出的实际像素及花屏来源仍待定位。

取回的日志只覆盖到首次锁屏后不久。没有记录到第二次按键、唤醒开始、恢复帧、非零亮度请求或本次关屏 BL_EN 的最终回读。空的 pstore 也无法定位截断原因。上一版 DB5C 已捕获亮屏因子恢复到 1 但 CoreBrightness 未下发非零亮度的情况。A6F6 本次证据只确认到锁屏，不能把 DB5C 的后续记录当作本次唤醒结果。

Wi-Fi 同次确认 session 1 持续处理扫描，多次 WMI 扫描完成，锁屏前最后一轮 wifid 报 35 个网络。关联后的重复扫描和长期流量独立验收。后续取证需要覆盖第二次按键，核对 IOMFB 屏幕状态、首张非黑帧、亮度事务和 GENI 回读。此次未改驱动或构建候选。


## 2026-11-11 A6F6 复刷取证

用户授权刷入 `A6F640C2738F39FE`。同包 boot 和 vendor_boot 已写入四分区，A 槽读回，重启命令成功。75 项归档 SHA 与 13 区检查通过。回执 `uke-port/releases/2026-11-11-system-backlight-wifi-start/flash-result-20261111-072759.json`。本次取证覆盖首次熄屏和第二次按键。
