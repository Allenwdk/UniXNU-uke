# WCN6750 Wi-Fi development

## 2026-11-08 当前验收结论（覆盖下方旧候选阶段）

V4.9.7 **680B9BF31C100A14**，vendor_boot SHA `5ac7fbff02795a8049502237f9e5ecfc6cbb362ec9d467dfd0ac667019a53ce1`，已刷已回读并通过本轮基础功能：设置Wi-Fi行、开放/输入密码RSN-CCMP网络、DHCP、应用/Safari流量。用户确认“设置出现wifi行，连接也没啥问题，第二个加入的是输入密码的网络”。原MG wi-fi valid1/true，Bluetooth false不伪造。

网关ARP阶段36.683ms（前版60.339s），DHCP ACK→发布1.699810s；第二地址192.168.1.51，密钥/授权/DHCP闭环。192344新环932条无重复seq、raw SHA6cf2915d…；同boot一次GPU自动恢复status0/dead0独立待归因。TLS日期复测、错密码/关开/冷启动/唤醒/长期压力、原UI停住根因另保留。

设备因取证当前原厂boot/vendor_boot A+B+A槽+TWRP，未恢复移植镜像，未写userdata。后续baseline为192344-wifi-v49-7-readback.bin。根docs入口：[总结](../../docs/status/wifi-wcn6750-summary-2026-11-08.md)、[本次回读](../../docs/status/wifi-v49-7-readback-2026-11-08.md)、[当前交接](../../docs/status/wifi-wcn6750-handoff-2026-11-08.md)。本轮仅日志和总结，无新实现或候选、未提交推送。

## 下方为逐版历史记录


2026-11-08 **V4.9.7修复候选已就绪未刷，CRC 680B9BF31C100A14**。仅补WifiNet原KPI的BPF RX/TX包tap，以及当前format2 board.toml的/chosen/mac-address-wifi0（匹配配置en0的020000756750）；不改原MG/Preferences布尔或显示逻辑。正式kernel/payload、原函数包边界ASan/UBSan、最终ADT与编译BPF调用、45SHA/13区通过。原AMFI/trust/签名/PAC/两卷认证与SystemOS修复、用户态/GPU addon/Data seed、999B诊断参数保持。设置行和延迟改善待真机；HTTPS日期复测/蓝牙不在本版范围。见 [V4.9.7](wifi-v49-7-bpf-capability-2026-11-08.md)。

2026-11-08 **V4.9.6用户确认应用与Safari能联网，仍约一分钟后才就绪，设置缺Wi-Fi。** 184318新环1000条，无重复seq；WebKit三类原AMFI均通过、同boot流量稳定。用户补充未校准日期、HTTPS本轮先不修；另缺蓝牙但蓝牙尚未bring-up，不伪造能力。原MG快照wi-fi valid1/false；configd网关BPF阶段60.339秒且无线已收到reply，源码缺BPF包tap。原卡死本轮未报告，不能宣布根因已修。设备原厂四分区/A槽/TWRP，未写userdata。见 [V4.9.6回读](wifi-v49-6-readback-2026-11-08.md)。


2026-11-08 **V4.9.6诊断候选已就绪未刷**：仅payload参数换成sheng-diag=1 + sheng-gpu-trace=1（移除mode4/log-wifi），999 B；原内核逐字节与V4.9.5相同，CRC仍 **ADDDAFDE557D876C**，必须用vendor_boot **f390cfb04fba98c63fb8b72dd70af6edcd7f030969d07c2de3d9f3791250b6c6**区分。正式payload、35SHA/13区通过，无运行源码改动。启用已有线程/IPC/GPU环，现成较晚导出阈值240 worker秒；新测试若画面停住，至少再等2分钟进fastboot。设备原厂四分区/A槽/TWRP，未写userdata。baseline181901 V4.9.5；本版不是功能修复。见 [V4.9.6](wifi-v49-6-ui-freeze-trace-2026-11-08.md)。

2026-11-08 **V4.9.5回读：打开Safari后全画面停住，触控/回桌面均无反应（用户确认）。** 495条新记录、无重复seq；ROOTFS v495 status0，首个WebContent原AMFI error0，局部启动阻塞已修；239.999232秒最后落盘合成命令完成，此后无新帧，内核/触摸仍记录到约298秒，无panic/GPU recovery。自动快照200.756845秒早于Safari232.908362秒，未覆盖现场；相关IPS与上轮完整快照实际新增0。根因仍未知，不改JIT/AMFI/资源限额。原V4.9.5已DO_NOT_FLASH。见 [V4.9.5回读](wifi-v49-5-readback-2026-11-08.md)。


