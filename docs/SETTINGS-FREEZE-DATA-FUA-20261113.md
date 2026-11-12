# Settings freeze and Data FUA completion on uke

The user reported a desktop freeze after repeated Settings transitions on `3722f635…` at device clock 08:54. Boot `FE026F8E-9649-4073-B827-BFC325A187D4` has 518 new valid log records. The last valid backboardd completion was at 450.433 s and the last valid submission was fence 5842 at 450.464 s. The frame file has a non-trace suffix. The kernel log ends at 456.600 s. The failure mechanism remains unresolved.

RamBlock now flushes the Data pages and bitmap before completing a write marked Force Unit Access. An opt-in `sheng-diag=1` sample reports Data flush and FUA counts alongside GPU fences and display latches. The host test reproduced the old early completion and passed on the corrected entry point.

The paired image with vendor_boot SHA-256 `6ee815a8744aca7786af104f9745c1b2af2db673ddacc2221eecadb16c5d953c` passed formal host build and verification. It awaits deployment and hardware testing.
