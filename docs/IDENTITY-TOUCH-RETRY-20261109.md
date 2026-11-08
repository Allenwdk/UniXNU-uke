# J820 身份与触摸重试（2026-11-09）

分支 `fix/uke27-identity-touch-retry-20261109` 从 core main `558a7474`、device main `9198a6f4` 创建；原实验另存 stash 和本轮 fixes，main 基线保持。

用户限定本轮只重试身份与触摸，保持原缩放。本轮只补原始 24A446 J820 `/product` 的 8 项身份字段；不增加 `/chosen/display-scale=0`，保持原 scale2、999 B 参数及原 5 条 CA 修正。product 内的 artwork-scale-factor 是原设备身份值，不改板级显示模式。

sheng 已 `git pull --ff-only` 到 `a68ee4ee67cc71792fbf519de7e3e28072c717b5`。ShengHidInput 上游 main 为 `71a40e76daeae1bb4127546f96ffa7339c7a06ff`，其 tracked tree 与旧 pin `24f1f3fbed6feff42e1ca02e2ce386a5c3c36da8` 相同；本轮同步 pin，没有宣称新增触摸算法修复。触摸轴仍为 30480×20320，不替换成显示像素。Dock 与台前调度效果须实机验证。

## 两次 panic 的机器码复核与修正

从两份失败 release 的实际 FAT payload 重组完整 kernel，均得到 SHA `32005e98f4f710987b55a65bab51b2c7c2a17d5b8627172651a9fb86eb5facf8`。首次包元数据中的 `0a310f5c…` 是原 kernel 输入，不能当最终上机 kernel；第二次更换 kernel 的说法没有形成有效对照。

PC `0xfffffff00b404ef4` 实际为 `OSMetaClass::~OSMetaClass()+0x34`，从 `this+8` 取 `reserved`，再访问其 `+8`；照片 `x8=0xefff` 对应 FAR `0x10007`。去 PAC 的 LR `0xfffffff00b629628` 位于 `NativeLegacyDisplay::~NativeLegacyDisplay()` 的 deleting destructor 中，在调用基类 D2 后返回的位置。此前“AGX 元类构造失败”定位错误，已撤回。

旧 fixture 将 `IOMobileFramebufferLegacyD2Ev` 绑定到原地址 `0xfffffe000a288c6c`。它通过 `0xfffffe000a299530` 和 chained import `0xfffffe000836b0e8` 调用 `OSMetaClassD2Ev`；普通显示对象被当元类清理，恰好解释上述错误内存访问。

本轮将该绑定改为 `0xfffffe000a288cac`，它依次尾调 `0xfffffe000a2819bc`、import stub `0xfffffe000a2999e0`，后者经 `0xfffffe000836b340` 调用原 `IOServiceD2Ev`。两条 import 的实际目标分别为 `0xfffffe000b3d385c`（元类）和 `0xfffffe000b46c2d0`（普通 IOService），由原始固定 kernelcache 和最终文件机器码共同核验。保留正常析构/PAC/错误返回，不增加跳过清理的 guard。

这是已证实的析构绑定错误；触发对象清理的上层初始化失败条件仍未得到完整运行日志。本轮构建、打包及实机结果分开记录，不因静态修正就宣称 Dock 或启动已修复。

打包使用原板级 `pack-uke-vendor-boot.py`，保持紧凑 vendor_boot 布局；不直接使用通用 packer 的完整分区输出。主机证据在工作区 `uke-port/fixes/2026-11-09-identity-touch-retry/`，镜像归档在 `uke-port/releases/2026-11-09-identity-touch-retry/`。无设备操作、无 userdata 写入。
