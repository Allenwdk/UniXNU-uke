# Uke ShengHidInput board variant

Based on upstream ShengHidInput `71a40e76daeae1bb4127546f96ffa7339c7a06ff`. Source, runtime class NativeHidInput, existing symbols and report IDs remain. The board adds a Consumer-only built-in button factory while retaining the original touch factory and legacy combined mode. The upstream generic keyboard service imposed a 150ms Consumer Power filter; a service dictionary override read back zero without correcting hardware short presses.

Build uses the existing `build-sheng-hid-input.py` and original arm64e IOHID ABI audit. This local variant is selected through the tracked device feeds.toml local module; the original upstream commit is recorded here and the ignored fetched checkout remains unchanged. Hardware acceptance is recorded in the root docs and device porting.toml, not inferred from compilation.
