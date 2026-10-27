# 从 recovery 读取 XNU 日志（userdata 窗口）

适用于 uke（Xiaomi Pad 7）。XNU 里没有 adb/fastboot，测试结束后必须由人按键进 fastboot；日志由 `UkeLastLog` 每 2 s 写到 userdata 最后 32 MiB 的环形窗口，在 TWRP 下用 dd 读出。

## 前提

- 内核带 `sheng-log=1`（board.toml 已含），且 UFS root layout 已就绪。启动日志里应出现：
  `UkeLastLog: userdata log window at lba <abs>, 8192 blocks`
- 读日志必须进 TWRP：先刷回原厂 boot/vendor_boot（XNU 镜像起不了 recovery）。

## 步骤

1. 设备在 fastboot：

   ```bash
   fastboot flash boot_a /home/allenwdk/UniXNU/uke-images/boot.img
   fastboot flash vendor_boot_a /home/allenwdk/UniXNU/uke-images/vendor_boot.img
   fastboot reboot recovery
   ```

2. 最多等 10 s，然后重启 adb server（TWRP 的 adb 经常这时才枚举）：

   ```bash
   sleep 10; adb kill-server; adb start-server; adb devices -l
   ```

3. 按 live GPT 计算窗口偏移。不要用 board.toml 的 `last_lba`：实际分区比它大 7168 个 4K 块。

   ```bash
   adb shell 'size=$(blockdev --getsz /dev/block/by-name/userdata); \
              skip=$((size/8-8192)); \
              echo size=$size skip=$skip; \
              dd if=/dev/block/by-name/userdata bs=4096 skip=$skip count=8192 of=/tmp/ukelog.bin'
   adb pull /tmp/ukelog.bin /home/allenwdk/UniXNU/ukelog-new.bin
   ```

   本机当前值：`size=471908312`（512 B 扇区）→ `skip=58980347`，窗口绝对 LBA 61941755。

4. 解码（在 `UniXNU-uke` 仓库）：

   ```bash
   cd /home/allenwdk/UniXNU/UniXNU-uke
   python3 tools/decode-ukelog.py /home/allenwdk/UniXNU/ukelog-new.bin -o /home/allenwdk/UniXNU/boot-new.log
   awk '!seen[$0]++' /home/allenwdk/UniXNU/boot-new.log > /home/allenwdk/UniXNU/boot-new.unique.log
   ```

   解码器校验 magic/CRC、按 seq 排序，输出 `N valid records, sequence 0..N-1`。

   GPU addon 镜像启动后，用同一个日志跑一次自动核对：

   ```bash
   python3 scripts/check-gpu-addon-boot.py /home/allenwdk/UniXNU/boot-new.log
   ```

   它取最后一次出现的 `UKE GPU addon:`/`UKE GPU3: M`/`UKE GPU4: K`/`UKE GPU7/8` 行，逐项判
   addon stage、两个视图（`a5.0/5.0`）、`o>0`、插件 marker 与 68f1/68f2/68f3 失败原因，自动区分
   probe/stable，并以退出码 1 表示有必须修复的项。

## 记录格式

每条 16 KiB：

```
[ 0] "UKELLOG1"
[ 8] seq (u64 LE)
[16] payload length (u32)
[20] payload crc32 (u32)
[32] msgbuf 尾部
```

每 2 s 一条，窗口 2048 条，约保留最近 68 分钟。

## 必须注意：窗口里可能混着上一次启动

写指针每次启动都从 slot 0、seq 0 开始。如果本次写的条数少于上次，slot N 之后仍是上次的记录，magic/CRC/seq 都连续，解码器会把两次启动拼在一起。只取本次启动：

```python
a = open('/home/allenwdk/UniXNU/ukelog-new.bin', 'rb').read()
b = open('/home/allenwdk/UniXNU/ukelog-old.bin', 'rb').read()   # 上一次的 dump，保留一份
for i in range(len(a) // 16384):
    if a[i*16384:(i+1)*16384] == b[i*16384:(i+1)*16384]:
        print('current boot records:', i)
        break
```

然后 `head -c $((N*16384)) ukelog-new.bin > ukelog-current.bin` 再解码。没有旧 dump 时，只有本次启动写满 211 条以上才不会混。

## 已知坑

