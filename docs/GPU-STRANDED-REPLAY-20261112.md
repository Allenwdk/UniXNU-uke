# Oldest stranded GPU job replay

Boot F907407E-1328-42BC-B43E-808D79012947 still froze with concurrent binning disabled. It recorded a switch timeout and SMMU translation faults. Replacement clients received kIOReturnBusy.

The upstream 172f53f recovery started replay after the oldest stranded job and initialized the completed fence to that unexecuted job. A production submission test with actual replay and fence assertions failed on that code. Adreno740 c2b137440309c79dc5b307c2230cf36461e3648a keeps the predecessor fence and replays the oldest job. Both ring directions pass ASan/UBSan regression with leak detection disabled as in the existing fixture. Uke also records upstream mount and page-table failure counters.

Candidate vendor_boot 38703fa6f2c7952c692f4aabe1f423c09377a38b10da2507bbd30e09262d240d is unflashed. The first hardware fault and reconnect failure remain under investigation. The device is on stock four partitions, slot A and TWRP.