2026-11-08 **V4.9.5已正式构建留档未刷：CRC ADDDAFDE557D876C**。V4.9.4已证外部TCP双向，但WebKit实际SystemOS APFS根挂载缺MNT_ROOTFS、原AMFI c1拒绝；本版仅在原两卷认证/boot manifest、精确UUID/device、只读APFS、真实rootvnode与union拓扑均成立后补该实际根位。原AMFI/trust/签名/PAC和卷认证返回保持，Data/bindfs不获身份。正式kernel/payload、13区、38SHA、生产helper正例+21负例ASan/UBSan通过。Safari/设置收益未验；设备原厂四分区/A槽/TWRP，未写userdata。见 [V4.9.5](wifi-v49-5-systemos-rootfs-2026-11-08.md)。


2026-11-08 **V4.9.4 已就绪未刷：CRC 0D545D3AB293579C**。按用户指定核对，原构建确实缺上游ee54dcd；已原样补入root union statfs64 fsid修复，并扩展WebKit/Safari/Preferences/IconServices启动事实取证。该上游修复针对BaseBoard系统分区判断/通知，不直接改变AMFI c1的内核卷条件。正式kernel/payload、13区、39项SHA通过；Wi-Fi/GPU native源码、用户态/trust/Data seed与V4.9.3一致，内核mount flags和原AMFI返回保持。实际fsid分支已见于编译代码。外网/设置收益待真机；设备原厂四分区/A槽/TWRP，未写userdata。见 [V4.9.4](wifi-v49-4-launch-trace-2026-11-08.md)。


2026-11-08 V4.9.3实机开放网络关联、DHCP与网关ARP成立；外网/WPA2/设置未验收。WebKit c1拒绝及Settings归属更正见根docs/status/wifi-v49-3-readback-2026-11-08.md；本轮扩展launch facts观察实际卷身份。


2026-11-08T16:26:27.150367+08:00：V4.9.3 CRC653F7447CAFF934D 已按当次“刷吧”授权部署；40项SHA/CRC通过，boot_a/vendor_boot_a/boot_b/vendor_boot_b Sending与Writing全OKAY，set_active a/current-slot a/reboot均成功，未写userdata。设备已执行重启，屏幕启动、稳定INFO、关联/DHCP/双向流量及设置Wi-Fi行待用户测试。证据 uke-port/releases/2026-11-08-wifi-v49-3-worker-poll/flash-result-20261108-162617.json。


2026-11-08 **V4.9.3 修复候选已正式构建留档、未刷**：按 sheng 补齐板级 `pollGl → nw_wifi_poll`，使嵌套固件等待也能排入 control/data work；补 sheng 式 debug 过滤/去重并保留控制链取证。一次聚焦 ASan/UBSan 夹具对照：缺 poll 时请求超过 3000 ms 未分发，修正后模拟 INFO 95 ms 回复，持锁门禁/MSI/GLINK/重复 error 均保持。CRC **653F7447CAFF934D**，vendor_boot `60d6815c11269593…`，kernel-final `f61a2beafa304db0…`；正式 kernel/payload、13 区、一次 UEFI 压缩往返通过。联网与设置入口仍未验收；设备最后已知原厂四分区/A 槽/TWRP，本轮未操作设备。下一步按当次授权部署后先验稳定 INFO，再开放 AP join/DHCP/双向流量。见 [V4.9.3](../../docs/status/wifi-v49-3-worker-poll-2026-11-08.md)。


## B883 MMIO readback and stock board-data transport

B883 returned 0x170acce5 from both REO1_MISC and CE0_DST_CTRL.
This does not establish register access. The follow-up streams the
pinned original Xiaomi REGDB and CN O82 ELF BDF from the read-only
modem FAT volume. It compares both length and SHA256 before sending,
then follows ath11k's segmented QMI 0x25 protocol. Only a matching
success advances the transfer. Read-only MMIO snapshots are taken
before transfer, after acceptance and after firmware init-done.
The peer check uses synthetic bytes at the original file lengths.

## 82C2 device identity and AHB window probe

The device accepted IND_REGISTER and HOST_CAP. CAP returned chip 1,
family 0xb, SoC 0x40140120 and firmware 0x30088282. Its validity mask
0x1d excludes board ID. DEVICE_INFO returned 0x61e00000, length 0x200000.
There are 128 fresh slots relative to the 49A7 readback.

