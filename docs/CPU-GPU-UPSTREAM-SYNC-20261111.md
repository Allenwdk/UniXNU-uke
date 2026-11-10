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
bootimg与镜像输入/结构核验为本轮门禁。构建中，不把已编译或静态接线当作硬件验收。
设备仍以最后用户反馈的D4EF/A槽为准，本轮没有ADB/fastboot操作。

最终L3接线补齐：新页使用现有native MMIO API，sheng原有poll由既有日志轮询每2秒调用；固件延后enable时重试，成功仍只投最高档一次。A75为未刷中间包，已标DO_NOT_FLASH，由v2替代。
