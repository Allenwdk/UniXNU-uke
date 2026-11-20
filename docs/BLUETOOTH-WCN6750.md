# WCN6750 Bluetooth on Uke

The 24A446 candidate uses QcaBluetooth over QUP1 SE2 and Apple's Bluetooth
stack over UART.SK. QUP1 uses GIC INTID 616 and Apps SMMU SID 0x423. The board
wires CTS, RTS, TX and RX to GPIO 40–43, BT_EN to GPIO 156 and SW_CTRL to GPIO
80. GPIO 43 provides the falling-edge UART wake. Firmware setup switches the
UART from 115200 to 3 Mbaud. The firmware manifest pins the original Uke BTFM
archive and the WCN6750 patch and NVM files.

The Bluetooth and USB rail transactions share the existing Apps RSC TCS.
Bluetooth releases its own radio rails after shutting down the UART. WLAN
retains S1B and S3B while WPSS is active. UFS and USB/audio retain the shared
platform supplies S2B and L7B. A transaction with uncertain completion is
left for the next reboot.

The device builder overlays the signed original-daemon adaptation, BlueTool,
the Type0 library and firmware, and merges the BlueTool settings into the
accepted Wi-Fi cache. The candidate keeps the existing Wi-Fi service and Data
seed. The device default is off. The paired package turns it on with
`bluetooth=1`. The ADT defaults to `log-bluetooth=0`. A boot with
`log-bluetooth=2` enables the GENI loopback and detailed startup diagnostics.

The controller fixtures passed MBN and TLV initialization, reinitialization
after timeout, GENI DMA and the existing WCN7850 regression. The upstream
native adapter passed 165 instruction cases and 58 HCI and policy cases.
Apple libxpc and codesign verified the merged launchd cache. The candidate
`vendor_boot.img` SHA-256 is
`d5da3bd89bb686638061a7b1f693ad4abfd56d6b336b0d0033e00b27d68cedf6`.

The first tablet run reached Settings with a grey Bluetooth toggle.
The runtime libraries and executables mounted successfully. The first QCA
version request timed out at 115200 baud, before firmware download or UART.SK
registration. The follow-up starts the UART before asserting BT_EN and
reopens it after the 50 ms settle, as in the OEM Linux hci_qca driver. It
logs DMA and HCI startup details. The second tablet run passed the sixteen-byte
internal loopback but stalled the external TX command with CTS sampled high
and no TX DMA completion. The next candidate reads GENI IOS at its OEM
offset, masks the TX FIFO word count, and temporarily ignores deasserted CTS
for the ROM query while leaving RX ready. Normal CTS handling resumes after
the baud switch. The third tablet run completed all five TX bytes but
received no reply. GPIO156's APSS output latch was high while the pad sampled
low. The TLMM control value showed eGPIO present and APSS ownership clear.
The next candidate acquires APSS ownership before asserting BT_EN, following
OEM pinctrl. The fourth tablet run confirmed the expected BT_EN and CTS
levels and completed all five TX bytes, with no RX. The stock HAL uses the
same version request. The next candidate derives the 115200 baud clock
directly from the 19.2 MHz crystal and logs a timed 128-byte internal
loopback. The crystal-clock run matched all 128 bytes in about 11.55 ms and
still received no ROM response. The stock Recovery driver answered the same
version command twice, including a capture with a held UART power vote.
The follow-up powers the radio before starting GENI RX and enables the same
break-byte filter as the OEM UART. The WCN6750 host fixture now uses the
observed ROM reply. The cold-start tablet run downloaded the MBN and NVM,
reported firmware ready at 3 Mbaud, and registered UART.SK. Settings still
showed a grey toggle. The deployed lite-mode startup policy included
`com.apple.bluetoothd` in its persistent disabled list. The follow-up skips
that automatic entry with `bluetooth=1` and records the launch result for
the native Bluetooth jobs. The owner tested image
`9c012ce8e0684f0c3a28ba9a23d5a7faaf93febd61f978bfe3546ab5c10748c5` and
confirmed Bluetooth enable, accessory discovery, pairing and audio output.
In the matched boot, both native jobs passed launch constraints on their
first observed launch. The 3 Mbaud UART.SK channel and Bluetooth AudioPipe
opened successfully. No Bluetooth timeout, UART failure, repeated process
launch or transport disconnect was observed in the retained window.

The quiet candidate `9aa660f3a7746a947d45527643cf7ff378027c0b5791fc7ac010f8a08f2c97fe`
uses the same kernel and runtime with the diagnostic level off. It awaits
tablet testing.

Track reconnect after reboot, repeated power toggles, classic and LE
accessories separately, AirPods controls, HFP/SCO, system suspend and
sustained Wi-Fi coexistence as separate results.

- [QcaBluetooth source and host checks](https://github.com/Allenwdk/QcaBluetooth/tree/a84d7d3117af0e8d723ce7350d247fbec3939a25)
- [Apple launchd cache build](https://github.com/Allenwdk/UniXNU-uke/actions/runs/44644572925)
