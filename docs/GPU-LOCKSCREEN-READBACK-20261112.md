# GPU lock-screen readback, 2026-11-12

The recovery candidate vendor_boot 65376de9d334ab63206c857036f36bc915f6f3c0bf94449ff3723e9501ab4793 reached the lock screen and froze.

Current boot UUID 82F43E22-3F7B-4AB0-BC49-7854FA399CFE has a GpuRecoveryVictim command buffer near 96.840 seconds and 568 backboardd MadeBeforeRecovery errors between 96.983 and 116.432 seconds. The initial Vulkan loss is unresolved. The 45 switch timeouts in the persistent ring belong to the previous boot.

Evidence and candidate details are in the workspace report docs/status/gpu-lockscreen-readback-2026-11-12.md. The device is on stock boot/vendor_boot, slot A and TWRP after read-only capture.