The next image accepts only that BAR tuple for its diagnostic and reads
REO1_MISC and CE0_DST_CTRL using the original ath11k static window
offsets. The China O82 board data candidate is bd_o82.elf, based on
the OEM module's board-ID fallback and product/region dispatch, and
the recovery identity values. Its firmware acceptance is pending.

## 49A7 device readback

The fresh log captures CAP request `0x24` transaction 1 to node 13 port 8
and the matching response with result 1 and error 90, INCOMPATIBLE_STATE.
The WLFW service answers. The follow-up uses the original ath11k WCN6750
fixed-memory server-arrival order: IND_REGISTER, HOST_CAP, CAP and
DEVICE_INFO. Every transition requires an accepted reply. The peer fixture
checks the two new requests against their expected wire fields.

## WLFW identity queries

The 983B hardware capture records WPSS ready and handover at 52.611328 s,
IPCRTR up at 52.611909 s and service `45/13/8/301` at 52.612649 s.
There are 129 new valid slots compared with the D774 readback.

The next kernel sends read-only CAP and DEVICE_INFO requests over that
link. It builds the pinned Linux codec and exact ath11k descriptors,
validates endpoint, transaction and TLV lengths, and logs the board ID and
register-window address. The peer fixture exercises success, firmware
errors, malformed replies and timeouts. Its values are fixture inputs.
Hardware replies are pending. Stock BDF selection and MMIO attachment
depend on the device reply.

The original ath11k core and AHB compile into 21 arm64e objects.
The full backend still requires board platform, DMA and MSI operations.
The current kernel archive contains the identity codec.

User update after the 983B deployment: no panic was observed. This does not
yet establish WPSS ready, WLFW discovery or networking. The user authorizes
recovery transitions for log collection in this ongoing Wi-Fi task without
asking again. The post-flash handoff remains immediate: give the next user
action after reboot and do not poll. Readback evidence is collected under
`uke-port/fixes/2026-11-06-wifi-wpss-mapped-stores-readback/` in the workspace.

Repair candidate `983B83D46E8BAFB5`, source
`0e124681cdcfe3687bd068089aaa7a34e40e0e88`: formal kernel/payload and the
focused firmware memory check pass. The final `loadSegments` contains 253
instructions with no DC ZVA or direct generic-memory calls. GPU runtime
inputs and missing/stubbed sets remain unchanged. Release:
`uke-port/releases/2026-11-06-wifi-wpss-mapped-stores/`. Not flashed; WPSS
startup and wireless operation remain unverified.

## D774 failure and mapped-store repair

D774 is withdrawn (`DO_NOT_FLASH`). The user's panic photo resolves in the
exact shipped image to `bzero+0x48`, instruction `DC ZVA,X3`, called by
`loadSegments+0x208`. ESR is `0x96000061`. The recovered ring has only one
new valid slot and ends at 52.600944 s, before any Wi-Fi-stage line was saved.
It does not independently establish WPSS progress.

The repair preserves the firmware mapping and PAS authentication path.
`UkeWpssMemory.h` replaces generic destination memcpy/bzero with volatile
aligned 32-bit writes and byte heads/tails, retaining the loader's barrier.
The focused firmware check now copies all original segment data, zeros BSS
and verifies untouched gaps over a 25 MiB reservation bounded by guard
pages. Final ARM64 code must also be checked for the absence of DC ZVA and
generic memory calls in the loader. Hardware acceptance remains pending.

Deployment, 2026-11-06 02:03:25–02:03:33 +08:00: candidate CRC
`D774B827AA0C2341` was flashed under the user's current authorization.
All four boot/vendor_boot writes, slot-a readback and reboot succeeded;
14 hashes passed and no userdata write occurred. Actual boot, WPSS readiness
and WLFW discovery remain unverified. Evidence:
`uke-port/releases/2026-11-06-wifi-wpss/flash-result-20261106-020325.json`
in the workspace. This is transport/deployment evidence only.

Development branch: `feat/uke-wifi-20261106`, based on the latest local GPU
sources, including the pipeline journal and diagnostic export repairs.
Workspace status and the full staged plan live in
`/home/allenwdk/UniXNU/docs/status/wifi-wcn6750-port-2026-11-06.md`.

## WPSS control transport

`package/WifiProbe.cpp` adapts sheng's remote-processor worker at
`460179565a018c741b93e0b1efa07c55d4de497a`. The unchanged MDT, SMEM, SMP2P,
GLINK, QMP, RPMh, QRTR and FAT cores are in
`Sm7675Platform/remoteproc`, with source hashes in `upstream.json`.
`AdspFirmware.c` retains the upstream read-only UFS/FAT firmware interface;
its allocations are explicitly checked and initialized.

