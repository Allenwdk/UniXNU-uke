# A6F6 多次电源键回读与 CoreBrightness 事务候选

2026-11-11，用户按要求复刷 A6F6，开机首次电源键前拖动亮度条，面板确实随之变化。首次按键可熄屏，之后所有电源键均未点亮面板。按授权恢复流程回到原厂 boot/vendor_boot 四分区、A 槽、TWRP 3.7.1_14-0。主机只读回收尾环和 Data。378 个新增 CRC 有效槽覆盖至开机 208.821644 秒，尾环 SHA-256 `84ae87815b0bf7c4b71f35bf2a5032664c6ea930bd67b88828e2e6858d796109`。Data 续读后完整复核 12,310,069,248 B，同次启动 UUID `B49D8E51-9012-4678-9F83-5ACF7CC9DA48`，筛出 2,258,806 条统一事件。系统日志导出的墙钟与内核秒数缩放不同，按事件顺序、文字和 boot UUID 配对。[尾环](../../reference/logs/ukelog/ukelog-20261111-073339-a6f6-second-button.bin) · [内核事件](../../uke-port/fixes/2026-11-11-a6f6-second-button/kernel-display-events.json) · [系统亮度事务](../../uke-port/fixes/2026-11-11-a6f6-second-button/brightness-transactions.json)

## 已确认的停点

| 时刻（开机秒） | 同次事件 |
|---|---|
| 115–119 | 滑块多次改变目标，双片写入并读回，最后保存亮度码 1635 |
| 123.173649 | 第一次电源键按下，随后松开 |
| 123.903285 | 黑帧锁存，填充约 296 ms |
| 124.192569 | 第二次黑帧填充完成，DPU 已在扫描 GOP 缓冲区 |
| 124.208497 | 第 157 个背光请求，目标 0 |
| 124.209363 / 124.209410 | 双片 BL_EN 都为 0x3f，硬件亮度码保留 1635 |
| 127.061721 / 127.105282 | 第二次按键送达，44 ms 后 IOMFB 收到 powerstate 1 |
| 139.977461、171.770486 | 后续两次 IOMFB 开屏请求 |
| 205.728813 | 最后组合按键期间仍有开屏请求，作为恢复操作单独记录 |

BacklightMode 本次已加载，系统报告内置屏 `UseSyncBrightnessTransactionForDisplay: 1`。首次锁屏时 backboardd 要求 185 ms 渐暗，亮度事务从约 399.328 nits 跳到 0。系统三次唤醒都把 `DisplayBrightnessFactorWithFade` 设回 1，但同次 swap brightness 反馈仍是 0，后续事务继续发送 0，驱动再未收到非零请求。双片关断过程错误、超时均为 0。电源键和开屏请求已到达，接下来要验证同步亮度事务是否与当前显示实现不匹配。

本次 Wi-Fi 控制会话反复重连，尾环没有扫描完成。此前 A6F6 首次启动的真实扫描与用户反馈保留。本次重新启动没有重复得到该结果，因此 Wi-Fi 冷启动可靠性恢复为待查，不能将此前一次成功推广到每次启动。

## 原版 24A446 路径

本机原版 CoreBrightness `-[BLControl useSyncBrightnessTransactionForDisplay:]` 位于 `0x22a7eb098`。参数是显示对象，调用它的 displayType 确定内置屏类型，再调用 `_CBU_IsSyncBrightnessTransactionsSupported`。后者位于 `0x22a793f88`，查询的原版 MobileGestalt key 是 `DeviceSupportsFrameSynchronousBrightness`。`_SetIOMFBBrightness` 的同步分支发出 SyncDBV 通知，普通分支继续原系统的亮度写入路径。反汇编及 CFString 取值保存在本轮取证目录。

已核对 core 远端 main `3fd6a3c` 和 sheng `aa1c4e2`，上游 BacklightMode 只选择物理控制模式，未覆盖这个同步事务选择。保留该上游模块，Uke 增加独立的 `UkeBrightnessTransactions.m`，在 backboardd 中替换同步事务查询。初版误把参数当成 NSNumber，该判断已由下文的显示对象修正替代。启动时检查 selector 的参数数量、对象参数及 BOOL 返回类型，失败会打印 inactive。构建入口增加显式 `--direct-brightness` 参数。

