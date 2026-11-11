# GPU connection after render-server restart

On boot 457E40D9-E26D-4A68-BDB5-CAF44D626D8C the first GPU recovery followed a read translation fault at 0x703a0000. A second recovery followed a switch from ring 1 to ring 0 that timed out. The new backboardd and SpringBoard could not open IOAcceleratorES because the driver returned kIOReturnBusy.

The unflashed control vendor_boot 4cecac557f50ad0ba9af5b8190cce6498a6d9daf4332c0ead8befce29fde54ee changes only tu-debug=perf to tu-debug=perf,nocb. The device remains on stock boot/vendor_boot, slot A and TWRP.
