# Render cadence and GPU diagnostics on uke

Boot `FE026F8E-9649-4073-B827-BFC325A187D4` of `3722f635…` shows a peak of 116 direct-display latches in one second in the early sample and 74 in a later window. The 428–450 s render trace records a 20.457 ms median commit-to-completion interval. It does not provide a display-latch count for that interval.

The comparison image removes `sheng-diag=1`, `sheng-gpu-trace=1` and `tu-debug=perf` from the boot menu. Its kernel and signed addon match the FUA-corrected image. The Metal work placement defaults, compile-on-asking-thread setting and 120 Hz display policy are retained. Vendor_boot SHA-256 `c4c466330fa4f4fc614a7dfae9e9d8163e1ca19d7612f50fd5be2b6c30e2eb43` passed host verification. Device comparison is pending.

At 2026-11-13T01:50:09.839697+08:00 all four boot partitions were written, slot A was confirmed and reboot was accepted. Hardware testing is pending.

On boot 3DDF7238 the user reported another freeze after about four minutes of desktop swipes. A short power press had no visible effect. The GPU replayed eight jobs after a ring switch timeout at 96.083 s, and user-space activity persisted to 236.236 s. The verified single-ring comparison 68c1b6e8 awaits authorization.
