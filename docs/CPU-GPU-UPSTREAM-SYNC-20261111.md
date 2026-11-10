# CPU/GPU非温度功能同步与L3最高档（2026-11-11）

用户授权：“把除了温度限频以外的都同步过来，l3的暂时也用sheng的投最高档。”
本轮按此前清单同步CPU/GPU及直接依赖，排除ThermalProbe、ThermalLevel和新的
温度/热压力策略；不实现MEMLAT动态L3策略。D4EF电池UI验收包保持阶段回退。

## 上游与分支

core main已fetch到268bfa1c587db5ee616e955cdfd97a281500a6fb，sheng参考为
81f43ebaaa3317a5917b76093f7bc44c4949bc02。ArmCpu合入d90e457，Adreno740
合入9d99240，DarwinBoot合入5243c68。所有修改留在
feat/uke-ipados27-dsp-audio-power；每个仓另有backup/before-uke-cpu-gpu-sync-20261111。
实际完整SHA与来源在uke-port/fixes/2026-11-11-cpu-gpu-upstream-sync/。

## 实现范围

- CPU：复用最新sheng CpufreqProbe的ramp统计与16ms CPF1记录，保留3/4/1
  SM7675 EPSS分域与硬件LUT；原厂已有L3 base17d90000、LUT+100、vote+90。
  mode4按sheng启用后只投一次最高有效档（可受显式cap限制），增加级别/频率/读回日志，
  不是MEMLAT governor，不宣称实际L3频率已测量。新增L3页按工作区既有规则走
  ml_io_map_unmappable，投票算法不改；原sheng poll接入现有日志轮询，在enable延后时重试，成功仍只投一次。
- GPU governor：同步80ms真实空闲才触发wake boost、多窗口gpu-dcvs-span、窗口及原因统计。
  保留uke GMU启动表的九档255/353/500/633/684/736/835/900/950MHz、默认floor500、
  第九档独立占比数组及原BR/低开销追踪，未拿sheng A740频点替换。
- ArmCpu：同步共享电平SPI与直接分发、注册锁/屏蔽、idle/IPI/定时器/PSCI统计及
  idle-latency、可选Edge初始化接口。保留Uke Wi-Fi所需64槽，同时把路由位图扩到64位，
  避免32–63槽丢路由。idle-latency和Edge开关保留上游默认，未实测前不调参。
- GPU驱动：合入按ring围栏完成唤醒、每次暂停后推进ring切换、同步缓冲的cache维护
  优化、映射期间才运行mailbox、Deny角色优先级、BO/view/ring诊断与WC system BO池。
  围栏入口已接入板级gpuIrq，原有SMMU、preflight就绪/生命周期与BR计划保留。
- pool与preflight：NativeAdrenoSystemPages的held/grant与输出地址位宽准入一起接入
  既有IOKit锁外准备路径，未采用的grant也在原清理边界归还。trace flag16保留Uke
  preflight意义，pool用flag32，防止两套统计混淆。
- 内存池：复用sheng GpuMemoryPool读取DarwinBoot提供的最终UEFI内存图中、XNU窗口
  外的conventional空闲区；ADT预留16组范围，未手写DRAM地址。UEFI中ADSP、DTB、WPSS、
  GMU等保留区继续为reserved，D4EF的4.75GiB XNU窗口大小不变。
- 启动参数：在XNU原参数层内部匹配sheng-*与新上游无前缀别名，保留CLI>ADT>builtin
  优先级。现有原生@2x、18项ADT输入/显示参数和原签名addon/trust/Data继续保留。
- 诊断接线：CPF1分配、预算、文件计数和输出一起接通；独立UkeLastLog输出CPU ramp/
  idle、GPU窗口/原因、围栏、池、维护及BO/view owner计数。未引入温度传感器或热服务。

## 合并与构建边界

core的显示、Metal、RamBlock和SoftwareAES gitlink保持既有检出，CPU/GPU与
DarwinBoot各自合并后再更新指针。第一轮正式构建发现新驱动的
NATIVE_ACCELERATOR_BASE宏与旧已验收Metal ABI头文件的接口差异；构建配方在
24A446旧头下补IOAcceleratorES宏，保持原继承布局及签名Metal产物。
失败日志kernel-build-attempt1-accelerator-abi.log保留。

