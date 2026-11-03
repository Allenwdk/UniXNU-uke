# Uke iPadOS27 GPU验收版本发布

## 2026-11-03：已验收GPU27版本发布main完成

用户确认“这版本上述问题都解决了”，当前GPU基线CRC **90B6BC701210D9DB**。本次7仓main全部推送且远端SHA回读一致；仅发布文档/依赖引用，运行时代码与验收镜像保持。

| 仓库 | 本次发布的提交 | 方式 |
| --- | --- | --- |
| [Adreno740](https://github.com/Allenwdk/NativeAdreno/tree/main) | `22abfe46098e568fd0a66e1e358f021f2c0e964f` | 旧main保存为`backup/main-before-uke-gpu27-20261103-230417`后覆盖 |
| [AdrenoMetal](https://github.com/Allenwdk/AdrenoMetal/tree/main) | `854f99a49e88febb52d4105baa88a4db4c27718b` | 旧main保存为`backup/main-before-uke-gpu27-20261103-230417`后覆盖 |
| [GopFramebuffer](https://github.com/Allenwdk/NativeMobileDisplay/tree/main) | `f3e29a2401cf1242b4e2eaf2b89ef2dd2fcacbaa` | 旧main保存为`backup/main-before-uke-gpu27-20261103-230417`后覆盖 |
| [SM7675](https://github.com/Allenwdk/UniXNU-sm7675-common/tree/main) | `7f857a825a3c1f0882a50eaa824249c714d9679d` | 快进 |
| [UEFI](https://github.com/Allenwdk/uefi-uke/tree/main) | `8a1153e6c54c508e613127af8c639334cdd8e51f` | 快进 |
| [UniXNU](https://github.com/Allenwdk/UniXNU/tree/main) | `899fdcbf80619d8a0d68d5646c7ab131304010b4` | 快进 |
| [Uke](https://github.com/Allenwdk/UniXNU-uke/tree/main) | `c7aefb985fabc3a6d7c13f63d1f458fa71768519` | 快进 |

有分叉的目标远端旧main均原样保存到 `backup/main-before-uke-gpu27-20261103-230417`，按用户要求再覆盖main，使用精确旧SHA的force-with-lease防止并发覆盖；没有把旧分叉源码合回测试树。其余4仓直接快进，未创建额外备份。旧main完整SHA见发布审计。

core `.gitmodules`/gitlinks及设备feeds现指向远端可获取的已验收依赖，保持canonical组件ID；NativeAdreno只是Adreno740的用户远端仓名。设备本地工作区路径迁入忽略的local.toml，clean clone不依赖它。SoC的Sm7675Platform由本身kits目录发现。
13区台账和resolver状态通过；本轮不重建/不刷机，不把文档pin提交当成新二进制。修复后长时间并发、精确帧耗时和跨冷启动缓存仍待独立量测。
证据 `uke-port/fixes/2026-11-03-gpu-main-publish/publish-result.json`、`publish.log`、`publication-checks.json`，验收证据release/user-observation-success.json。
本表记录首次发布快照；设备仓随后仅补写这份发布台账，仍按普通快进推送。
