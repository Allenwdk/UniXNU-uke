# uke 电源/音量键与背光（2026-11-10）

状态：实现与主机检查完成，正式kernel/payload及双配置归档检查通过；没有本轮硬件验收。分支 `feat/uke-input-backlight-20261110`，没有 push、刷机或主机 userdata 写入。

## 复用和硬件依据

官方依据：[输入](https://unixnu.com/zh/porting/input)、[显示](https://unixnu.com/zh/porting/display)、[背光](https://unixnu.com/zh/drivers/sheng-backlight)。已 fetch core main `95ad78129203442c3c14c02336303adbddf8d0a3`、sheng main `a68ee4ee67cc71792fbf519de7e3e28072c717b5` 及相关组件。未合入清单见根文档 `docs/status/input-backlight-upstream-2026-11-10.md`。

驱动来自 sheng 最新 ButtonProbe、BacklightProbe、AppleARMBacklight、GeniI2c 和 GENI 测试，保留原服务类、函数符号、HID report 2 与 boot-arg 名称。BootResetCore 使用 uke 已有文件；未增加重启/PMIC reset 操作。实际构建使用设备 package 和正式 `build-native-legacy-display-27.py` 配方。

`uke-port/data/uke-live.dts`：

| 功能 | uke 接线 | 软件映射 |
|---|---|---|
| 电源键 | PMK8550 SID0/PON peripheral0x13 bit7 | report2 bit1 Consumer Power |
| 音量减 | 同一 PON bit6 | report2 bit3 Volume Decrement |
| 音量加 | PMXR2230 SID1/GPIO6，peripheral0x8d，低有效、上拉、32ms去抖 | report2 bit2 Volume Increment |
| SPMI 中断 | bus0/EE0，PDC pin1 -> SPI481/INTID513 | APID动态查找及owner验证 |
| 背光 A | KTZ8866 0x11，QUP0 SE0 0xa80000，GPIO52/53，SPI353/INTID385 | 11bit，0x04低3位/0x05高8位 |
| 背光 B | KTZ8866 0x11，QUP1 SE4 0x890000，GPIO176/177，SPI586/INTID618 | 同上 |

时钟/引脚编码来自 `reference/Xiaomi_Kernel_OpenSource-uke` 的 gcc-cliffs、pinctrl-cliffs 与 pinctrl-spmi-gpio。两组 GCC vote 分别为0x52008 bits18..22、0x52010 bits0..3+8；保留固件的 GENI 协议、时钟与 SCL 时序，协议不符时不发布可用背光。TLMM每GPIO真实基址0x0f100000，四引脚 FUNC1、2mA、SDA上拉/SCL无上拉。

## 按键与亮灭屏行为

SPMI ISR只清arbiter状态、屏蔽当前APID并唤醒工作线程；SPMI读写及peripheral latch应答均在工作线程。非本功能外设只按上游清arbiter状态，不读取其寄存器；只配置本EE拥有的PON和GPIO6。连续1000次非按键IRQ屏蔽PDC并记录stage=-9。采样失败保留该组旧状态并定时重试，GPIO6做32ms稳定去抖；HID晚注册会重放当前状态（包括开机时已按住）。移除TouchProbe旧的空按钮sink，触摸算法保持。

24A446 selector12原始表位于kernelcache文件偏移0x1360ce8+12*40；wrapper0xfffffe000a2806c4从IOExternalMethodArguments+0x20取scalarInput，读第一个uint32交request_power_change。只接受一个scalar、无structure、值0/1；检查证据是 fixes/power-abi.json 和 power-abi.asm。维持原GOP应答，不进入缺少面板平台后端的原厂电源迁移。

背光由工作循环串行控制：熄屏有效等级0，保存最近非零用户亮度；亮屏恢复。渐变/请求均异步且新请求覆盖未执行旧请求。启动先读两颗固件等级，不在启动回读尚未完成时写默认亮度；每次亮度写后回读两颗，失败不会被统计成已写成功；每个目标每颗最多3次失败尝试，后续不同目标可恢复。日志 `UKE BL`/`UKE BTN` 有界打印状态、请求、双片写入/回读/错误/timeout。

系统服务保留上游 iPadOS27 的 BrightnessMilliNits、marketing table、LmaxProduct、calibration参数和brightness-nits。2/140/500 nits只是未实测的滑块刻度，不能用它声称面板绝对亮度。

## 用户态与打包

保留ADT vmm-present。原样复用core `BacklightMode.m`，仅在backboardd对BrightnessSystemInternal initBLControl:检查ABI并将3改为1。构建仅包含此模块，Foundation重导出/依赖替换使用上游macho.py；不引入AppSync应用安装、签名修复或动态用户信任。

`build-backlight-runtime.py`核对原backboardd SHA256 `fbba7fd2925990dd859b24629ecab5cdbf6f2fac8c1c357ec0f9e412ded75559`，使用本机带libtapi的toolchain-gpu/ld、iPhoneOS17 SDK头编译arm64e/iOS27模块。保留原backboardd identifier与entitlements，用ldid签名，独立逐页验证CodeDirectory/特殊槽/CDHash。所有原section内容、地址和偏移保持，现有显示text-fixups仍可命中。license随addon分发。

addon只增加backlight/lib与backlight/libexec，原12项文件保持；信任bundle原5个module逐字节保留，新增两项CDHash的module。内核复用上游覆盖挂载，先库成功再覆盖backboardd，保留现有Wi-Fi工具下层。沿用原Data seed，不做主机userdata写入/清理。

启用 `sheng-buttons=1 sheng-backlight=1`；删除serial=3/debug=334/-uketextconsole三项诊断参数，保留-ukedeferredconsole及持久日志，正式payload参数984B。

## 已完成检查及验收边界

`check-input-backlight.py`在ASan/UBSan下直接编译生产buttonsIrq及背光kick/noteFailure/finished，配合上游GENI硬件模型和实际状态策略，共5082项检查通过；覆盖8种组合、释放、读取失败、32ms反弹、owner拒绝、无关/越界IRQ、1000次storm、亮度边界、双片写入回读、单片NACK、timeout/cancel/abort及后续恢复、20次亮灭屏状态循环。它是主机模拟，不是硬件证明。

运行时原section不变、原entitlements保留、逐页签名及故意损坏页面的负对照通过。正式kernel/payload、984B参数、58项源码哈希、13区及最终FAT/压缩往返通过；完整镜像检查结果见各release/build-result.json。payload.json分片时payload_kernel_sha256仅记录第一片，完整SHA/CRC取build-result.json。

实机待逐项记录：三键短按/长按/组合、音量指示、锁屏/重新亮屏；滑块低中高、渐变、双片回读与可见亮度；至少20次亮灭屏；触摸/显示/Wi-Fi回归。音频出声与SoC深度休眠不在本轮。

当前保留的2.25×显示开发配置有独立已知紫线/约3分钟无响应，见根docs/status/display225-stability-readback-2026-11-10.md；该配置不能作为稳定性通过证据。原生@2x配置已单独归档。候选显示配置另行明确记录。

补充：保留2.25×开发配置及其已知问题，另生成原生@2x验收包，仅采用已归档的5条原始显示fixups。两者使用同一按键/背光内核、相同Data/GPU/Wi-Fi输入；生成后恢复工作树原2.25×配置，准确配置以各release/board.toml为准。

最终归档：

2026-11-10 已完成uke电源/音量键及双KTZ8866背光适配，按需复用最新上游。正式kernel/payload、5082项ASan/UBSan主机检查、13区、签名/section与压缩往返通过；完整kernel CRC `A79E83AEA028FE78`。原生@2x验收候选vendor_boot `9ad565a1a01a529c61f4b7baffd256ca301725963d3ce54fe58572456ac7d17b` /47374336B，参数984B，未刷、实机未验收。另存2.25×开发包，继承旧显示问题并标记DO_NOT_FLASH；工作树原2.25×配置已恢复，不改Data种子、不做主机userdata写入。

构建输出说明：主构建目录当前payload是最后生成的原生@2x伴随包；工作树board.toml已恢复2.25×开发配置，后续重建会按该配置生成。实际部署只使用指定release及其board.toml/SHA256SUMS，不混用主构建目录与另一配置的记录。

## 用户选定验收配置

2026-11-10 用户已明确选择原生 @2x 作为按键/亮度实机验收配置，指定候选 `2026-11-10-input-backlight-native2x`，CRC `A79E83AEA028FE78`，vendor_boot SHA `9ad565a1a01a529c61f4b7baffd256ca301725963d3ce54fe58572456ac7d17b`。此回复仅确定验收配置；尚未取得本候选刷入授权，未操作设备，硬件待验。2.25×源码保留。


2026-11-10 按本轮“刷吧”授权部署原生@2x按键/亮度候选，CRC A79E83AEA028FE78，vendor_boot SHA 9ad565a1a01a529c61f4b7baffd256ca301725963d3ce54fe58572456ac7d17b /47,374,336B。51份归档SHA及完整内核CRC通过；boot_a/vendor_boot_a/boot_b/vendor_boot_b Sending/Writing全部OKAY，A槽已回读，fastboot reboot成功。未做主机userdata写入/擦除；刷后停止设备轮询，等待用户屏幕反馈。启动、三键、系统亮度及亮灭屏仍待实机验收，深度休眠另案。证据 uke-port/releases/2026-11-10-input-backlight-native2x/flash-result-20261110-130340.json。


2026-11-10 原生@2x按键/亮度候选A79E83AEA028FE78用户报告无系统界面，已DO_NOT_FLASH。授权恢复原厂四分区/A槽/TWRP后只读回收506新/1542旧槽；内核运行至301.302s，backboardd23次AMFI c8退出，根因是Data-backed owner服务缺启动角色；双片亮度回读均失败却发布st4为独立缺陷。正在构建受限role5/显式ad-hoc、回读失败不发布及开机文字恢复修复，未刷新候选，未写userdata。见[回读](status/input-backlight-boot-failure-2026-11-10.md)。


2026-11-10 后续：修复候选已构建未刷：CRC `4B26F2189B14865B`，vendor_boot SHA `eaa6b8fda40f42ad6897017d51c18932c66607b835bbb24757fb663d5049f5d7` /47374336B，994B参数、58项源码、5133项ASan/UBSan主机检查、13区、FAT及压缩往返通过。原生@2x/5条显示fixups，原Data种子、12项addon文件及5个信任module保持。backboardd专用role5、库role0及显式ad-hoc通过检查；AMFI代码不变。初始读失败不发布背光，具体I2C故障根因尚待新日志。设备留原厂四分区/A槽/TWRP，尚未取得新候选部署授权，没有刷新包。


2026-11-10 按本轮“刷吧”授权部署原生@2x启动角色修复候选，CRC 4B26F2189B14865B，vendor_boot SHA eaa6b8fda40f42ad6897017d51c18932c66607b835bbb24757fb663d5049f5d7 /47,374,336B。57份归档SHA及完整内核CRC通过；boot_a/vendor_boot_a/boot_b/vendor_boot_b Sending/Writing全部OKAY，A槽已回读，fastboot reboot成功。未做主机userdata写入/擦除；刷后停止设备轮询，等待用户屏幕反馈。AMFI启动修复、三键和亮度仍待实机验收，双片I2C失败根因尚待新日志。用户建议的板级参数迁往ADT已列入后续，未修改本次994B已审候选。证据 uke-port/releases/2026-11-10-input-backlight-startup-role-repair/flash-result-20261110-132644.json。


2026-11-10 4B26F2189B14865B候选用户报vnode_rele_ext引用计数panic，已DO_NOT_FLASH；授权恢复原厂四分区/A槽/TWRP，只读164新/1884旧尾槽。先行故障是backboardd缺NSConstantDoubleNumber；随后请求recovery，panic位于APFS卸载/vflush/vnode_update_identity链。已改为零padding追加亮度模块依赖、保留原Foundation ordinal，10类负对照通过；重打包中，卸载panic与I2C超时仍独立未结案。未写userdata、未刷后续候选。


2026-11-10 新候选已归档未刷：`2026-11-10-input-backlight-runtime-append`，vendor_boot SHA `78d707706d3fa21663c0cc0cba86be3a600f76e69911a752a34739bb1b9d0c73` /47374336B；完整内核逐字节复用，CRC仍为 `4B26F2189B14865B`，不能仅凭CRC与失败包eaa6b8fd区分。原生@2x/994B；原58项kernel输入、5133项主机生产代码回归及正式kernel结果复用，新增原Mach-O/签名/10类依赖负对照、正式payload、13区、FAT与压缩往返通过。仅修正亮度模块依赖加载，保留原Foundation ordinal34；没有修改AMFI或VFS断言。背光超时、APFS卸载panic均未宣称修复；设备原厂四分区/A槽/TWRP，无主机userdata写入，部署仍需当次授权。


## 双背光 GENI 交接修复

2026-11-10 原生 @2x 双背光 GENI 交接候选已归档，CRC `ADFA0CC8B0DFA041`，vendor_boot SHA `ab9bf41ebcf8f49aef480d69ca34c84c6b943d7987717694d5b87a523411dc55`，47,378,432 字节。补齐双路时钟和 DFS 核验、GENI I/O/FIFO 初始化及 SCL 上拉，双片回读成功并发布服务后才挂载亮度运行时。保留已获用户确认的按键路径和 18 项 ADT。实包 676 字节，正式 kernel/payload、压缩往返和主机检查通过。本轮未刷入，最后枚举为 fastboot。实机需验证两路回读、滑块和亮灭屏，arm64e 旧电源桥接缺口仍待处理。 [详细记录](../../docs/status/backlight-geni-handoff-2026-11-10.md)。


## 本次部署

2026-11-10 17:35:37 +0800 按本轮“刷吧”授权部署原生 @2x 双背光 GENI 交接候选，CRC `ADFA0CC8B0DFA041`，vendor_boot SHA `ab9bf41ebcf8f49aef480d69ca34c84c6b943d7987717694d5b87a523411dc55`。36项归档SHA及完整内核CRC通过。boot_a、vendor_boot_a、boot_b、vendor_boot_b 均写入成功，A槽已回读，fastboot reboot 命令成功。主机未写入或擦除 userdata。实机启动、双片回读、滑块和亮灭屏待同次反馈。证据 `uke-port/releases/2026-11-10-backlight-geni-handoff/flash-result-20261110-173535.json`。


## GENI 接收回读与候选

2026-11-10 用户确认 `ADFA0CC8B0DFA041` 可进系统、亮度不可调。回读 393 个新槽确认两路停在 READ，RX FIFO 无数据，readiness 门禁保留原版 backboardd。原厂 TWRP 确认两路均为 400 kHz。新候选 `B916752F0731FBEE` 已构建归档，vendor_boot SHA `61531bf23abc886a4d88cc0c4daf976df9c41eb315a11722e50f636365f2c9e6`。短读处理 RX FIFO 事件，初始化超时且引擎空闲时可切换单命令重复 START。原生 @2x、18 项 ADT、按键及签名资产保留。正式 kernel/payload 和 6,458 项主机回归通过。设备原厂四分区/A槽/TWRP，新候选待授权和实机验证。 [详细记录](../../docs/status/backlight-geni-rx-2026-11-10.md)。


## 本次部署

2026-11-10 17:51:59 +0800 按本轮“刷吧”授权部署原生 @2x 双背光 GENI 接收候选，CRC `B916752F0731FBEE`，vendor_boot SHA `61531bf23abc886a4d88cc0c4daf976df9c41eb315a11722e50f636365f2c9e6`。40项归档SHA及完整内核CRC通过。boot_a、vendor_boot_a、boot_b、vendor_boot_b 均写入成功，A槽已回读，fastboot reboot 命令成功。主机未写入或擦除 userdata。实机启动、双片回读、滑块和亮灭屏待同次反馈。证据 `uke-port/releases/2026-11-10-backlight-geni-rx/flash-result-20261110-175151.json`。


## GENI 运行期写入对照

2026-11-10 用户反馈 `B916752F0731FBEE` 滑块可操作、面板亮度不变。同次 377 槽确认双片启动读回 1390/1390、服务发布及亮度覆盖成功，运行期两路写低位均超时。新候选 `A1E11CB2144AC516` 已归档，vendor_boot SHA `d370ef1aab8fcd127d0289a7b8af606f42199a5d08e9104ff3a5612d179213e7`。仅以 uke-backlight-irq=0 在有传输时保持轮询，记录请求和首次超时时钟状态。正式 kernel/payload、6,458 项主机回归及实包核验通过。设备原厂四分区/A槽/TWRP，待当次授权及实机对照。 [详细记录](../../docs/status/backlight-geni-write-poll-2026-11-10.md)。


## 本次部署

2026-11-10 18:06:25 +0800 按本轮“刷吧”授权部署原生 @2x 双背光 GENI 运行期轮询候选，CRC `A1E11CB2144AC516`，vendor_boot SHA `d370ef1aab8fcd127d0289a7b8af606f42199a5d08e9104ff3a5612d179213e7`。36项归档SHA及完整内核CRC通过。boot_a、vendor_boot_a、boot_b、vendor_boot_b 均写入成功，A槽已回读，fastboot reboot 命令成功。主机未写入或擦除 userdata。实机启动、运行期双片写回、滑块和亮灭屏待同次反馈。证据 `uke-port/releases/2026-11-10-backlight-geni-write-poll/flash-result-20261110-180617.json`。


## 亮度验收与 IRQ 交接候选

2026-11-10 用户确认轮询版 A1E11CB2144AC516 亮度切换正常，同次 20 组双片写回一致、错误和超时均为零。IRQ 交接修复候选 `F0E5326709076140` 已归档，vendor_boot SHA `c904069f4c1988122d1290b326c34d727905a63655e16e9e04fb8e22f7d586f5`。ISR 屏蔽中断投递后交给工作线程处理 FIFO，保持固件水位。正常中断路径使用 25ms deadline 兜底，空闲时停止定时器。正式 kernel/payload、17,158 项 IRQ 交接及 6,458 项生产方法检查通过。设备原厂四分区/A槽/TWRP，IRQ 候选待授权及实机验证。 [详细记录](../../docs/status/backlight-geni-irq-handoff-2026-11-10.md)。


## 本次部署

2026-11-10 18:22:26 +0800 按本轮授权部署原生 @2x 背光 IRQ 交接候选，CRC `F0E5326709076140`，vendor_boot SHA `c904069f4c1988122d1290b326c34d727905a63655e16e9e04fb8e22f7d586f5`。41项归档SHA及完整内核CRC通过。boot_a/vendor_boot_a/boot_b/vendor_boot_b 写入成功，A槽已回读，reboot 命令成功。主机未写入或擦除 userdata。中断版实机亮度待反馈。用户报告电源键后仅画面变黑、背光不熄，已续查 arm64e 电源桥接。证据 `uke-port/releases/2026-11-10-backlight-geni-irq-handoff/flash-result-20261110-182218.json`。


## arm64e 熄背光桥接

2026-11-10 已部署中断版 F0E5326709076140，重启后交还用户测试。主机定位到旧背光电源桥接被排除在 arm64e 构建之外。新熄背光候选 `19E50071A926C648` 已正式构建归档，vendor_boot SHA `0b2915eccaca6bfc0991e2fa26c5321810501822fb32064ef92fb145f4ff773d`。通过原主屏 selector12 成功返回回调异步写零或恢复保存亮度，修正排队零与亮屏同轮到达的处理。原24A446 ABI、最终arm64e分支、59项桥接/邮箱及6,458项生产回归核验通过。新候选待当次授权和实机验证。 [详细记录](../../docs/status/backlight-power-bridge-2026-11-10.md)。


## 本次部署

2026-11-10 18:31:18 +0800 按本轮授权部署原生 @2x 熄背光候选，CRC `19E50071A926C648`，vendor_boot SHA `0b2915eccaca6bfc0991e2fa26c5321810501822fb32064ef92fb145f4ff773d`。51项归档SHA及完整内核CRC通过。boot_a/vendor_boot_a/boot_b/vendor_boot_b 写入成功，A槽已回读，reboot 命令成功。主机未写入或擦除 userdata。用户已确认此前中断配置版调光正常，本次锁屏背光写零及亮屏恢复待实机反馈。证据 `uke-port/releases/2026-11-10-backlight-power-bridge/flash-result-20261110-183116.json`。

2026-11-10 用户确认已部署的中断配置版 F0E5326709076140 调节亮度正常。vendor_boot SHA c904069f4c1988122d1290b326c34d727905a63655e16e9e04fb8e22f7d586f5。记录用户所测亮度功能通过，中断投递/兜底计数与锁屏熄背光仍需同次日志核对。


## 两次卡死回读与目标字段修正

2026-11-10 熄背光版 19E50071A926C648 两次卡死，原包已 DO_NOT_FLASH。尾环按相邻窗口连续性及 boot UUID 拆出最新 Logo 102槽、前次桌面128槽和更早 IRQ 290槽，首个失败启动统一日志 1,607,220 条，最新启动缺可解析 tracev3，未取得卡死线程栈。目标字段修正版 `5E1DE44B2420C975` 已正式构建，vendor_boot SHA `ebedbaf9d0d97eea2da26f1a8b05fb67133f5facd23783a6d2cac18b3514c42a`。按原24A446 client+0xd8 读取主显示目标，移除返回回调中的注册树查询，将电源日志移到背光工作线程。59项桥接与6,458项生产回归、最终arm64e及实包核验通过，卡死完整因果及新候选实机效果待验。设备原厂四分区/A槽/TWRP。 [详细记录](../../docs/status/backlight-power-freeze-repair-2026-11-10.md)。


## 本次部署

2026-11-10 18:55:57 +0800 按本轮“刷”授权部署主显示目标字段修正版，CRC `5E1DE44B2420C975`，vendor_boot SHA `ebedbaf9d0d97eea2da26f1a8b05fb67133f5facd23783a6d2cac18b3514c42a`。51项归档SHA及完整内核CRC通过。boot_a/vendor_boot_a/boot_b/vendor_boot_b 写入成功，A槽已回读，reboot 命令成功。主机未写入或擦除 userdata。启动、桌面稳定性和锁屏熄背光/亮屏恢复待实机反馈。证据 `uke-port/releases/2026-11-10-backlight-power-target/flash-result-20261110-185549.json`。


## 再次卡死与显示客户端登记修正

2026-11-10 目标字段版 5E1DE44B2420C975 用户再次报告卡死，已 DO_NOT_FLASH。原厂四分区/A槽/TWRP只读回收233新槽，boot DD8DEEC0 的1,835,547条事件确认主桌面152.1704秒ready、日志到约167秒；未保存明确panic或卡死线程栈，移除getProvider未解决问题。新候选 `EB162C352E6EF226` 已正式构建，vendor_boot SHA `989c1f8c8703ea408b94fce9df3c95deb366008d4ff8dd7ffec9d0c7643e5ed7`。仅登记自有显示工厂创建的主屏客户端，以指针及registry ID匹配后读取24A446目标字段；常开回调不再查询任意客户端的类或注册树。66项桥接和6,458项生产回归、最终arm64e与实包核验通过。原生@2x、18项ADT、696B参数及亮度IRQ路径保持。尚未刷入，卡死修复与锁屏熄背光均待实机验证；设备留TWRP，主机未写userdata。 [本轮记录](../../docs/status/backlight-power-client-cache-2026-11-10.md)。


## 本次部署

2026-11-10 19:25:23 +0800 按本轮“刷吧”授权部署显示客户端登记候选，CRC `EB162C352E6EF226`，vendor_boot SHA `989c1f8c8703ea408b94fce9df3c95deb366008d4ff8dd7ffec9d0c7643e5ed7`。59项归档SHA、完整内核CRC及13区通过。boot_a/vendor_boot_a/boot_b/vendor_boot_b四分区写入成功，A槽已回读，reboot命令成功。主机未写入或擦除userdata。当前只确认部署传输，启动、桌面稳定性、亮度及锁屏熄背光/恢复待用户反馈和同次日志。证据 `uke-port/releases/2026-11-10-backlight-power-client-cache/flash-result-20261110-192522.json`。


## 最低亮度与输出使能修正

2026-11-10 用户确认客户端登记版EB162C352E6EF226不再卡死、点亮正常，但锁屏仅最低亮度并花屏，熄背光未通过。本次464新尾槽至281.599718秒，原显示层4次关屏/5次亮屏请求，背光邮箱仅有1次power=1；172秒关屏后仍写回双片8/8且display_on=1。源码还缺少KTZ8866 BL_EN位控制。新候选 `065F781ECE6CD0EE` 已正式构建，vendor_boot SHA `98fe988f7b6058812e0a62d3f8422de4a3b265df34b290e3511b40c3db96ba5c`。复用Linux上游0x08 bit6使能语义，双片分别读改写及验证；关输出后写零，恢复亮度后再开输出。限定已登记主屏客户端处理成功或Unsupported返回的请求，保留原返回值，过滤其它错误；关屏最低亮度不覆盖待恢复亮度。71项桥接/邮箱和118,876项生产检查、最终arm64e与实包核验通过。原生@2x、18ADT、696B及IRQ路径保持。新候选未刷；真实熄背光、锁屏花屏消失和循环稳定性待实机。设备原厂四分区/A槽/TWRP，主机未写userdata。 [本轮记录](../../docs/status/backlight-enable-gate-2026-11-10.md)。


## 本次部署

2026-11-10 19:54:06 +0800 按本轮“刷吧”授权部署双片背光输出使能候选，CRC `065F781ECE6CD0EE`，vendor_boot SHA `98fe988f7b6058812e0a62d3f8422de4a3b265df34b290e3511b40c3db96ba5c`。64项归档SHA、完整内核CRC及13区通过。boot_a/vendor_boot_a/boot_b/vendor_boot_b四分区写入成功，A槽已回读，reboot命令成功。主机未写入或擦除userdata。当前只确认部署传输，启动、桌面稳定性、亮度及锁屏熄背光/恢复待用户反馈和同次日志。证据 `uke-port/releases/2026-11-10-backlight-enable-gate/flash-result-20261110-195404.json`。


## 真实熄背光验收与两项余项研究

2026-11-10 065F781ECE6CD0EE真实熄背光及恢复已获用户和同boot双片0x3f/0x7f、0/1366回读确认。两个余项分别研究：Apple键盘过滤器默认LockKeyDelay=150ms与短按取消相符；GENI实际状态机的25ms逐事件模型复现关断写入100ms、确认200ms和点亮确认450ms，强怀疑IRQ下半部仍靠deadline推进，具体唤醒故障未定。研究没有修改运行源码或构建新候选，设备取证后留原厂四分区/A槽/TWRP。 [完整记录](../../docs/status/input-backlight-acceptance-research-2026-11-10.md)。


## 后续策略实现及IRQ来源候选

2026-11-10 候选129116FF4B98EE39（vendor_boot fc28aa378aa0240c52d0ac04f3299fbbb5d08b6c77baacd090f9cd2b4e0d1a4b）正式kernel/payload通过、未刷。本机按键服务LockKeyDelay=0播种并在自有Keyboard(1/6) event service合并/读回，音量/Home及其它键盘不改。新增IRQ signal/return、inbox/tick、captured/direct与等待时间计数，真实IRQ推进来源仍待同次实机。17策略、23实际Apple过滤范围、229归因、17160 IRQ交接、118876生产及71邮箱主机检查通过。原生@2x/696B/18ADT及065F完全熄背光回退保持。 [详细记录](../../docs/status/button-policy-irq-trace-2026-11-10.md)。


## 本次部署

2026-11-10 21:04:03 +0800 按本轮“刷吧”授权部署本机按键策略及IRQ来源确认候选，CRC `129116FF4B98EE39`，vendor_boot SHA `fc28aa378aa0240c52d0ac04f3299fbbb5d08b6c77baacd090f9cd2b4e0d1a4b`。80项归档SHA、完整内核CRC及13区通过。boot_a/vendor_boot_a/boot_b/vendor_boot_b四分区写入成功，A槽已回读，reboot命令成功。主机未写入或擦除userdata。当前只确认部署传输，启动、桌面稳定性、亮度及锁屏熄背光/恢复待用户反馈和同次日志。证据 `uke-port/releases/2026-11-10-button-policy-irq-trace/flash-result-20261110-210402.json`。

## Consumer HID 与直接关背光

1291 的 LockKeyDelay 已读回 0，短按仍失效。两路 IRQ 的全部捕获状态由截止定时器消费。候选 `DA490B0C85DE6245` 改独立 Consumer report 2、使用高优先级 callout 在 gate 内推进 I²C，且关屏只关闭 BL_EN、保留亮度码。主机通过，设备只读取证后留 TWRP。[实现和验收计划](../../docs/status/physical-buttons-irq-callout-2026-11-10.md)


## 候选部署

2026-11-10 21:35:04 +0800 按本轮授权部署原生 @2x 候选 `DA490B0C85DE6245`，vendor_boot SHA-256 `5c784948f9466e323c70bf5e904956540f3000a0dfeae5de5c5ab29882b8be04`。89 项归档校验和、13 区预检通过；四分区写入成功，A 槽已回读，重启命令成功。主机未写擦 userdata。系统启动、首次短按和关屏花屏待实机反馈。证据 `uke-port/releases/2026-11-10-physical-buttons-irq-callout/flash-result-20261110-213455.json`。

## 背光与画面时序

DA490B0C85DE6245 用户确认短按与调光正常。锁屏先黑约 0.1 秒后全灭，亮屏先亮背光约 0.1 秒后见画面。尾环 233 个新槽见 Consumer 匹配，未覆盖被测亮灭序列。恢复原厂四分区/A 槽/TWRP 并只读取证。

4A0A0C61C9B6C392 主机构建及归档通过，vendor_boot SHA c1bdcf43dfc7f2c230f07aa86249b7c1acdf182aa8624d251ea7710708fa686a。关屏高优先级调度，亮屏等新表面完成，200 ms 兜底。实机待验。uke-port/releases/2026-11-10-backlight-frame-sync/build-result.json。 [验收细节](../../docs/status/backlight-frame-sync-2026-11-10.md)

只读 Data 快照中的同次统一日志 boot UUID `93C46429-10FA-45BC-BABE-A45D6DAEC7A2` 补齐尾环缺失的测试段。SpringBoard 记录 34 条内建 C/30 按下/释放、17 次单击识别。13 次完整电源请求到第二颗 `BL_EN` 回读为 0.799–1.026 ms，最后一次回读前 `capW=151/150`、`capT=0/0`，最大 IRQ 到消费为 79/87 µs，已记录双片码为 1366/1366，`BL_EN` 为关 0x3f、开 0x7f，错误和超时为零。统一日志时间差按 timesync 的 625/12 修正，与 backboardd 内嵌单调时间的 65.526 s 跨度吻合。记录的是系统请求与寄存器回读，画面实际黑亮时刻没有光学测量。


## 候选部署

2026-11-10 21:56:12 +0800 按本轮授权部署背光与画面时序候选 `4A0A0C61C9B6C392`，vendor_boot SHA-256 `c1bdcf43dfc7f2c230f07aa86249b7c1acdf182aa8624d251ea7710708fa686a`。78 项归档校验和、13 区预检通过。四分区写入成功，A 槽已回读，重启命令成功。主机未写擦 userdata。亮灭屏时序、短按和调光回归待实机反馈。证据 `uke-port/releases/2026-11-10-backlight-frame-sync/flash-result-20261110-215603.json`。

## 锁屏黑帧提交

用户在 4A0A0C61C9B6C392 确认亮屏没有可见延迟，锁屏约 0.1 秒花屏后完全熄灭。801 个新有效尾环槽记录 10 次关屏的黑帧填充完成均早于关背光请求，双片关断回读耗时 0.727–0.987 ms。10 次唤醒 8 次等到表面完成、2 次走 200 ms 兜底。设备原厂四分区/A 槽/TWRP，只读回收。

871557F1A96ECDE1 正式构建与主机核验通过，vendor_boot SHA-256 8673a24f72041643f12ead7ed5d8917f1b55fc777ded12b7e23f37669f7f3172。纯黑提交先填满 GOP 再切 DPU，锁存后报告完成。已知未锁存时保留旧槽。实机待验，证据 uke-port/releases/2026-11-10-backlight-blank-scanout/build-result.json。 [实现与验收](../../docs/status/backlight-blank-scanout-2026-11-10.md)


## 候选部署

2026-11-10 22:16:11 +0800 按本轮授权部署锁屏黑帧候选 `871557F1A96ECDE1`，vendor_boot SHA-256 `8673a24f72041643f12ead7ed5d8917f1b55fc777ded12b7e23f37669f7f3172`。70 项归档校验和、13 区预检通过。四分区写入成功，A 槽已回读，重启命令成功。主机未写擦 userdata。锁屏花屏及亮屏回归待实机反馈。证据 `uke-port/releases/2026-11-10-backlight-blank-scanout/flash-result-20261110-221602.json`。


## 熄屏实机反馈与暂停

871557F1A96ECDE1 实机反馈。用户反馈：如果系统是直接灭屏，就会花；如果是在锁屏熄屏，系统有渐暗的过渡，就会好一些。 用户要求先停在此版。证据为本轮用户观感，原因待定位。
