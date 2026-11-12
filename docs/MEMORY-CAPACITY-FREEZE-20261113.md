# 冻结归因与12GB内存利用核对（2026-11-13）

用户要求暂不刷连续日志候选，先确认冻结是否必为GPU以及12GB是否全部利用。本轮没有执行开发包刷写，仅在现有TWRP读取Linux内存表，并核对失败启动记录、实际payload ADT和当前/上游源码。

结论：冻结根因未定，不能限定为GPU硬件或驱动。VM窗口外接管、映射/缓存属性、页回收和锁等待也应纳入调查。当前XNU接管规模约 **8.865 GiB**，没有把整机全部12GB交给XNU；这个容量事实本身不证明冻结由内存引起。

## 当前实际容量

| 范围 | 实证 |
|---|---|
| 失败XNU启动主窗口 | `1519080 free pages + 53784 wired pages = 1572864 pages`，4KiB页，正好6GiB；不是6GiB空闲内存。 |
| 失败XNU窗口外接管 | 内核22.830963秒：`751104 pages adopted, 0 refused`，共2934MiB；145.255561秒再次报告相同计数。 |
| XNU合计接管规模 | 6GiB + 2934MiB = 8.865234375GiB。adoption实现同步增加max_mem/max_mem_actual/mem_actual/sane_size；未在冻结系统上读取hw.memsize，不把该计算值冒充现场sysctl。 |
| 本轮TWRP/Linux MemTotal | 11,746,988 KiB = 11.20280075GiB，来自当前设备/proc/meminfo。它属于recovery启动，不是失败XNU启动的可用内存。 |
| 当前Linux live DT内存段总和 | 11.715026855GiB；其中还有内核/固件保留，不能把DT段总和当可供应用分配的内存。 |

实刷f33ab396的FAT内ADT直接读取确认：`dram-base=0x8C0000000`、`dram-size=0x240000000`（9GiB）、`darwinboot-memory-size=0x180000000`（6GiB）。窗口外表在payload里是运行时填充占位，静态为零并不表示未启用；751104页接管日志证明本次运行时已启用。启动参数没有memory-outside=0，实际源码wanted默认1；gpu-pool=0且同次pool计数全零，避免同一批页再交给GPU专用池。

## 没有利用的部分在哪里

Uke board的DRAM边界限定为 `0x8C0000000..0xB00000000`，只覆盖连续9GiB高位段。UEFI MemoryMapLib.c第82–89行也只静态登记该9GiB，明确把 `0x880000000`、`0x8B0000000`、`0x8B8100000` 三段留待验证。本轮Linux live DT仍列出它们（合计918MiB），另有低于4GiB的若干可用段。并非所有差额都能解释为必需保留。

当前XNU `arm_vm_outside_init` 仅接受 `base>=gDramBase && end<=gPhysBase` 的窗口外段，而且接管aperture覆盖的物理跨度最多4GiB。将低32位内存与35GiB附近高位段同时纳入，会遇到很大的物理地址空洞。因此不能只把9GiB改成12GiB或扩大DRAM边界就声称支持全部内存；需要独立验证多段映射、页描述符、UEFI/安全/驱动保留区及实际所有权。

## 上游的“利用全部内存”边界

本轮刷新reference/UniXNU-sheng的origin/main到 `c2d8230fbdef56780250c58a802ceb312108c4ae`，本地参考检出仍为cd73e23；新增提交为键盘修复和文档，未合并。已检查的低bank默认接管提交 `c73f9c0` 明确报告：额外458MiB，hw.memsize为6.37GiB，461秒运行记录。它证明该sheng设备/系统的低bank接管，不证明Uke 12GB全部可用。[上游提交](https://github.com/UniXNU/UniXNU-sheng/commit/c73f9c07131588288bc93a447d1770d854dbfbd6)

上游前一提交c7a0346曾记录接管低bank后部分启动出现连续分配失败/GPU未启动，以及GPU pool与音频固定缓冲重叠；后续恢复接管并排除音频缓冲。这个历史说明内存路径需要核验，不能移作本次冻结根因。Uke使用自己的高位段和gpu-pool=0，不能直接套用sheng的低地址保留常量。

## 冻结是否可能由内存引起

可能，但当前没有证明。应区分全局容量耗尽和内存管理错误：映射/缓存别名、保留区所有权、连续分配、释放后使用或锁等待，即使仍有大量空闲页也可能引发GPU出错或整机停住。

同次155.293秒有languageassetd按per-process-limit退出，当时 `memorystatus_available_pages=1287668`（约4.912GiB）；不是全局内存耗尽的证据，更不能代表219秒后冻结瞬间的空闲量。启动接管0 refused只证明当时这些页通过接管检查，不等于长期写入、并发映射或所有权绝对正确。122秒已恢复GPU超时、208秒文件节点服务SIGSEGV和最后219秒进度均须分别配对，不能仅凭Safari触发就把排查限定到GPU。

连续日志候选b8ffe6b0保持未刷，按用户要求暂缓。若后续继续采集，判读应同时覆盖VM/分配/回收/存储与GPU，不以“GPU高负载”作为预设根因。本轮不修改内存参数、不扩大DRAM登记范围。

原始证据在 `uke-port/fixes/2026-11-13-memory-capacity-audit/`：`meminfo.txt`、`iomem.txt`、`cmdline.txt`、`dt-memory.bin`、`packed-adt-memory.json`、`findings.json`、`sheng-fetch.log`。XNU计数来自此前同次UUID92E994D1回读，未把TWRP和XNU状态混用。
