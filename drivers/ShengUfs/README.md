# ShengUfs

SM8550 UFS transport and sheng persistent storage.

The historical native UFS root qualification applies to the iOS 17 path.

Build and interface details: [documentation](https://unixnu.com/drivers/sheng-ufs). Component sources and dependencies: [catalog](https://unixnu.com/reference/components).

UniXNU's `scripts/build-sheng-source-drivers.py --with-storage-family`
compiles this driver and public IOStorageFamily 337.100.1 against XNU 12377
for arm64e iOS 26. Verified-root read policy remains enabled. These archives
still require family/APFS linkage and device qualification; they do not establish
an iPadOS 26 root boot. See the [migration procedure](https://unixnu.com/porting/migration#source-drivers).

The arm64e `build-arm-driver.py ShengUfs` entry now selects
`build-sheng-source-storage.py`. It builds a deferred public-source
IOBlockStorageDevice adapter and the board's SourceStorage runtime. The runtime
loads the original IOStorageFamily and APFS, checks the original base class
size, authenticates the 23F84 policies, registers the source classes, and starts
NativeUfs on the platform root. Its runtime metadata has a separate name and
inherits the original storage metaclass; its C++ class name is preserved for
compiler-generated arm64e pointer authentication.

`native-source-storage=1 native-ufs-root=1` selects this path. The root policy
tables travel in `kernel.macho.vroot`; only the owner public anchor is linked
into the kernel. This implementation has not been built or run since the
request to stop testing. Class size equality alone does not establish virtual
interface compatibility. The previous iOS 17 qualification does not apply.
