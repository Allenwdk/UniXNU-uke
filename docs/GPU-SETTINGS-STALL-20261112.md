# Settings transition stalls on uke, 2026-11-12

Boot `FEEE4C66-9B3B-4B9C-AE2F-83F08190DF1B` of vendor_boot `40872767…` records a ring 1 to 0 timeout at 375.569 s with STOP set in `0x341`. Recovery replayed three jobs, and the same backboardd PID 62 resumed completions after a 1.059 s gap. Settings transitions recorded from 379.881 s include a frame that spent 187.606 ms between creation and commit. The first hardware switch failure and the specific source of that pre-commit delay remain unresolved.

The candidate includes upstream Turnip BO cache retirement changes, Metal encoding CPU time and BO counters, and the sheng retained GPU snapshot diagnostics. The switch watchdog counts a timed-out switch once and uses a diagnostic register read that leaves the ring available for recovery. The software register-peer test covers both switch directions, a failed diagnostic read, and retirement of the replayed job at its actual fence.

The paired image with vendor_boot SHA-256 `d19516cae3372dabcead18a3ed3de5839c0ddd220d23d301149c5ab9bf448da2` passed host packaging and signature verification. Settings transitions, sustained rendering and sleep/wake await hardware testing.