- 解码输出按 16 KiB 全量拼接，行大量重复；先 `awk '!seen[$0]++'` 去重再读。msgbuf 环形回绕会把行撕裂（前后缀串行），属正常。
- `fastboot reboot recovery` 后 `adb wait-for-device` 常超时；按第 2 步重启 server。必要时重新插拔 USB。
- 在 XNU 里没有 adb；测试后长按电源关机，再按住电源 + 音量减进 fastboot。
- 窗口是 userdata 最后 8192 个 4K 块；`blockdev --getsz` 的单位是 512 B 扇区，所以要 `/8`。
- 原厂 `boot.img` / `vendor_boot.img` 在 `/home/allenwdk/UniXNU/uke-images/`。刷过 XNU 镜像后必须换回，否则 TWRP 起不来。
- 读不到记录时，先 grep 内核日志里的 `UkeLastLog:`，写入端会打印拒绝原因（`off` / `no root layout` 等）。

## 崩溃探针（sheng-crashlog=1）

`package/CrashLogProbe.c` 由 UkeLastLog 的 startup 拉起，把用户态崩溃报告镜像进 msgbuf（因此也进上面的 userdata 窗口）：

- 首次扫描（约 60 s）：列 `CrashReporter` 条目、`/Applications`、stat `Preferences.app`、枚举 `/private/var/containers/Bundle/Application` 找 `Preferences.app`/`MobileTimer.app`，然后分时打印 `backboardd-`、`SpringBoard-`、最新 `Preferences-` 报告（8 KiB 头 + 完整报告窗口/符号）；
- 之后每 90 s 列一次目录，并按文件名哈希打印新出现的 `.ips`（不要用 mtime——设备时钟在 1970 年，跨启动不可比）；
- **复现崩溃后至少等 3 分钟再复位**，否则下一次扫描来不及落盘；
- 日志里 grep `UkeCrash:`（`newest Preferences`、`window off=`、`sym`、`installed settings/clock`、`containers=`）。

注意 26 MB 级别的 CoreAnimation 缓冲映射失败不经过 `mach_vm_allocate_kernel`；`UKEVMA:` 行只反映用户 task 的小块固定地址分配。

## GPU addon（sheng-gpu-addon=1）

带 addon 的内核在 Data 挂载后的第一次 exec 解压内嵌 blob，写 `/private/var/ngpu/{addons,bin,daemons}`，然后执行原有的 bindfs 挂载。日志读法：