The worker reads `modem:qca6750/wpss` from the original firmware partition.
It validates all segments against the 25 MiB WPSS reservation, preserves
the signed metadata, and invokes PAS init/memory-setup/authentication for
peripheral 6. Authentication errors stop startup. No firmware binary is
embedded or committed.

Uke inputs: IPCC `0x406000`, remote client 24, remote PID 13, SMP2P items
617/616, crash reason 626, WPSS `0x9bb00000 + 0x1900000`, and GLINK items
478/479/480. `WifiQrtr.c` registers IPCRTR before WPSS starts and records
WLFW service `0x45`, instance 3. These are source inputs, not device results.

The current branch defaults to startup mode 2 so no bytes need to be added
to the existing 999-byte command line. An explicit `uke-wifi=0` disables
it, `uke-wifi=1` surveys only, and `uke-wifi=2` starts the control transport.
To add an override to a payload, first keep the complete command line below
the board's 1000-byte limit.

CX/MX are shared resources: this diagnostic requests their highest cmd-db
levels and retains those votes and XO through handover. Do not clear the
AP's shared votes at WPSS handover. Shared vote ownership and runtime power
management remain implementation work. TCS1 and its interrupt must be free;
startup refuses existing ownership and times out failed requests.

## Build and evidence

```sh
cd /home/allenwdk/UniXNU/UniXNU-build
./unixnu build kernel
./unixnu build payload
python3 scripts/porting-record.py check --device ../UniXNU-uke-build
```

The payload build needs the existing EDK II `TianoCompress` directory on
PATH, recorded in the workspace environment index.

The one focused host check uses the imported runtime parser on the actual
stock pieces and compares its authentication metadata byte-for-byte with
the stock MDT, also checking all file hashes and reserved-memory bounds:

```sh
python3 ../UniXNU-uke-build/scripts/check-uke-wifi-firmware.py \
  --protocol-dir ../UniXNU-sm7675-common/kits/Sm7675Platform/remoteproc
```

Private copies are in `.cache/firmware/uke-wifi/qca6750/`; the 78 file hashes
are in `firmware/wcn6750/manifest.json`. BDF selection still needs the real
target's board identity. The first firmware is ELF32 with 11 load segments;
the parser supports both ELF32 and ELF64 regardless of the Linux descriptor.

Device acceptance requires fresh `UKE WIFI:` stages, WPSS ready, GLINK
negotiation, IPCRTR link, then a WLFW service announcement. A formal build
does not establish those results. The ath11k hardware backend, DMA/MSI,
scan/association, WifiNet, supplicant and DHCP are not yet integrated. No
network interface is published by this first control-transport candidate.


## 2026-11-08 V4.6 readback and V4.7 owner launch role

V4.6 firmware/scan/en0/wifid run, but AMFI c8 still blocks the native supplicant. Seventy unique PIDs were denied, zero real control clients attached and two open joins timed out after30s. Preferences has a crash record; lack of framework logs does not prove its Wi-Fi extension was never loaded. Device remains stock boot/vendor_boot A+B, slot A, TWRP; no userdata write.

Pinned24A446 binary analysis identifies the system-service fallback as built-in category6 with constraint identifier8. V2 trust capability is7 (bit2 set even for category0). Category5 is accepted by the original service manager and requires platform validation plus the original init/mbfloagent parent policy. V4.7 changes only the owner supplicant entry metadata02000000->02000500, keeping v2, the cache entry0 and all four prior modules unchanged. Hardware result pending. Canonical workspace report: ../../docs/status/wifi-amfi-launch-role-2026-11-08.md; evidence: ../../uke-port/fixes/2026-11-08-wifi-amfi-policy-proof/launch-role-proof.json.


## 2026-11-08 handoff refresh after V4.8 deployment

Canonical current handoff: ../../docs/status/wifi-wcn6750-handoff-2026-11-08.md. V4.8 CRC977C5379420FA182 was flashed to all four boot/vendor_boot slots, selectedA and rebooted at06:51:46+08:00; no post-reboot user observation/readback yet. V4.7 passed AMFI category5 but has85 control sessions and81 persisted INFO timeouts. Current next action is V4.8 RX/worker/TX evidence against the V4.7 063724 ring, not restarting P0/P1 or treating the old TWRP capture state as current. No device action occurred during this documentation update.
