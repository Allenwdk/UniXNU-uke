# uke GPU 驱动 vs sheng：完整差异对照（2026-10-29）

目标：sheng 的 GPU 在 SM8550/A740 上稳定 90–120 Hz；uke（SM7675/A732）目前 GPU 合成器可用但约
15–20 fps、偶发 1 s 卡顿和闪烁。本文对照 `reference/UniXNU-sheng` 与
`UniXNU-uke-format2` / `UniXNU-format2` / 各 fork，列出差异和改动顺序。

## 1. 相同部分（已对齐）

| 件 | uke | sheng | 状态 |
|---|---|---|---|
| GPU 内核驱动 | `Adreno740` 上游 `5c8c145`（A732 profile + pool adopt + BO release 修复在顶部） | 同一上游 Adreno740（catalog/feeds） | ✅ |
| GPU SMMU kit | `ArmSmmu4K` 上游 `3ba9245` | 同一上游 | ✅ |
| Metal 前端/插件 | `AdrenoMetal` 上游 `ce5e19e` + iOS17 shared-event 声明 | 同一上游 | ✅ |
| GPU 上下文 | GMU cold start、HFI、stage-1 SMMU、ring/CP、fences | 同 | ✅ |
| 显示直扫 | DPU stage-1 窗口 + VIG0/1 flip（uke 自有，D1b） | sheng 用 GPU scanout（`sheng-fb-gpu=1`） | 路线不同 |

## 2. 关键差异（按对 90–120 Hz 的影响排序）

| # | sheng 有 / uke 缺 | sheng 的做法 | uke 现状 | 影响 |
|---|---|---|---|---|
| D1 | **GPU BO pool 默认 1536 MiB** | `NativeAdrenoService` 从 `sheng-gpu-pool` 取 1536，bring-up 内 `reservePool`，BO 连续、span 少 | board 写死 `sheng-gpu-pool=0`，全部散页 BO | **span 容量 131072 被 34 MB b3a8 surface 耗尽 → `OutOfMemory(Device)`，动画回退/闪烁/低帧**。gpucomp24 已改 1536 |
| D2 | **GPU DCVS 调频** | `sheng-gpu-dcvs=1`：16 ms 窗口按 busy 调频；app 启动/触摸 boost 到顶档，空闲 80 ms 回 floor（348 MHz） | 无 DCVS，只有 boot 时一次固定 vote（当前 950 MHz/16.5 GB/s） | 固定顶档不省电但**不应降帧**；真正缺的是 **app 启动/动画的 boost 策略**（sheng 专门修过 30–36 ms 首帧） |
| D3 | **vsync 120** | `sheng-vsync=120`，显示驱动 `startVsync/vsyncTick/getRefreshAttributes` | 显示 fork 无 vsync 代码，CA 没有 120 Hz 的 display link | **没有 120 Hz 帧节奏**；SwapWait/动画按 60 或更低排帧 |
| D4 | **GPU scanout 呈现** | `sheng-fb-gpu=1`：插件 `GpuScanoutClient` 在 GPU 上 blit 到 GOP，完成回调 retire | uke 用 DPU 直扫 + 延迟一帧（无 fence） | sheng 有 GPU 完成同步；uke 靠延迟一帧，**闪烁/撕裂**来源 |
| D5 | **CPU 调频** | `sheng-cpufreq=4`（EPSS 域） | board 未设置 | 单核 CPU 下合成/提交线程可能低频，加剧 jank |
| D6 | **背光/按键/banner/UI focus** | `sheng-backlight=1 sheng-buttons=1 sheng-banner=0 boot-ui-focus4=SpringBoard` | uke 未设置 | 非帧率主因，但影响整机体验 |
| D7 | **BO pool 默认值** | 1536 MiB | 0（见 D1） | 同上 |
| D8 | **呈现路径的 fence** | GPU scanout completion | DPU 直扫无 fence，延迟一帧 | 闪烁/撕裂 |

## 3. 已确认不是差异的点

- GPU 提交/TLB：uke 已按上游 `5c8c145` 去掉每帧 `TLBIALL`。
- stall 快照显示 `FSR=0x400`（FSC=0，无真实 fault）、FAR 恒定无效值；**不是 BO 生命周期错误**，
  而是单帧过长触发上游 50 ms stall 阈值（invalidate/kick 又造成 0.1–0.16 s 卡顿）。
- 窗口大小：GPU 输入 aperture 是 **31 位（2 GiB）**，slot0 只能 1 GiB（`setScope` 拒绝更大窗口）；
  因此不能靠扩窗口解决 OOM，必须走 pool/连续 BO（D1）。

## 4. 处理顺序

1. **D1（已改 gpucomp24）**：`sheng-gpu-pool=1536`，让 BO 连续、span 数下降，消掉
   `OutOfMemory(Device)`；这是当前低帧/动画卡顿/闪烁的直接原因之一。
2. **D3 vsync 120**：从 sheng 的 `GopFramebuffer@33e941a`（或 reference/UniXNU-sheng 的显示实现）
   移植 `startVsync/vsyncTick/getRefreshAttributes`，board 加 `sheng-vsync=120`。
3. **D2 DCVS**：把 sheng `GpuPowerProbe.cpp` 的 `startGpuDcvs`（含 touch/wake boost）按 A732 的
   OPP 表移植；board 加 `sheng-gpu-dcvs=1`、`sheng-gpu-dcvs-min=3`。
