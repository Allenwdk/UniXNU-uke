# 顶点命令分配失败与桌面黑屏（2026-11-12）

完整工作区报告：`../docs/status/gpu-replay-black-readback-2026-11-12.md`（相对设备仓根目录）。

用户报告 38703fa6… 版缺桌面图标/壁纸，进入桌面约一分钟后黑屏，电源键不恢复锁屏。同次 UUID 01D32BCB-6961-475C-AE1C-AA2C0E0767E4：约99.61s iconservicesagent 旧代资源拒绝；backboardd仍完成1945次命令，136.993s在Turnip顶点绑定库内0x5c6cc向NULL写入后SIGSEGV。替代进程GPU Busy，mount step0xb30明确是新VM attach遇到共享SMMU fault。首次fault原因未定，壁纸与图标同因也未证明。

core 9a9d835、Metal 6cf2ad3 修正分配失败后继续写命令及恢复日志ID前缀；3种分配失败/成功对照和3项Cargo契约检查通过。旧函数负对照ASan崩溃。实际Meson源码路径检查已加入，所选Mesa重新完整构建。

候选 release `2026-11-12-gpu-vertex-allocation`，vendor_boot SHA-256 d7b39501ecbf0dd96efeebc58a6ef72a920b3567e41d59521804cae1327d6467。内核复用上一正式构建，完整FAT内核SHA bb4693f772ddc50863bf239c96035e4d7e81ad785efc4123dec1679d7a9aa85a，772B参数、Data、Wi-Fi及背光不变。图形库arm64e构建、签名逐页/owner trust、正式payload及压缩回解通过；未刷。设备原厂四分区/A/TWRP，只读回读完成且未写擦userdata。

待当次授权后验收图标、壁纸、动画、>8分钟连续渲染及熄亮；首次GPU故障仍须新日志定位。
