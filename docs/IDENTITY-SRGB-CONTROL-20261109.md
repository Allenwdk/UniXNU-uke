# Uke 产品色域 sRGB 对照（2026-11-09）

用户要求“色域改回srgb试试”。相对此前不panic但swap失败的身份候选，只在设备 `board.toml [adt.product.string]` 添加 `artwork-display-gamut="sRGB"`，覆盖OS profile的P3。其余7项新身份、原scale2、chosen scale缺省、5条CA修正与Legacy实例析构修复保持。

这次只正式重建payload，复用上一版已核对的精确原始kernel，不声称重新编译kernel。最终FAT重组的完整kernel SHA `ce558022d19b8a9e0979da4b1e333fb94330ffe8718fc355081ebbb2dbd7b7f4` 与上一版逐字节相同；配置、两个kernel分片、trust、addon、vroot、Data种子及crypto HMAC文件均逐字节相同。ADT差异仅 `/product/artwork-display-gamut` 与 `/chosen/random-seed`。证据为release的 `single-variable-proof.json`。

- release：工作区 `uke-port/releases/2026-11-09-identity-srgb-control/`。
- vendor_boot SHA：`3d249d9934883c9bc9c4e5dcd59a08aa688b0acbfd46f7e6baca1902362cdb1d`，47,083,520 B。
- kernel CRC仍为 `6C93A149891869B1`，不能用于区分前一P3包；以release路径和vendor_boot SHA区分。
- 正式payload、压缩回环、紧凑vendor_boot打包及13区通过。
- 未刷机、未操作设备或写userdata；设备保持原厂四分区/A槽/TWRP。main不变。

这是色域单变量对照，是否让实际surface从b3a8切回显示驱动接受的格式、是否消除swap失败，须当次授权刷入后实机验证。本轮没有新增b3a8支持或改动触摸算法。
