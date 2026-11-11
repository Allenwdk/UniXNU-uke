# Runtime recovery compatibility, 2026-11-12

On boot AC485C6F-57AC-4CCB-A774-9DA0A362F33D the application-opening animation froze between two frames. The last successfully scheduled command completed at 131.147542 seconds. Submission failed at 131.156906 seconds, two completion errors were GpuRecoveryVictim, and backboardd then reported 670 MadeBeforeRecovery errors.

The runtime replaces the Vulkan device after a loss and reports GpuHang or GpuRecoveryVictim. The existing render-server restart checked only DeviceLost's Metal code 11. AdrenoMetal 1abb7ea1202ed441e3c8d3d7fc75494c28cda629 adds a separate query for these generation-loss errors and runs the existing post-handler restart path. Public Metal error codes are preserved. Two Cargo host regressions pass.

The paired candidate vendor_boot SHA-256 is d57dd83a2ece9f86184feebf204ab43ac2466dc0aea1d2133c99b6fe337dfae2. It reuses the native kernel and enables bounded runtime loss logging. The first Vulkan failure remains unresolved. The candidate is unflashed and the device is on stock four partitions, slot A and TWRP.
