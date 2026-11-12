# Uke frame-path recording

The normal board profile uses mode-2 dual-ring scheduling, direct scanout and the fixed 120 Hz display configuration. The logging build enables the existing command, encoding, runtime, GPU pass, completion, display, touch and role-thread sampling paths.

Frame records rotate between two 12 MiB files. The kernel exports a diagnostic bundle at uptime 60 seconds, then waits 30 seconds after each completed export before the next. The two existing export halves alternate, with the header committed after the body is durable. Bounded channel and file sizes fit a 128 MiB half. Incomplete file reads and omitted content appear in diag-skipped.txt.

In TWRP use tools/read-frame-path.py with --serial b8a81f12 and --out pointing to a fresh host output directory. It reads the device without writing userdata and decodes both completed exports. Live capture and freeze coverage await hardware verification.

The host-verified paired vendor_boot SHA-256 is f33ab396d7b3eb4aac85a8b835cc64190c5b67b31485852848b39083b8a73e15. Formal kernel, arm64e plugin, 4 KiB signatures, owner trust, compressed payload and image checks passed. Hardware recording remains pending.
