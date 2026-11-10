# CED0 两次启动停滞与 GPU 池映射修正（2026-11-11）

用户报告 CED0C67E2D0F3E7C 第一次进入系统，上划后界面卡死。手动重启后屏幕停在 Apple 标志。CED0 原包和刷写回执已保留，原包标为 DO_NOT_FLASH。取证后设备为原厂四分区、A 槽、TWRP，主机只读 userdata。

| 启动 | UUID | 同次持久日志 |
|---|---|---|
| 第一次 | 1A7BA4B4-4BB2-4CB1-8137-D98CC7379260 | 1,863,446 条。GPU 已发布，记录显示提交至少 8 次。最后一条池采样为 129 个存活 grant、39,676 KiB。 |
| 第二次 | 7047A306-517C-4F66-BA53-BEE1E2B4289B | 283,240 条。GPU 已发布并接受显示提交。尾环还记录 SpringBoard、BackBoard 及至少 4 次提交。 |

两次均匹配 CED0 的 03:28:53 内核 banner。尾环通过 swap 2 的 IOSurface 地址 fffffff82118af20 配对第二次。尾环有 403 条 CRC 有效变化槽，排除 1645 个与 E8AF 相同的槽，最后记录到 247.970372 秒。尾环 SHA-256 为 `49598cf3e2dccb87942ab95b8ac5851c854deb6c2f3c4d75c75f6786382306e1`。原始及配对 boot 日志见 two-boot-evidence.json。Data 只读快照转移 12,310,069,248 B，FUSE 已卸载。

第二次 GPU 提交在 121 秒样本为 6 次，至 241 秒样本仍为 6 次。完成处理从 121 增至 361，已唤醒 5 次，记录的围栏超时为 0。内核仍输出电池、Wi-Fi、触摸心跳和按键事件。触摸报告计数在这次尾环为 0。pstore 为空。显示的 retire 日志只打印前四次，不能用打印次数推算全部退休。已接受显示提交也不能确认面板实际完成扫描。两次症状是否有共同触发点仍未证实。

## 上游修复

新拉取的 Adreno740 `5282bd96c73bd0e2e111c8380ce3c8b0c5c19ba9` 移除 GPU 池注册时的整段常驻 CPU 映射。上游在 sheng 实机记录过大范围常驻 WC 映射与 GMU 启动停滞，后续 sheng `43f6f01` 报告采用临时映射后进入桌面。CED0 仍使用旧的整段映射，第二次尾环还记录一段高位池注册返回 NoMemory。Uke 两次日志均已进入 GPU 发布阶段，现场未证实常驻映射直接造成两次症状。

在现有功能分支合入上游修复。池注册只保存物理地址标记，清零时按 run 创建 WC 映射并在退出前释放。保留 CED0 显式 WC 描述符、启动表范围检查、secure_memset 和 DSB。构建候选后以同次证据验证连续两次冷启动、进桌面上划、池分配和显示提交。

## 修复候选

候选 `A5E3AA6C0356E4BF` 已归档于 `uke-port/releases/2026-11-11-cpu-gpu-pool-transient/`。vendor_boot SHA-256 为 `353510a7af450abf2ecb8d3d62f61fbc4ee479c25ab62c6ce123bf116a6d3724`，大小 47,435,776 B。正式 kernel、payload、boot 和 vendor_boot 构建完成，58 项归档 SHA 和 13 区检查通过。最终内核确认注册仅返回物理标记，清零调用 XNU secure_memset，映射在 run 清零后释放。最终镜像保留配套 DarwinBoot 固件。修复候选未刷，设备留在 TWRP。


## 2026-11-11 A5E3 授权部署

用户授权刷入 `A5E3AA6C0356E4BF`。同包 boot 和 vendor_boot 已写入四分区，A 槽读回，重启命令成功。58 项归档 SHA 与 13 区检查通过。回执 `uke-port/releases/2026-11-11-cpu-gpu-pool-transient/flash-result-20261111-035206.json`。连续启动、桌面上划与 GPU 池待实机验证。
