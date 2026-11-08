# Wi-Fi V4.9.7 原缩放基线（2026-11-09）

用户确认本轮重新编译并刷入的镜像“这个不panic”，要求作为后续 main 基线。

- 源码：core `558a74742c608cbaacd6d405ede5ed757450f7f7`，device `dac703fdd6abbac85b1c6936fb305c7691931dff`。
- 两仓固定标签：`uke-wifi-v497-noscale-20261109`。从该标签或本地 main 创建后续开发分支。
- 镜像：工作区 `uke-port/releases/2026-11-09-wifi-v497-noscale-rebuild/`。
- kernel CRC：`8100A5409519BA15`。
- kernel-final SHA256：`4818fa7ab19ad5e823795365cc1d37d0b3d6efa6953d1c80e9543722cb00fe36`。
- boot SHA256：`2e46cba11922b6f2ea4018cdf5b75a564d8215fc75a8e17547bd90a5763c1328`。
- vendor_boot SHA256：`96c99ac6b995204327bf58117ff095b7c1d05cbee0df3b128c6ca8f5ee06b470`，47,087,616 B。

本轮正式 kernel、payload 和 13 区检查通过；49 项记录的编译输入哈希与 V4.9.7 验收包一致。保留原 scale2、999 B 参数和原 5 条 CA 修正。没有加入新增 J820 身份、缩放文本修正或本轮 HID pin 实验。

2026-11-09 03:51:56 +08:00 四分区写入、A 槽选择与重启命令成功。设备结论来自用户“不 panic”的反馈；本轮没有新日志回读，没有重新完成 Wi-Fi 全套验收或长期压力测试。11 月 8 日 Wi-Fi 基础验收单独保持。未写 userdata。

原始证据保存在 release 的 `flash-result-20261109-035153.json`、`user-acceptance-20261109.json`、`build-result.json` 和 `SHA256SUMS`；构建时状态字段不回写，验收另存收据。

本地 main 已更新；旧 main 为 `backup/main-before-uke-wifi-v497-20261109`。本次没有推送远端。主工作树的实验文件仍保留在原功能分支，main 的运行时源码与上述标签一致。
