# Uke iPadOS 27.0.1 sleep and swap

The default `uke_ipados27_defconfig` includes the owner Data swap backend. The 24A446 patch series places the swap file under `/private/var/vm` on writable owner Data and boosts demand swap-in I/O under the native UFS root policy.

The board ADT defaults enable `platform-sleep=1` and `platform-sleep-wake-after=60`. `sleep-disabled=1` gates XNU sleep until the Uke wake source is ready. `log-sys=2` enables the 30-second CPU and VM sampler used in the recorded run.

The retained `2026-11-22-uke-power-key-sleep-performance` readback used the power-key display-wake image with device commit `485499268aba3362469e7baa5e5ee8bd15796bb0`.

| Time asleep | Wake source | Display |
| --- | --- | --- |
| 32.681 s | Power key, INTID 513 | Lit after resume |
| 60.000 s | Timer, INTID 27 | Stayed off, then slept again |
| 50.443 s | Power key, INTID 513 | Lit after resume |

Six samples in that readback reported 128 MiB allocated swap, zero swap used and zero swap-in or swap-out. Demand I/O under memory pressure remains unverified.

With device commit `2656b1355bce153c9b28de7aa0c4b98adccd5e7d`, the user confirmed the lock screen went dark after about five seconds. The vendor_boot SHA-256 for that image is `40b41fc7d1666b99d0580bfc3caf3f73998be5d990ad7d27d4c375d74cc0e8e2`.