- `UKE GPU addon: stage=3 files=7 bytes=39398839 error=0`：首次写入成功；cookie 命中时安装函数直接返回、**不再打印这一行**（没有这一行但 `M a5.0/5.0` 就表示缓存命中），`stage=2` 表示仍在写，`-1` 表示失败（`error=` 是 errno）。首次写入约 39 MB，可能比旧镜像多几秒到几十秒，未完成前不要复位。
- 心跳第 6 行 `M a<addon>.<status>/<addon2>.<status> …`：`a5.0` = Extensions 视图已挂载且 `NativeAdrenoMetal.bundle/Info.plist` 可见；`a5.0/5.0` = LaunchDaemons 视图也可见；`o<opens>/<busy>/<closes>` 的 opens > 0 表示插件或 daemon 已打开 AGX user client。
- 第 7 行 `K<total> <markers>`：插件的 0x4147xxxx 标记（6800 进入插件、6801 找到 Turnip、6802 设备创建、68f1 无 Turnip、68f2 无设备、3xxx 成功步骤、4xxx 失败状态、5xxx 已上报）。
- GPU7 的 `R …` 是 raw user test 报告，`T …`/`W …` 是 Turnip 阶段行；`D …` 是打开/属性读取计数；第 6 行末尾 `d<ever><alive>:<pid>` 是 `adrenoprobed` 进程。
- 若 `M a1.…`：源目录不存在（写入失败或路径错）；`a2/3/4`：bindfs 挂载失败或视图不完整；对照 `error=` 和 `M` 行。
- 授权路径：`S` 行的 MACF 是驱动的只读探测，仍可能显示 `backboarddP1`；真正的 open 由内核补丁的 client/service grant 放行。`o<opens>` > 0、插件 `K…` 和 GPU7 `R/T` 才是授权成功的证据。`o` 一直 0 且 `M a5.0` 时检查加速器上的 `NativeSandboxGrantProcess`/`NativeSandboxGrantService`。
- `sheng-iconservices=1` 把 `com.apple.iconservices.iconservicesagent` 从 `lite_jobs` 的禁用名单里去掉：这次启动应在日志里看到该进程（`proc`/CrashReporter 里不再每轮出现它的 `CI::GLContext` 崩溃），图标/设置页应正常。若 addon 失败，它会回到崩溃循环（系统仍存活）；去掉该参数即恢复禁用。
- addon2 修复了首版补丁的 scratch 冲突：bindfs 挂载参数原先在 `scratch+0x400/+0x600`，会覆盖 cache 阶段留给 launchd 的环境串/向量（+0x400/+0x600，向量 +0x500），init exec 会把 `/private...` 当 envp；现在两处挂载都用 `+0x800`（mkdir 仍 +0xc00）。addon3 又修复了内核 zlib 的分配器：本内核以 `KERNEL` 编译、强制 `NO_ZCFUNCS`，`inflateInit` 不装默认 `zalloc/zfree`，旧 blob 补丁会在首启 `native_owner_data_prepare` 里空指针调用；addon3 自带 `kalloc_data`/`kfree_data` 分配器。addon4 另把 GPU7/GPU8 宽行（插件 R/T）变更写入日志。旧 addon1–addon3 镜像不要再刷。
- 探针镜像（tag `addon5p`）：blob 不含 `NativeAdrenoMetal.publish`，插件只跑 stage-1 用户态测试并返回 nil，软件渲染保持不变。建议先刷 `vendor_boot_uke_addon5_probe.img` 验证 `UKE GPU addon:`、`M a5.0/5.0`、`o>0`、`K6…` 与 daemon，通过后再刷发布版 `vendor_boot_uke_addon5.img`，看 `K68xx`/`R`/`T` 与 backboardd 的原生设备。
- 若 `M a5.0`、`o>0`、`K68xx` 表明原生设备已注册但画面黑/停留旧帧，换 `vendor_boot_uke_addon5_fbgpu.img`（tag `addon5f`，`sheng-fb-gpu=1`：帧交给插件的 scanout client 做 GPU 拷贝，offer 被拒时自动回退 CPU 拷贝）。
- addon5 的 clients 名单补上 `WidgetRenderer_Default`（驱动 int-196/200：缺它时小组块白块），与 `iconservicesagent`、`PosterBoard` 一并允许；三个共享 GPU 窗口之外的进程得到 busy 并回退软件渲染。
- panic 捕获（addon6fix）：`panic()` 现在把 `UKE PANIC: <标题>` 写进 PRAM（panic 上下文里 UFS 写不可用），下一次启动由 UkeLastLog 在 tick 4 打进 msgbuf→userdata；抓取时 grep `UKE PANIC:`。addon6fix 镜像（kernel `ab7bfafa…`/payload `fc969bfb…`/vendor_boot `81b19cdc…`，995 B）已构建校验，未刷。
- 日志行号：屏幕第 5/6/7 行在 decode 出的日志里分别是 `UKE GPU2:`/`UKE GPU3:`/`UKE GPU4:`（`setRow` 打印 slot-3）；宽行是 `UKE GPU7:`（`R` 报告）与 `UKE GPU8:`（`T`/`W`）。取证：`grep -aE "UKE GPU addon:|UKE GPU[0-9]+: (M|K|S|R|T|W)|UKE: display surface|UKE: surface mapping|UKeCrash:" boot.log`。
- 刷完 addon5 后直接跑 `scripts/check-gpu-addon-boot.py <log>`：它把上面这些判据一次跑完（`--mode probe` 要求探针的 `K` 行非空；`--mode stable` 另外拒绝 68f1/68f2/68f3）。

刷机（两种 UEFI 启动镜像都可配 200 GiB packed Data seed）：`fastboot flash boot boot_uke_usbdal2.img`、`fastboot flash vendor_boot vendor_boot_uke_addon5.img`（CA 映射实验用 `vendor_boot_uke_addon5_map2.img`；iconservices 兜底用 `vendor_boot_uke_addon5_noicon.img`；画面黑/旧帧用 `vendor_boot_uke_addon5_fbgpu.img`）、`fastboot set_active a`、`fastboot reboot`。