4. **D4 GPU scanout**：评估切回 `sheng-fb-gpu=1`（插件 blit + completion）替代 DPU 直扫，
   或给 DPU 直扫加 GPU 完成 fence；解决闪烁/撕裂。
5. **D5 CPU 调频**：`sheng-cpufreq=4`（先确认 SM7675 EPSS 域与 SM8550 的一致性）。
6. **D8**：与 D4 一起做；DPU 直扫的 one-frame delay 在 fence 可用后去掉。

## 5. 判定

- D1：运行 10 min 无 `OutOfMemory(Device)`，动画不再回退；`GPUT9 surface cache` 不再异常增长。
- D3：`sheng-vsync` 行出现 120 Hz tick；render server SwapWait 按 8.3 ms。
- D2：`native_gpu_dcvs` 行显示 app 启动/触摸 boost 到顶档；帧时间下降。
- D4：`UKE DPU: direct` 不再需要延迟一帧，无闪烁/撕裂。

## 6. 2026-10-29 更新：pool 生效后仍闪烁

- gpucomp24（`sheng-gpu-pool=1536`）仍报 `OutOfMemory(Device)`：`NativeAdrenoService` 读 boot arg 在
  bring-up 之后，pool 实际未预留（日志 `gpu pool tables 0`）。gpucomp25 在 probe 的 `gpuStart()` 里、
  `NativeAdrenoGpuContextBringup` 之前设置 `native_adreno_pool_mib`，与 sheng 一致；用户确认**流畅度明显提升**。
- 剩余闪烁的日志证据：换帧 surface 在 `format=62336138`（b3a8，34 MB 双平面）时
  `presentable=0`，被 `presentBase` 直接跳过；只有 `format=42475241`（BGRA）走 DPU 直扫。
  模糊/特效会切到 b3a8，跳过的那一帧让屏幕停在上一个 BGRA 帧，视觉上就是闪烁。
- 插件的 `clearDeepNow()`（清 CA 的 deep 标志）在 device 创建时执行，但没有改变 CA 的格式选择；
  b3a8 仍出现。
- 下一步：**P2 GPU b3a8 解码**（metal2vulkan 已支持两平面纹理，给 `GpuScanoutPresenter` 加一个
  两平面→BGRA 的 render pass，或让 CA 只出 BGRA）；DPU 本身不能扫 b3a8。

## 7. P2 落地：b3a8 由 DPU 原生 10-bit 扫描（gpucomp26）

结论先：**不需要给 b3a8 加 GPU render pass**。原厂 DPU 驱动（reference 的
`Xiaomi_Kernel_OpenSource-uke/drivers/gpu/drm/msm/disp/dpu1`）本来就支持 10-bit RGB 线性格式，
而 b3a8 的 plane 0 就是一个 10-10-10-2 颜色平面，VIG 管线能直接取。

- **几何**（与 metal2vulkan `ctexture/surface_two_planes.rs` 的实测一致）：
  plane 0 = 4 B/texel（行距 `4*width = 12,800`），plane 1 = alpha 1 B/texel（行距 `width = 3,200`，
  也就是 surface record 的 `stride` 字段），alloc = `3200*2136*5 + 1024 = 34,177,024`。
  plane 0 单独 27,340,800 B，正好 6675 页。
- **DPU 寄存器**：firmware 留下的 VIG0/VIG1 是 `SRC_FORMAT=0x000237ff`、`UNPACK=0x03020001`
  （`boot-dpu-*` 快照）。按 stock `dpu_hw_sspp_setup_format` 的字段公式复算 `ARGB8888` 得到同一个
  `0x000237ff`，`XRGB2101010` 只差两位（bit8 alpha 清、bit14 DX 置）= `0x000276ff`，unpack 不变；
  只有 `XBGR2101010`（R 在低位）才需要 `unpack=0x03010002`。
- **实现**（`drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp`）：
  `dpuStart` 保存四组寄存器（SRC0/STRIDE/SRC_FORMAT/UNPACK），`dpuStop` 恢复；
  `dpuSlot` 改成映射“需要的前缀字节数”，因此 34 MB 的 b3a8 帧只映射 plane 0；
  `dpuPresentFrame(..., tenBit)` 每次 flip 同时写源地址、stride、源格式；BGRA 写回 firmware 原值；
  b3a8 写 `dpuTenBitFormat()`（从 firmware 原值推导）。`presentBase` 的 `presentable` 对双平面
  用 `5 B/texel` 判据，BGRA 与 b3a8 走同一条“延迟一帧 + DPU 直扫”路径。
  CPU 逐像素解码保留为 fallback（DPU 窗口不可用时）。
- **为什么不用 GPU pass**：插件侧 `GpuScanoutPresenter` 只能 import 裸 buffer，两平面纹理需要
  IOSurface 与其 plane 事实（`newTextureWithDescriptor:iosurface:plane:`）并且要有已编译的 AIR
  shader 做格式转换——本机 Linux 没有 Metal 前端编译器，这条路要拖入工具链依赖；DPU 方案零拷贝、
  零 GPU 时间，且完全落在原厂 DPU 驱动的能力范围内。
- **风险与回退**：若固件不是 ARGB8888 字序（`UNPACK != 0x03020001`），`dpuPresentFrame` 拒绝 10-bit
  （`UKE DPU: tenbit refused`）并回到 CPU 解码；`dpuStop` 恢复全部四组寄存器。
  颜色若 R/B 反，只需把 `dpuTenBitWordOrder()` 的期望换成 `0x03010002` 并写 `XBGR2101010`。
