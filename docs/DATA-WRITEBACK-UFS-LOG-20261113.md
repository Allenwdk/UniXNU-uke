# Data write-back and incremental UFS logs on uke

RamBlock includes `aad2a16` and `525abe5`. In read-write direct mode it uses the board's cached-write callback and syncs pages before persisting their bitmap bits. `sheng-data-fua=1` selects write-through mode. Synchronize or the five-second timer flushes pending writes. The host tests cover concurrent I/O, failed flushes and simulated power loss.

The userdata log still uses `UKELLOG1` records on 16 KiB boundaries. The writer appends new message bytes in the occupied blocks and skips quiet intervals. It samples under the XNU log lock and commits the cursor after the FUA write succeeds. The decoder uses the length and CRC in the record header, including when unwritten suffix blocks hold older bytes.

The host-verified candidate uses `tu-debug=perf` and vendor_boot SHA-256 `3722f6351123d52e28709711a8ab10f9e11ba601e349f84be4709479acb93dd9`. Hardware verification is pending.

At 2026-11-13T01:09:45.871494+08:00 the four boot partitions were written, slot A was confirmed and reboot was accepted. Device acceptance is pending.