该路径差异符合本次反馈停点，仍是待实机验证的修复假设。首次唤醒是否出现非零目标、是否恢复调光和系统渐变，要由本次候选的同次日志验证。花屏来源和黑帧填充的显示时序仍未结案。[官方亮度接口](https://unixnu.com/zh/porting/display#_5-亮度接口) · [本地适配](../../UniXNU-uke/package/UkeBrightnessTransactions.m)

## 候选身份与验证

本轮只改变用户态亮度适配库。内核仍为 `A6F640C2738F39FE`，用 vendor_boot SHA-256 区分这个新候选，完整值为 `72d34a39cd579a62de9a11bb60dd92eb15ea4ef285fb6afa3fa8d37097e9d66c`，文件大小 47,525,888 B。镜像路径 `uke-port/releases/2026-11-11-backlight-direct-transactions/`。设备源码 `23949af299763e7f48c4ee17fb85bcfd9154d1f5`，所有仓仍在 `feat/uke-ipados27-dsp-audio-power`，未推送。

正式 kernel、payload、bootimg、vendor_boot 通过。实际 FAT 的 addon 只有 `backlight/lib/UniXNUBacklight.dylib` 与 A6F6 不同，backboardd、supplicant、图形运行时逐文件相同。信任缓存只替换原亮度库的哈希，backboardd 的 category5 及其他条目保持原值。arm64e、4 KiB 代码签名与签名页摘要通过，83 个平台输入、14 个存储输入相同，启动参数 721 B，2 missing / 19 stubbed 未增加。

候选未刷。设备留在 TWRP。下一次验收先调到中档，第一次熄屏后按键唤醒，检查渐暗、渐亮、滑块位置及后续调光。至少保留第二次按键后 30 秒的日志，再回收。期望看到 direct transactions 的加载记录、唤醒后的非零背光目标及双片 enable 回到 0x7f。[候选构建](../../uke-port/releases/2026-11-11-backlight-direct-transactions/build-result.json)


## 2026-11-11 CoreBrightness 普通亮度事务候选授权部署

用户授权刷入 `A6F640C2738F39FE`。同包 boot 和 vendor_boot 已写入四分区，A 槽读回，重启命令成功。77 项归档 SHA 与 13 区检查通过。回执 `uke-port/releases/2026-11-11-backlight-direct-transactions/flash-result-20261111-074925.json`。Wi-Fi 首次扫描、列表反复刷新、连接中的扫描与亮度对照待实机验证。


## 72d34a39 实机失败与显示对象参数修正

用户确认 vendor_boot `72d34a39…` 仍不能在熄屏后恢复亮度。本次 382 个新增 CRC 有效尾槽，尾环 SHA-256 `92aea15256a7a91cb893fd1ba74807d824ab6fba4b4f5815559712fabb7bf6f9`，只读 Data 完整读取 12,310,069,248 B。按启动 UUID `039B275B287E442084529F0A1F654B36` 筛出 2,231,770 条统一事件。设备已恢复原厂四分区、A 槽、TWRP。

同次日志既有旧库的 `CoreBrightness direct transactions selected for integrated display`，也有主屏 `UseSyncBrightnessTransactionForDisplay: 1`。前者只是安装替换方法后的启动打印，未证明真正选中了普通事务。原版 24A446 的调用桩 `0x2300219c0` 向参数对象发送 `displayType`，`0x230021920` 发送 `displayId`。因此参数是显示对象。上个候选把它当成 NSNumber，并以数值 0 作判断，导致分支从未生效。前文关于参数类型和编号的判读已修正。[同次证据](../../uke-port/fixes/2026-11-11-direct-brightness-readback/evidence-summary.json) · [调用桩](../../uke-port/fixes/2026-11-11-direct-brightness-readback/objc-stubs.asm) · [selector 字符串](../../uke-port/fixes/2026-11-11-direct-brightness-readback/objc-selectors.txt)

修正后调用原方法识别显示对象，将它选中的同步事务改为普通事务。实际查询时增加 `selected direct brightness for ...` 日志，与启动时的 `override installed` 区别。原物理亮度模式适配继续使用上游 BacklightMode。该修改只解决此前候选没有切换路径的问题，唤醒亮度和花屏的修复效果仍待实机。

新候选 vendor_boot SHA-256 为 `a027ac1c754b97f52959bca8c942c473cae7ebf34f8713d6fad46d01c9ce1b45`，内核仍是 `A6F640C2738F39FE`。归档 `uke-port/releases/2026-11-11-backlight-display-object/`，设备源码 `7f053759750b2211c3f81f4b982a01c243fad035`。正式 kernel、payload、bootimg、vendor_boot 和刷前检查通过。实包只更换亮度库及对应的一项信任哈希，83 个平台和 14 个存储输入与基线相同。候选未刷。下一次核对实际 query 日志、开屏后的非零亮度请求、双片启用回读及滑块调光。[构建记录](../../uke-port/releases/2026-11-11-backlight-display-object/build-result.json)


## 2026-11-11 显示对象参数修正候选授权部署

用户授权刷入 `A6F640C2738F39FE`。同包 boot 和 vendor_boot 已写入四分区，A 槽读回，重启命令成功。76 项归档 SHA 与 13 区检查通过。回执 `uke-port/releases/2026-11-11-backlight-display-object/flash-result-20261111-080023.json`。Wi-Fi 首次扫描、列表反复刷新、连接中的扫描与亮度对照待实机验证。


## a027ac1c 动画与花屏实机反馈

2026-11-11，用户确认该版熄屏、亮屏动画“非常完美”，且不再花屏。vendor_boot SHA-256 `a027ac1c754b97f52959bca8c942c473cae7ebf34f8713d6fad46d01c9ce1b45`，内核 A6F640C2738F39FE。亮屏仍有可感知延迟，Wi-Fi 仍有问题，继续回读同次日志。此条验收覆盖用户本次观察到的动画与无花屏表现。
