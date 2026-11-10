# ADT驱动选项迁移实施（2026-11-10）

用户授权实施输入及显示配置迁移，并恢复因命令行预算省略的配置。core/device分支均为feat/uke-adt-driver-args-20261110；修改前的tracked diff、untracked文件、board及local输入保存在uke-port/fixes/2026-11-10-adt-driver-args/before。保留原显示、Wi-Fi和GPU开发改动，没有设备操作。

## 实现

18项进入/chosen/unixnu-driver-args字符串：触摸/HID/按键/背光6项，加显示register/gop/unit-scale/pm/validate/transfer/cpu-present/phase/noconsole和dpu-scanout/vsync/vsync-fixed12项，值保持。生成器和payload共用driver_args.py校验，内核白名单通过host回归精确比较，避免漂移。

当前命令行652B；ADT属性387B（含结尾NUL）。恢复serial=3、boot-progress=2、sheng-gpu-preempt=2，debug=334及文字/延迟控制台已保留。preempt2原来是同值默认，现在显式恢复；本27线boot-progress无实际消费者，只恢复配置字符串，不宣称增加动画/功能。没有恢复因性能/稳定性撤回的实验开关。

设备补丁uke-adt-driver-args.patch追加在现有patch series尾部。两种PE_parse入口复用原解析器，命令行优先、限定键的ADT其次、既有PE_builtin最后。其余原XNU键不查询新属性；既有owner默认和接口不变。整个属性先校验：最多4096B、一个终结NUL、无内嵌NUL、键唯一/白名单、canonical十进制或0x十六进制u32；非法/缺失返回原回退逻辑。显式name=0不落到默认项。

SecureDT初始化前返回原默认路径，初始化后重新查询，不缓存缺失或跨地址滑移的裸指针。C和IOKit消费者仍调用原API，故BacklightProbe和kern_exec挂载门禁、触摸/HID/显示两侧均能读取同一配置。没有把ADT字符串复制回CommandLine，也不扩大boot_args ABI或1000B门禁。

内核含unixnu-driver-args-v1 ABI标识；payload存在ADT驱动选项时拒绝不含该读取能力的旧内核，避免误配。payload.json新增ADT原参数、18项配置值/来源和覆盖顺序；真实kernel_args仍是实际652B命令行。

## 主机验证

check-adt-driver-args.py提取实际prepared XNU bootargs.c和生产header，在ASan/UBSan下通过237项检查，包括18项整配置实际读取、旧payload全CLI路径、显式关闭、字符串/1/4/8字节/presence查询、初始化前后与不同DT地址、NUL/超长/重复/未知键/数值溢出，以及原XNU键隔离。内核与Python白名单18项一致；旧内核+新ADT的构建负对照通过。它不是实机验收。

首次编译发现当前kernel头不声明memchr，已改为有界NUL循环。此前失败留下缺失bootargs.o，archive的真实导入检查因PE_parse_boot_argn缺失而拒绝；使用同一编译记录重编真实bootargs.o后重跑正式构建，没有加stub或绕过门禁。正式kernel/payload归档检查通过。

## 候选与验收边界

仍采用用户已选原生@2x验收配置，2.25×显示开发源码保留。继续使用此前已归档的运行时依赖追加修复（Foundation原导入保持、backboardd role5/库role0）；背光I2C超时和APFS卸载panic仍未结案，本轮迁移不宣称修复这两项。

新候选必须包含匹配的新kernel和ADT；旧payload仍可依靠命令行工作，旧kernel不能消费ADT配置。后续实机先验启动和配置值/来源，再测触摸、按键、显示/亮度；需要当次部署授权，未刷入、未写userdata。

## 最终归档