按既有要求不追加可选主机模型/单元/压力测试，以正式kernel、firmware、payload、
bootimg与镜像输入/结构核验为本轮门禁。正式构建和最终归档已完成，不把已编译或静态接线当作硬件验收。
设备仍以最后用户反馈的D4EF/A槽为准，本轮没有ADB/fastboot操作。

最终L3接线补齐：新页使用现有native MMIO API，sheng原有poll由既有日志轮询每2秒调用；固件延后enable时重试，成功仍只投最高档一次。A75为未刷中间包，已标DO_NOT_FLASH，由v2替代。


## 最终候选与验收门禁

最终候选 `E8AF2F935F600E17`，归档 `uke-port/releases/2026-11-11-cpu-gpu-upstream-sync-v2/`。
boot SHA256 `e8cbc39f13b40300d930915b7da3b9ab3ec2c5b63af03292c7ae88215cf9b289`；vendor_boot SHA256 `e7f17d80aa29b03d1472e4d58a65c02be7d90c564ed1933d6c50fe1b8c805d19`，47,435,776B。
新boot包含FD SHA256 `8c19c72451def1ea302bf37525cd1ebb03584ff0b44fd5beae5895d1938f959d`，
DarwinBoot EFI `56ee330f9bceb16efa028cf957adbb96d8ac38b943a4693c24ef11fcaa9bb058`。
必须与本包vendor_boot成套使用；D4EF原包仍保留作电池UI已验收回退。

| 仓库 | 最终归档源码 |
|---|---|
| core | `7d8a23ae207a465b28925c0cb81bc74e11794835` |
| device | `0c45de3fbbc4a1155f6fa551dfb2fae4a80cc04e` |
| soc | `e5c8d35c32381613e72d37b5dfdc3d65c923b39e` |
| ArmCpu | `795159ef145e1768b27e7fa9c1bf01dcc981e17c` |
| Adreno740 | `c3cb6093c4ba466d7c25589df85f9b92440e528a` |
| DarwinBoot | `a4e33b3af7574425da7cc52ba7356b466d084ad7` |
| uefi | `8a1153e6c54c508e613127af8c639334cdd8e51f` |

source-revisions.json是最终源码身份；coverage.json保留初始合并范围，之后的L3
重试和native-MMIO补接由最终device源码及build-report源哈希覆盖。正式kernel、
firmware、payload、boot/vendor_boot通过；83项显示/平台源码、14项存储源码、
实际归档SHA及两个组件archive均核验。原ADSP、电池、Wi-Fi、背光和触摸源与
D4EF归档逐字节对照一致。

固件LZMA解压后包含实际新DarwinBoot模块，FFS正文与构建输出一致（GenFv
仅反转FFS状态字节）；ADT相比D4EF只新增16组空outside范围及正常random-seed。
721B启动参数和18项输入/显示ADT、addon/trust/Data签名资产保持；prelink
缺失2/桩24与基线一致。最终L3 vote+90、GPU IRQ调用和SPI route的64位ldsetl
指令、CPU/GPU新符号已核对，GPU HFI表哈希与同步前一致。13区检查通过。

A75A9E6483062E93是未刷中间包，补齐L3延后enable的原有poll接线与native-MMIO
后由v2替代，已标DO_NOT_FLASH，不当作硬件失败。预检脚本旧路径问题已修复，
42处DT保留区对77个UEFI描述符无静态错误；stock cfg额外扫描本次为0，未冒称
已经检查全部原厂配置。失败日志和中间包均保留。

候选未刷，硬件未验。本轮没有ADB/fastboot，没有主机写擦userdata；未运行额外
可选模型/单元/ASan套件。下一步需该候选当次授权后刷四分区/A槽，再验证同次
镜像身份、L3 enable/LUT/vote读回（请求最高档不等于测得最高实频）、CPU/GPU
升降档、围栏/切换、outside池范围/分配/回退，以及电池、Wi-Fi、显示、背光、
触摸与稳定性回归。CPU idle-latency与Edge、GPU span保留上游默认，不预先调参。
温度采集/热压力与新温度限频功能未引入，L3动态MEMLAT按用户要求暂缓。
