# Safari single-ring freeze and sheng XNU fixes

On boot `01A8EFB6-167B-4719-A940-9CE1C3ABFE76`, the GPU fence count advanced through 345.477 s with zero timeouts and ring switches. The WebKit networking and GPU extensions passed launch constraints at about 352.6 s. The kernel reported two Mach port guard AST events and stopped logging after an UDP connect at 352.809 s. Unified logs reach 331.350 s after clock alignment. The persisted frame file is byte-identical to an earlier diagnostic boot. The freeze mechanism remains unresolved.

The candidate includes sheng `9b19ac2` and `d6c4690`. `uke-progress=1` enables the existing progress line for Data, GPU fences and display latches without enabling full GPU tracing. The candidate remains single-ring. Vendor_boot SHA-256 `7e4b18dfd711f32097401dfb5f6aea9f4f07e35f9ac8d6e38303c2661333a2be` passed the formal build and host verification. Device testing is pending.

At 2026-11-13T02:29:28.998134+08:00 the candidate was written to all four boot partitions. Slot A was confirmed and fastboot accepted reboot. Safari testing is pending.

On boot E886E69B the user saw black regions while swiping home from Safari, then a freeze. The matching home gesture begins at 177.141 s. The last synchronized Data/GPU/display sample is at 112.424 s. Host-verified comparison 3477040c changes only dpu-scanout to 1 and awaits hardware testing.
