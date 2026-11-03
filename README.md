# UniXNU-uke

Xiaomi Pad 7 / POCO Pad X1 (`uke`, SM7675) 的 UniXNU format-2 设备仓。
当前目标为 **iPadOS 27.0.1 / 24A446 / arm64e**，配置 `uke_ipados27_defconfig`。

2026-11-03，用户确认当前GPU版本的启动、桌面/设置图标和小组件加载、设置中上划Dock时的界面重启问题均已解决。
基线 `2026-11-03-ipados27-gpu-coreimage-resources`，CRC **90B6BC701210D9DB**。
完整镜像SHA、修复过程与验收范围见 [GPU-IOS27](docs/GPU-IOS27.md)，结构化记录见 [porting.toml](porting.toml)。
精确性能、长时间并发与跨冷启动缓存测试尚未量测。

## 构建与源码

技术路线为 Metal → metal2vulkan → Turnip → Darwin KMD → Adreno，使用真实chip ID `0x43030b00 / gen7_11_0`。
保持上游每进程VM、CP页表切换、mode2双ring抢占、FIFO/优先级、内存复用、缓存和恢复。
SM7675共享操作由 [UniXNU-sm7675-common](https://github.com/Allenwdk/UniXNU-sm7675-common) 提供；板级OPP、带宽、固件和分区输入归本仓。

在配好的 [UniXNU](https://github.com/Allenwdk/UniXNU) 工作树中执行：

```sh
./unixnu defconfig uke_ipados27_defconfig -D ../UniXNU-uke
./unixnu fetch -D ../UniXNU-uke
./unixnu build kernel -D ../UniXNU-uke
./unixnu build payload -D ../UniXNU-uke
```

IPSW、原24A446信任输入、现有Data seed、owner身份及gen71100固件等私有构建输入由本地配置提供，不随源码上传。
feeds中提交远端可获取的固定版本；可选本地覆盖放入忽略的 `local.toml`，不作为新克隆的前提。
内核与用户态GPU更改后必须重建匹配的addon和owner trust。构建细节见 [GPU-IOS27](docs/GPU-IOS27.md)。

160MiB原FAT使用标准UEFI压缩运输，vendor_boot采用压缩文件；须配套支持解压的 [uefi-uke](https://github.com/Allenwdk/uefi-uke)。
保留原DTB、table和bootconfig，最终vendor_boot小于96MiB。

## 刷写与记录

boot/vendor_boot必须成对刷 `boot_a`、`vendor_boot_a`、`boot_b`、`vendor_boot_b`，再 `fastboot set_active a`。
recovery转bootloader使用 `adb reboot bootloader`；不擦Data。构建/刷写成功与真机功能验收分开记录。
现有CPU回退基线为 `2026-11-03-ipados27-ca-surface-track`；具体命令见release README。

- [项目进度](docs/PROGRESS.md)
- [iOS27 GPU实现与验收](docs/GPU-IOS27.md)
- [历史GPU硬件记录](docs/GPU-PORT.md)
- [编辑与验证约定](AGENTS.md)

旧iOS17和早期stage-6记录保留在Git历史与GPU-PORT/PROGRESS中，不能代替当前27线状态。
