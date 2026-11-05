# WCN6750 Wi-Fi development

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
