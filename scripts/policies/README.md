# 本机签名根策略输入

本机 iPadOS 27 输入位于此目录下的 ipados-27.0.1-24A446/OS 与 SystemOS。
两个子目录均由 Git 忽略，各保存原有 manifest.bin、trusted-key.bin、
verification.json、header.bin 和 signature.bin。

这些是既有、已完整验证过的 OS/SystemOS 镜像的签名策略，两个卷共用同一 owner anchor。
构建必须保持原签名、公钥和 OwnerBootIdentity 一致；缺失时从已核对的原始输入恢复，
不得通过重新生成密钥、替换签名或放宽验证来消除错误。私钥不在此输入目录。
原有 owner 密钥另保存在工作区 uke-port/data/verified-root-owner-24A446/keys/，
目录权限 0700、文件权限 0600；正常内核构建读取 OS/SystemOS/trusted-key.bin，不访问该密钥备份目录。

正式 unixnu tree 会将这些输入复制到目标 tree/scripts/policies/；
build-sheng-source-storage.py 已支持从这个位置读取。清理目标 tree/build 不会删除这里的原件。
当前 CONFIG_XNU_SOURCE_STORAGE_TABLES=userdata，只把两个 544 字节签名头放入 root-policies.bin，
完整表仍随原 userdata 输入使用。

2026-11-08 恢复时已对两个完整 manifest 执行原 VerifiedRoot C 实现的 RSA 签名/表摘要验证，
并核对目标 profile 的镜像摘要、卷 UUID 及 OwnerBootIdentity 的两个签名头摘要。
恢复证据在工作区 uke-port/fixes/2026-11-08-main-tree-build-repair/。

切换 OS/profile 时必须单独准备匹配的已验证输入；prelink-regression.json 本身不是根策略。
