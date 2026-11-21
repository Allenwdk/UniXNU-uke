# Uke Focus Pen Pro acceptance

The owner reported no issues after retesting the component split on Xiaomi Pad 7 with iPadOS 27.0.1 24A446. The tested release uses `boot.img` SHA-256 `a5b9cf48b37bfe229b807582c82bbec60f5114c319106de0692bc49fc2a856fb` and `vendor_boot.img` SHA-256 `4f0527af845acad88d694b8815ffa2277810671767637ced7cc25880114ecc5b`. The device was rebooted on slot A after both boot slots were written.

The test release opted into `uke-pencil=4` and `timebase-hz=24000000`, with the Pencil prelink roots and 24A446 card fixups. Ordinary board configuration leaves Pencil disabled unless selected. The pen sources are pinned at `b946ec01745f4888020339f2137a634339b9443f`. The board supplies panel scanning, calibration, HID attachment, PMIC data and runtime assembly. [NativeFocusPenPro](https://github.com/Allenwdk/NativeFocusPenPro/tree/b946ec01745f4888020339f2137a634339b9443f)

The radio and stroke host tests, plus same-board panel capture replay, passed. The signed radio service, hover service, pairing application add-on and owner trust are byte-identical to the corresponding pre-split inputs. The owner retest covers the flashed split baseline. Individual Pro features, first-time pairing and long-run reconnect remain separate verification items.