2026-11-10 ADT驱动选项迁移候选已构建归档，未刷：CRC `04965B68DC35D914`，vendor_boot SHA `2bb066f8239ff0fccd7173a40b409e05f0cf99a9628521b6b8ad0c609904e28b` /47378432B。命令行652B，ADT驱动选项387B；输入/显示18项完整保留，恢复serial=3、boot-progress=2、preempt2。正式kernel/payload、237项生产解析器ASan/UBSan、旧kernel误配负对照、13区、FAT与压缩往返通过。原Data/GPU/Wi-Fi addon/trust保持，与78d70770基线的ADT差异仅新增独立属性和随机seed。用户态依赖追加修复仍待实机，I2C超时/APFS卸载panic未结案。本轮未操作设备。

归档`uke-port/releases/2026-11-10-adt-driver-args-native2x/`。主构建payload为此原生@2x候选，工作树保留2.25×显示开发fixups与新的ADT配置，后续重建以board.toml为准；部署以指定release配置和SHA256SUMS为准。新候选尚无刷入授权或实机验收。


## 本次部署

2026-11-10 16:50:14 +0800 按用户本轮“刷吧”授权部署原生 @2x ADT 候选，CRC `04965B68DC35D914`，vendor_boot SHA `2bb066f8239ff0fccd7173a40b409e05f0cf99a9628521b6b8ad0c609904e28b`。55项归档SHA及完整内核CRC通过。boot_a、vendor_boot_a、boot_b、vendor_boot_b 均已写入，A槽已回读，fastboot reboot命令成功。主机未写入或擦除userdata。启动、触摸、三键与亮度待用户屏幕反馈和同次日志，双片I²C超时及APFS卸载panic仍待实机验证。证据 `uke-port/releases/2026-11-10-adt-driver-args-native2x/flash-result-20261110-165006.json`。


## 苹果 Logo 回读

2026-11-10 用户报告原生 @2x ADT 候选 `04965B68DC35D914` 停在苹果 Logo，已标记 DO_NOT_FLASH。原厂四分区/A槽/TWRP 已恢复，主机只读回收 464 新槽及 boot 40E1661D 的 1,114,187 条统一事件。BackBoard 实际呈现 AppleLogo，4 次直扫已退役，144.52 秒后快照中无新帧，内核运行至 280.86 秒。双 I²C 初始化超时后物理亮度模块仍加载，CoreBrightness 主屏容器失败。因果待对照，下轮优先仅隔离 backboardd 亮度覆盖并保持 ADT/输入配置。主机未写 userdata，设备留 TWRP。 [回读记录](../../docs/status/adt-driver-args-logo-readback-2026-11-10.md)。


## 亮度覆盖对照

2026-11-10 已构建原生 @2x 亮度覆盖对照 `C04832F3FA2E2C59`，vendor_boot SHA `a28c9a1f2bed4676977d799fbedaf22f6fdba2c9be12ff85a13d1c30159e7b5b`。候选以 `uke-backlight-runtime=0` 跳过亮度覆盖挂载，保留按键、背光探测、18项ADT和原生@2x。实包676字节，正式kernel/payload、arm64e、FAT、压缩往返及签名资产比对通过。原厂四分区/A槽/TWRP沿用上轮状态。本轮未操作设备，待当次刷入授权与同启动验证。 [对照记录](../../docs/status/adt-no-backboard-overlay-2026-11-10.md)。


## 本次部署

2026-11-10 17:15:43 +0800 按本轮“刷吧”授权部署原生 @2x 亮度覆盖对照，CRC `C04832F3FA2E2C59`，vendor_boot SHA `a28c9a1f2bed4676977d799fbedaf22f6fdba2c9be12ff85a13d1c30159e7b5b`。28项归档SHA及完整内核CRC通过。boot_a、vendor_boot_a、boot_b、vendor_boot_b 均写入成功，A槽已回读，fastboot reboot 命令成功。主机未写入或擦除 userdata。启动和三键待同次实机反馈。证据 `uke-port/releases/2026-11-10-adt-no-backboard-overlay/flash-result-20261110-171535.json`。
