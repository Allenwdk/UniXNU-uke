# Agent instructions

This is the uke (Xiaomi Pad 7, SM7675) device repository in format 2. Board
wiring, firmware selections and policy live here; shared SoC resources are in
UniXNU-sm7675-common and OS identity in the shared profiles. Preserve runtime
source filenames, classes, symbols, bundle IDs and protocol IDs; the canonical
component names come from the catalog and the board's fork pins live in
feeds.toml.

Read these pages before editing:

1. [Porting](https://unixnu.com/porting/)
2. [Device repositories](https://unixnu.com/porting/device-repository)
3. [Migration](https://unixnu.com/porting/migration)
4. [Components](https://unixnu.com/reference/components)
5. [Device trees](https://unixnu.com/porting/device-tree)

Record hardware results in porting.toml and run
`python3 scripts/porting-record.py check --device .` from the UniXNU checkout
before publishing.
