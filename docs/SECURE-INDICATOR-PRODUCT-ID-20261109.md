# Camera安全指示异常：触发根因与修复（2026-11-09）

## 已查明的问题

这次SpringBoard反复abort由此前新增的J820 `/product/product-id` 触发：**这20字节不是单纯的界面/产品显示元数据，而是MobileGestalt设备类型匹配值，会开启特定Apple硬件功能路径。** 原Uke正常main基线没有该字段。

我此前将原始J820的 `fcafc1ef9614d7ffe41106f7b7a33e9194429fdf` 与artwork字段一起复制，导致QuartzCore将此设备匹配为支持Medina/Exclave安全指示的型号；Uke没有对应ExclaveOS/SIL资源，形成下列完整故障链。这是身份补齐实现的错误，不是摄像头传感器或ISP未适配的证据。

## 静态机器码与同boot日志证据

1. 原24A446 MobileGestalt在 `0x1af9ed64c` 从 `IODeviceTree:/product` 读 `product-id`，要求CFData长度20。`_MGIsDeviceOfType` (`0x1af9ebf0c`)将目标值与设备值按两个64位字和一个32位字逐项比较；`_MGIsDeviceOneOfType` 在 `0x1af9ec6a4` 遍历给定型号列表。
2. QuartzCore `___CADeviceSupportsMedina_block_invoke` (`0x18473f248`)构造一组20字节类型值，并在 `0x18473f5bc` 调用 `_MGIsDeviceOneOfType`。literal pool `0x184808200` 的前16字节加 `0xdf9f4294` 的尾4字节，**恰好完整等于被复制的J820 product-id**；匹配时在 `0x18473f5cc` 将Medina标志置1。对应系统偏好名是CAEnableMedina；本轮未修改它。
3. `-[CASecureIndicatorLayer _copyRenderLayer:layerFlags:commitFlags:]` 在 `0x18456f050` 检查该标志；启用时调用名称转换。其名称转换函数在 `0x18456fe48`：有SILManager弱导入时使用 `_SILManagerIndicatorTypeIDFromName`，否则有内置Camera/Microphone等默认映射。内置映射本来认得Camera，但本次弱导入存在，走SILManager。
4. SILManager在 `0x29dfe8c38` 调用 `SILManifest.manifest`，再调用 `indicatorFromName:error:`；没有manifest或没有对应项会返回-1。
5. 同次启动 **162BD3122A1B4071B374592D072209A8** 的PID30、297、313逐一记录：找不到ExclaveOS下的 `SILManagerAssets.framework`，fallback `SILManagerComponent.framework/secureindicatorassets/` 也不存在，随后 `No manifest files found`，紧接Camera类型解析失败与三次 `CASecureIndicatorLayerInvalidName`。
6. `_copyRenderLayer` 在 `0x18456f098` 抛该异常，返回位点 `0x18456f09c` 加实际cache slide `0x144000`，正好是运行日志栈中的 `0x1846b309c`。后续6个QuartzCore帧也在此slide下对应真实commit/flush调用链。

因此，已经证实的链条是：

```text
复制J820硬件product-id
  -> MobileGestalt设备类型命中Medina名单
  -> QuartzCore启用Medina安全指示路径
  -> SILManager加载不存在的Exclave指示资源，manifest为空
  -> Camera名称转换返回-1
  -> SpringBoard抛异常，连续三次abort，系统进入recovery流程
```

先前只提到“显示未发布IOMFBSupportsSecureIndicator”不足以解释它：那条BackBoard日志是真实的，但不是目前查到的核心触发开关。此次已补齐真正的类型判定与资源失败链，不能再把结论停在“可能是身份问题”。

完整原始缺资源路径为：

- `/private/preboot/Cryptexes/ExclaveOS/System/ExclaveKit/System/Library/PrivateFrameworks/SILManagerAssets.framework/`
- `/private/preboot/Cryptexes/ExclaveOS/System/ExclaveKit/System/Library/Frameworks/SILManagerComponent.framework/secureindicatorassets/`

所有静态地址基于原始24A446 cache，不是直接拿带ASLR的运行地址做符号查找。证据：本轮fixes下的 `root-cause-proof.json`、`current-secure-indicator-unified.json`、MG/QuartzCore/SILManager反汇编与固定符号表。

## 本轮修复与镜像

只撤回OS profile中新加的Apple硬件 `product-id`，恢复Uke已验证基线不声明该硬件型号的状态；保留其余7项产品/artwork字段、设备sRGB、原scale2、chosen scale缺省、5条CA修正和实例析构绑定修复。

不改QuartzCore异常/隐私指示检查，不发布虚假的SIL硬件能力，也不添加空manifest。没有改相机驱动。

- core修复提交：`879b694d081feea1a99537d0f0ee645175ad34f6`，仍在 `fix/uke27-identity-touch-retry-20261109`。
- release：`uke-port/releases/2026-11-09-secure-indicator-repair/`。
- vendor_boot SHA：`40efee0fb5764f7539f41355118164c73e0b27aaa68940751fa6840a13c665b1`，47,083,520 B。
- 完整kernel SHA：`ce558022d19b8a9e0979da4b1e333fb94330ffe8718fc355081ebbb2dbd7b7f4`，与sRGB失败包逐字节一致，CRC仍 `6C93A149891869B1`，必须以vendor_boot SHA区分。
- 只正式重建payload，未重编kernel。正式payload/UEFI压缩回环/紧凑vendor_boot/13区通过；最终FAT中配置、kernel、trust、addon、vroot、Data与crypto HMAC均逐字节保持。最终ADT只删除product-id并更新random-seed，证据 `single-variable-proof.json`。
- **未刷入**，没有设备操作或userdata写入；设备保持原厂四分区/A槽/TWRP，main基线不动。实机能否恢复桌面仍需后续本轮授权刷入验证。

打包命令仍从UniXNU主树运行 `PATH=<TianoCompress目录>:$PATH ./unixnu build payload --kernel <此前已核对的kernel-raw.macho>`，完整记录在本轮fixes/payload-build.log与package-release.py。正常回退仍是main基线release `2026-11-09-wifi-v497-noscale-rebuild`。
