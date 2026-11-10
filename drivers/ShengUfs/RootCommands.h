// SPDX-License-Identifier: BSD-2-Clause-Patent
// Shared production command path. Included inside the root transport namespace.
// The host harness replaces only MMIO, DMA synchronization, clock and lock primitives.
// One command slot (RootTransportLayout.h): its request list entry, command descriptor and data area.
struct UfsSlot {
  unsigned index;
  uint32_t entry, bit;     // request list entry and its doorbell bit
  uint8_t *utrd, *ucd, *data;
  uint64_t ucdPhys, dataPhys;
};
// The geometry of every slot, set once at bring-up (hostSlotGeometry), and the request list's form: in a cached region (each entry
// cleaned and invalidated by its own line) or on its own write-combined page (no cache maintenance).
static UfsSlot gSlotGeom[kSlots];
static bool gListUncached = false;
static int8_t gEntrySlot[32] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                 -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
// Doorbell bits of the commands rung and not yet seen complete (read by the completion interrupt), and per slot the completions the
// interrupt has seen (what a waiting command sleeps on).
static volatile uint32_t gOutstanding = 0;
static volatile uint32_t gSlotGen[kSlots] = {};

static void hostSlotGeometry (unsigned index, uint8_t *utrd, uint32_t entry, uint8_t *ucd, uint64_t ucdPhys, uint8_t *data,
                              uint64_t dataPhys)
{
  UfsSlot &slot = gSlotGeom[index];
  slot.index = index; slot.entry = entry; slot.bit = 1U << entry;
  slot.utrd = utrd; slot.ucd = ucd; slot.data = data; slot.ucdPhys = ucdPhys; slot.dataPhys = dataPhys;
  gEntrySlot[entry] = static_cast<int8_t> (index);
}

// Slots 0..count-1 of region A with the request list at its offset 0 (entry 2s).
static void hostSlotGeometryRegionA (unsigned count)
{
  for (unsigned e = 0; e < 32; ++e) gEntrySlot[e] = -1;
  gListUncached = false;
  for (unsigned s = 0; s < count && s < kRegionSlots; ++s)
    hostSlotGeometry (s, gHost.virt + ufsSlotUtrdOffset (s), ufsSlotEntry (s), gHost.virt + ufsSlotUcdOffset (s),
                      gHost.phys + ufsSlotUcdOffset (s), gHost.virt + ufsSlotDataOffset (s), gHost.phys + ufsSlotDataOffset (s));
}

static UfsSlot ufsSlot (unsigned index) { return gSlotGeom[index]; }

// Completion waits (hostWaitCompletion, supplied by the port): between two polls of a command, a port with the controller's interrupt
// sleeps until the interrupt sees this slot complete (or a short timeout) instead of spinning. `seen` is the slot's completion count
// read before the doorbell was polled, so a completion between that poll and the sleep is not missed.
struct HardwareTransferPort {
  const UfsSlot &slot;
  uint64_t deadline;
  uint32_t seen = 0;
  uint64_t started = mach_absolute_time ();   // when the wait began: the port polls first and sleeps only past its spin budget
  uint32_t doorbell () { seen = hostCompletionCount (slot.index); return (read32 (gHost.hc, 0x58) & slot.bit) ? 1U : 0U; }
  uint32_t interrupt () { return read32 (gHost.hc, 0x20); }
  uint32_t completedOcs () {
    if (!gListUncached) cacheInvalidate (reinterpret_cast<const uint8_t *> (reinterpret_cast<uintptr_t> (slot.utrd) & ~uintptr_t (63)), 64);
    return *reinterpret_cast<volatile uint32_t *> (slot.utrd + 8) & 0xff;
  }
  bool expired () { return mach_absolute_time () >= deadline; }
  void delay () { hostWaitCompletion (slot.index, seen, started); }
};

static void putBe32 (uint8_t *at, uint32_t value)
{
  at[0] = static_cast<uint8_t> (value >> 24);
  at[1] = static_cast<uint8_t> (value >> 16);
  at[2] = static_cast<uint8_t> (value >> 8);
  at[3] = static_cast<uint8_t> (value);
}

static uint32_t loadBe32 (const uint8_t *at)
{
  return (uint32_t (at[0]) << 24) | (uint32_t (at[1]) << 16) | (uint32_t (at[2]) << 8) | at[3];
}

// Issue the UPIU already placed in the slot's command descriptor through its request list entry. `direction` is
// the UTRD data direction (0 none, 1 from host, 2 to host). Direction 1 is reachable only through hostWrite4k, which
// confines it to the persistent store window. Returns the overall command status
// (0 = completed) in *ocs. Timeout/fatal status quarantines the allocation. Commands of different slots run concurrently:
// each touches only its own entry, descriptor and data area, and rings only its own doorbell bit (writing 0 bits has no effect).
// hostSubmit's halves: hostIssue rings the doorbell (*deadline: when the command times out); hostAwait polls for its completion.
// One caller may have commands of several slots outstanding between them.
// Zero copy (Linux ufshcd and Apple's storage drivers DMA straight into the request's pages): a physical region description table
// built from the caller's buffer (hostBuildPrds) instead of the slot's data area. Up to kMaxPrds entries of up to 256 KiB each, at most
// kXferMax per command; the caller's buffer gets the cache maintenance the data area otherwise gets.
struct UfsPrd { uint64_t addr; uint32_t bytes; };
static constexpr unsigned kMaxPrds = (kUcdStride - kPrdtOffset) / 16;
static constexpr uint32_t kXferMax = 1U << 20, kPrdMax = 256U << 10;
static bool gZeroCopy = true;
// Data commands by path, for the log: straight to or from the caller's pages, or through the slot's data area with a copy.
static volatile uint64_t gDirectCommands = 0, gStagedCommands = 0;
// Commands in flight, for the log: when each slot's doorbell rang (0 while the slot has no command out) and the command's opcode (the
// SCSI opcode, or the UPIU transaction type for others). The log names the oldest, so a command that does not complete is visible.
static volatile uint64_t gSlotIssuedAt[kSlots] = {};
static volatile uint8_t gSlotOpcode[kSlots] = {};

static IOReturn hostIssue (const UfsSlot &slot, uint32_t direction, uint32_t dataBytes, uint32_t timeoutUs, uint64_t *deadline,
                           uint32_t *ocs, const UfsPrd *prd = nullptr, unsigned prds = 0, const void *buffer = nullptr)
{
  *ocs = 15;
  if (prds > kMaxPrds || (prds && !buffer)) return kIOReturnBadArgument;
  if (!timeoutUs || dataBytes > (prds ? kXferMax : kDataMax) || (dataBytes & 3) ||
      (dataBytes ? (direction != 1 && direction != 2) : direction != 0)) return kIOReturnBadArgument;
  if (gHost.poisoned) return kIOReturnNotReady;
  if (read32 (gHost.hc, 0x58) & slot.bit) {
    gHost.poisoned = true;
    gHost.ready = false;
    return recordTransport (kIOReturnBusy, *ocs);
  }
  if (read32 (gHost.hc, 0x20) & kUfsFatalStatus) {
    gHost.poisoned = true;
    gHost.ready = false;
    return recordTransport (kIOReturnIOError, *ocs);
  }
  gRootDiagnostic.command++;
  gRootDiagnostic.type = slot.ucd[0];
  gRootDiagnostic.lun = slot.ucd[2];
  gRootDiagnostic.opcode = slot.ucd[gRootDiagnostic.type == 1 ? 16 : 12];
  gRootDiagnostic.scsi = 0xffff;
  gRootDiagnostic.response = 0;
  gRootDiagnostic.protocol = 0;
  gRootDiagnostic.residual = 0;
  gRootDiagnostic.sense = 0;
  uint8_t *utrd = slot.utrd;
  uint64_t ucdPhys = slot.ucdPhys;
  uint32_t *words = reinterpret_cast<uint32_t *> (utrd);
  for (unsigned index = 0; index < 8; index++) {
    words[index] = 0;
  }
  // I (bit 24): the completion sets IS.UTRCS and raises the interrupt (UFSHCI UTRD dword 0; Linux ufshcd UTP_REQ_DESC_INT_CMD).
  words[0] = 0x10000000U | (direction << 25) | (hostInterruptMode () ? 1U << 24 : 0U);
  words[2] = 0x0fU;
  words[4] = static_cast<uint32_t> (ucdPhys);
  words[5] = static_cast<uint32_t> (ucdPhys >> 32);
  words[6] = (512U >> 2) | ((kRspOffset >> 2) << 16);
  const unsigned entries = dataBytes ? (prds ? prds : 1U) : 0U;
  words[7] = entries | ((kPrdtOffset >> 2) << 16);
  if (dataBytes) {
    uint32_t *table = reinterpret_cast<uint32_t *> (slot.ucd + kPrdtOffset);
    uint64_t sum = 0;
    for (unsigned i = 0; i < entries; ++i) {
      const uint64_t address = prds ? prd[i].addr : slot.dataPhys;
      const uint32_t bytes = prds ? prd[i].bytes : dataBytes;
      if (!bytes || bytes > kPrdMax || (bytes & 3) || (address & 3)) return kIOReturnBadArgument;
      table[4 * i] = static_cast<uint32_t> (address);
      table[4 * i + 1] = static_cast<uint32_t> (address >> 32);
      table[4 * i + 2] = 0;
      table[4 * i + 3] = bytes - 1;
      sum += bytes;
    }
    if (sum != dataBytes) return kIOReturnBadArgument;
  }
  if (!gListUncached) cacheCivac (slot.utrd, 32);
  cacheCivac (slot.ucd, kPrdtOffset + 16 * (entries ? entries : 1U));
  if (dataBytes) cacheCivac (prds ? buffer : slot.data, dataBytes);
  if (!hostInterruptMode ()) hostWrite (0x20, 1);  // Acknowledge completion only; preserve error evidence. With interrupts the handler does.
  clock_interval_to_deadline (timeoutUs, kMicrosecondScale, deadline);
  __atomic_or_fetch (&gOutstanding, slot.bit, __ATOMIC_RELEASE);
  gSlotOpcode[slot.index] = slot.ucd[slot.ucd[0] == 1 ? 16 : 12];
  __atomic_store_n (&gSlotIssuedAt[slot.index], mach_absolute_time (), __ATOMIC_RELAXED);
  hostWrite (0x58, slot.bit);
  return kIOReturnSuccess;
}

// `direction` is the command's UTRD data direction. Only data the device wrote (2) is invalidated after the command: the device does
// not change the data of a write (1), which was cleaned to memory before the doorbell, so its cache lines need nothing (Linux's
// arch_sync_dma_for_cpu returns at once for DMA_TO_DEVICE). An invalidate there would also drop a line a CPU wrote to the caller's
// page while the command ran.
static IOReturn hostAwait (const UfsSlot &slot, uint32_t dataBytes, uint64_t deadline, uint32_t *ocs, const void *buffer = nullptr,
                           unsigned direction = 2)
{
  HardwareTransferPort port = { slot, deadline, 0 };
  UfsTransferPoll state;
  UfsTransferEnd end = pollUfsTransfer (port, state);
  __atomic_store_n (&gSlotIssuedAt[slot.index], 0, __ATOMIC_RELAXED);
  if (end == UfsTransferEnd::Completed) {
    __atomic_and_fetch (&gOutstanding, ~slot.bit, __ATOMIC_RELEASE);
    hostWaitDone (port.started);
  }
  gRootDiagnostic.polls = state.polls;
  gRootDiagnostic.dbSeen = state.doorbellSeen;
  gRootDiagnostic.isSeen = state.interruptSeen;
  *ocs = state.ocs;
  if (end != UfsTransferEnd::Completed) {
    // No cancel/reset/retry: the controller may still own the buffer, even
    // with DB=0. Keep it allocated and prohibit all subsequent CPU writes.
    gHost.poisoned = true;
    gHost.ready = false;
    return recordTransport (end == UfsTransferEnd::Timeout ? kIOReturnTimeout : kIOReturnIOError, *ocs);
  }
  cacheInvalidate (slot.ucd, kPrdtOffset);
  if (dataBytes && direction != 1) cacheInvalidate (buffer ? buffer : slot.data, dataBytes);
  gRootDiagnostic.response = loadBe32 (slot.ucd + kRspOffset);
  return recordTransport (*ocs == 0 ? kIOReturnSuccess : kIOReturnIOError, *ocs);
}

static IOReturn hostSubmit (const UfsSlot &slot, uint32_t direction, uint32_t dataBytes, uint32_t timeoutUs, uint32_t *ocs)
{
  uint64_t deadline = 0;
  IOReturn result = hostIssue (slot, direction, dataBytes, timeoutUs, &deadline, ocs);
  return result != kIOReturnSuccess ? result : hostAwait (slot, dataBytes, deadline, ocs, nullptr, direction);
}

static bool hostClearUcd (const UfsSlot &slot)
{
  if (gHost.poisoned) return false;
  // Check before touching the command/response area, not just before ringing.
  if (read32 (gHost.hc, 0x58) & slot.bit) {
    gHost.poisoned = true;
    gHost.ready = false;
    recordTransport (kIOReturnBusy, 15);
    return false;
  }
  if (read32 (gHost.hc, 0x20) & kUfsFatalStatus) {
    gHost.poisoned = true;
    gHost.ready = false;
    recordTransport (kIOReturnIOError, 15);
    return false;
  }
  uint32_t *words = reinterpret_cast<uint32_t *> (slot.ucd);
  for (unsigned index = 0; index < (kPrdtOffset + 16) / 4; index++) {
    words[index] = 0;
  }
  return true;
}

static IOReturn hostProtocolError (uint32_t reason)
{
  gRootDiagnostic.protocol = reason;
  gHost.ready = false;
  gHost.poisoned = true;
  return recordTransport (kIOReturnIOError, gRootDiagnostic.ocs);
}

// Validate a completed response before interpreting zero-filled status bytes.
// Protocol codes: 1 type, 2 task tag, 3 LUN, 4 length/EHS, 5 query echo,
// 6 short/overrun transfer, 7 nonzero NOP result.
static IOReturn hostResponseHeader (const UfsSlot &slot, uint8_t type, uint8_t lun, uint8_t tag)
{
  const uint8_t *rsp = slot.ucd + kRspOffset;
  if (rsp[0] != type) return hostProtocolError (1);
  if (rsp[3] != tag) return hostProtocolError (2);
  if (rsp[2] != lun) return hostProtocolError (3);
  unsigned length = (unsigned (rsp[10]) << 8) | rsp[11];
  if (rsp[8] || length > 512 - 32) return hostProtocolError (4);
  return kIOReturnSuccess;
}

// Bring-up commands (NOP, query, the wake-up SCSI commands) run on slot 0 before any other command can start.
static IOReturn hostNop (void)
{
  const UfsSlot slot = ufsSlot (0);
  if (!hostClearUcd (slot)) return kIOReturnNotReady;
  slot.ucd[3] = 1;
  uint32_t ocs = 15;
  IOReturn result = hostSubmit (slot, 0, 0, 200000, &ocs);
  if (result != kIOReturnSuccess) return result;
  result = hostResponseHeader (slot, 0x20, 0, 1);
  if (result != kIOReturnSuccess) return result;
  const uint8_t *rsp = slot.ucd + kRspOffset;
  if (rsp[10] || rsp[11]) return hostProtocolError (4);
  if (rsp[6] || rsp[7]) return hostProtocolError (7);
  return kIOReturnSuccess;
}

// Run a SCSI command. direction 0: no data, 1: host -> device from the DMA data area (already filled by the caller), 2: device
// -> host into the DMA data area. Callers other than hostWrite4k never pass direction 1.
// The task tag is the slot's request list entry, unique among the commands in flight. hostScsiIssue builds and rings the command,
// hostScsiAwait waits for it and checks its response; hostScsiDir runs both.
static IOReturn hostScsiIssue (const UfsSlot &slot, uint8_t lun, const uint8_t *cdb, unsigned cdbLength, unsigned direction,
                               uint32_t dataBytes, uint32_t timeoutUs, uint64_t *deadline, uint32_t *scsiStatus,
                               const UfsPrd *prd = nullptr, unsigned prds = 0, const void *buffer = nullptr)
{
  *scsiStatus = 0xffff;
  if (!cdb || !cdbLength || cdbLength > 16 || dataBytes > (prds ? kXferMax : kDataMax) || direction > 2 ||
      (dataBytes & 3) || (dataBytes ? !direction : direction)) return kIOReturnBadArgument;
  if (!hostClearUcd (slot)) return kIOReturnNotReady;
  uint8_t *ucd = slot.ucd;
  const uint8_t tag = static_cast<uint8_t> (slot.entry);
  ucd[0] = 0x01;                      // COMMAND UPIU
  ucd[1] = direction == 2 ? 0x40 : direction == 1 ? 0x20 : 0x00;   // R: data in, W: data out
  ucd[2] = lun;
  ucd[3] = tag;
  putBe32 (ucd + 12, dataBytes);
  for (unsigned index = 0; index < cdbLength; index++) {
    ucd[16 + index] = cdb[index];
  }
  uint32_t ocs = 0xf;
  return hostIssue (slot, direction ? direction : 0U, dataBytes, timeoutUs, deadline, &ocs, prd, prds, buffer);
}

static IOReturn hostScsiAwait (const UfsSlot &slot, uint8_t lun, uint32_t dataBytes, uint64_t deadline, uint32_t *scsiStatus,
                               const void *buffer = nullptr, unsigned direction = 2)
{
  *scsiStatus = 0xffff;
  uint32_t ocs = 0xf;
  IOReturn status = hostAwait (slot, dataBytes, deadline, &ocs, buffer, direction);
  if (status != kIOReturnSuccess) return status;
  const uint8_t tag = static_cast<uint8_t> (slot.entry);
  status = hostResponseHeader (slot, 0x21, lun, tag);
  if (status != kIOReturnSuccess) return status;
  const uint8_t *rsp = slot.ucd + kRspOffset;
  *scsiStatus = (uint32_t (rsp[6]) << 8) | rsp[7];
  gRootDiagnostic.scsi = *scsiStatus;
  gRootDiagnostic.residual = loadBe32 (rsp + 12);
  unsigned length = (unsigned (rsp[10]) << 8) | rsp[11];
  if (length) {
    unsigned senseLength = (unsigned (rsp[32]) << 8) | rsp[33];
    if (length < 2 || senseLength > length - 2) return hostProtocolError (4);
    const uint8_t *sense = rsp + 34;
    if (senseLength >= 14 && ((sense[0] & 0x7f) == 0x70 || (sense[0] & 0x7f) == 0x71))
      gRootDiagnostic.sense = (uint32_t (sense[2] & 15) << 16) | (uint32_t (sense[12]) << 8) | sense[13];
    else if (senseLength >= 4 && ((sense[0] & 0x7f) == 0x72 || (sense[0] & 0x7f) == 0x73))
      gRootDiagnostic.sense = (uint32_t (sense[1] & 15) << 16) | (uint32_t (sense[2]) << 8) | sense[3];
  }
  if (!*scsiStatus && ((rsp[1] & 0x60) || gRootDiagnostic.residual)) return hostProtocolError (6);
  IOReturn result = *scsiStatus == 0 ? kIOReturnSuccess : kIOReturnNotReady;
  gRootDiagnostic.result = static_cast<uint32_t> (result);
  return result;
}

static IOReturn hostScsiDir (const UfsSlot &slot, uint8_t lun, const uint8_t *cdb, unsigned cdbLength, unsigned direction,
                             uint32_t dataBytes, uint32_t timeoutUs, uint32_t *scsiStatus,
                             const UfsPrd *prd = nullptr, unsigned prds = 0, const void *buffer = nullptr)
{
  uint64_t deadline = 0;
  IOReturn status = hostScsiIssue (slot, lun, cdb, cdbLength, direction, dataBytes, timeoutUs, &deadline, scsiStatus, prd, prds, buffer);
  return status != kIOReturnSuccess ? status
                                    : hostScsiAwait (slot, lun, dataBytes, deadline, scsiStatus, prds ? buffer : nullptr, direction);
}

// The original read-only entry point: data, when present, always flows device -> host.
static IOReturn hostScsi (uint8_t lun, const uint8_t *cdb, unsigned cdbLength, bool toHost, uint32_t dataBytes,
                          uint32_t timeoutUs, uint32_t *scsiStatus)
{
  if (dataBytes && !toHost) { *scsiStatus = 0xffff; return kIOReturnBadArgument; }
  return hostScsiDir (ufsSlot (0), lun, cdb, cdbLength, dataBytes ? 2U : 0U, dataBytes, timeoutUs, scsiStatus);
}

// Query read request (opcode 0x03 READ ATTRIBUTE / 0x05 READ FLAG). Returns the value field.
static IOReturn hostQueryRead (uint8_t opcode, uint8_t idn, uint32_t *value)
{
  if (!value || (opcode != 3 && opcode != 5)) return kIOReturnBadArgument;
  const UfsSlot slot = ufsSlot (0);
  if (!hostClearUcd (slot)) return kIOReturnNotReady;
  uint8_t *ucd = slot.ucd;
  ucd[0] = 0x16;                      // QUERY REQUEST
  ucd[3] = 1;
  ucd[5] = 0x01;                      // standard read request (header dword 1, byte 1)
  ucd[12] = opcode;
  ucd[13] = idn;
  uint32_t ocs = 0xf;
  IOReturn status = hostSubmit (slot, 0, 0, 200000, &ocs);
  if (status != kIOReturnSuccess) {
    return status;
  }
  status = hostResponseHeader (slot, 0x36, 0, 1);
  if (status != kIOReturnSuccess) return status;
  const uint8_t *rsp = ucd + kRspOffset;
  if (rsp[10] || rsp[11]) return hostProtocolError (4);
  if (rsp[12] != opcode || rsp[13] != idn || rsp[14] || rsp[15]) return hostProtocolError (5);
  if (rsp[6] != 0) {
    gRootDiagnostic.result = static_cast<uint32_t> (kIOReturnError);
    return kIOReturnError;
  }
  *value = loadBe32 (rsp + 20);
  return kIOReturnSuccess;
}

// ---- Command slots ------------------------------------------------------------------------------------------------
// gSlotCount slots exist (hostSlotsInit: what the DMA region and the request list hold). With more than one, the last is kept for
// the panic path, which must not wait, and the others form the pool that block I/O takes a slot from for one command.
static unsigned gSlotCount = 1;
static uint32_t gSlotFree = 1;
static IOLock *gSlotLock = nullptr;
// Pool statistics (under gSlotLock): slots taken now, the most taken at once, takes that had to wait for a slot.
static uint32_t gSlotsBusy = 0, gSlotsPeak = 0;
static uint64_t gSlotWaits = 0;
static volatile uint32_t gSlotWaitersNow = 0;   // takers waiting for a slot now, for the log
// With the pool empty, takers queue in arrival order and a freed slot goes straight to the oldest of them (no thread can be passed over).
struct UfsSlotWaiter { UfsSlotWaiter *next; int slot; };
static UfsSlotWaiter *gSlotWaitHead = nullptr, *gSlotWaitTail = nullptr;
// Hybrid completion waits (as Linux's hybrid block polling): a wait polls for its first gSpinUs microseconds, then sleeps on the
// completion interrupt. Short commands complete while polling; long ones leave the CPU after the budget.
static uint32_t gSpinUs = 200;
// Set once by the panic path (native_ufs_log_panic). The CPUs that still run may hold a slot forever, so with one slot it is tried for
// 200 ms and then used anyway. Commands only poll (TransferPoll.h), so they also run with interrupts masked.
static bool gHostPanic = false;

static void hostSlotsInit (unsigned count, IOLock *lock)
{
  gSlotCount = count < 1 ? 1 : count > kSlots ? kSlots : count;
  gSlotFree = gSlotCount > 1 ? (1U << (gSlotCount - 1)) - 1 : 1;
  gSlotLock = lock;
}

// Returns the slot to use; *pooled says whether it must be given back.
static UfsSlot hostSlotTake (bool *pooled)
{
  *pooled = false;
  if (gHostPanic) {
    if (gSlotCount > 1) return ufsSlot (gSlotCount - 1);
    for (unsigned tries = 0; tries < 200000; tries++) {
      if (IOLockTryLock (gSlotLock)) {
        const bool free = gSlotFree & 1;
        if (free) gSlotFree = 0;
        IOLockUnlock (gSlotLock);
        if (free) { *pooled = true; break; }
      }
      IODelay (1);
    }
    return ufsSlot (0);
  }
  IOLockLock (gSlotLock);
  unsigned index;
  if (gSlotFree) {
    index = static_cast<unsigned> (__builtin_ctz (gSlotFree));
    gSlotFree &= ~(1U << index);
  } else {
    ++gSlotWaits;
    UfsSlotWaiter waiter = { nullptr, -1 };
    if (gSlotWaitTail) gSlotWaitTail->next = &waiter; else gSlotWaitHead = &waiter;
    gSlotWaitTail = &waiter;
    __atomic_fetch_add (&gSlotWaitersNow, 1, __ATOMIC_RELAXED);
    while (waiter.slot < 0) IOLockSleep (gSlotLock, &waiter, THREAD_UNINT);
    __atomic_fetch_sub (&gSlotWaitersNow, 1, __ATOMIC_RELAXED);
    index = static_cast<unsigned> (waiter.slot);
  }
  if (++gSlotsBusy > gSlotsPeak) gSlotsPeak = gSlotsBusy;
  IOLockUnlock (gSlotLock);
  *pooled = true;
  return ufsSlot (index);
}

static void hostSlotGive (const UfsSlot &slot, bool pooled)
{
  if (!pooled) return;
  if (gHostPanic) { gSlotFree |= 1U << slot.index; return; }   // no sleeper wakes in a panic
  IOLockLock (gSlotLock);
  --gSlotsBusy;
  if (UfsSlotWaiter *waiter = gSlotWaitHead) {
    gSlotWaitHead = waiter->next;
    if (!gSlotWaitHead) gSlotWaitTail = nullptr;
    waiter->slot = static_cast<int> (slot.index);
    IOLockWakeup (gSlotLock, waiter, true);
  } else {
    gSlotFree |= 1U << slot.index;
  }
  IOLockUnlock (gSlotLock);
}

// A command the device refused for now: BUSY (0x08) or TASK SET FULL (0x28), as when more commands are queued than it accepts. It is
// sent again after a short pause, up to kBusyRetries times; any other status is final.
static constexpr unsigned kBusyRetries = 50;
static volatile uint64_t gBusyRetries = 0;   // commands sent again, for the log
static bool hostRetryable (IOReturn status, uint32_t scsi, unsigned attempt)
{
  if (status != kIOReturnNotReady || (scsi != 0x08 && scsi != 0x28) || attempt >= kBusyRetries) return false;
  __atomic_fetch_add (&gBusyRetries, 1, __ATOMIC_RELAXED);
  IODelay (attempt < 4 ? 50U << attempt : 1000U);
  return true;
}

// The PRDT of a buffer for zero copy: its pages' DMA addresses (hostDmaAddress, supplied by the port; 0 = not reachable), adjacent
// pages merged. 0 when the buffer does not qualify (not page aligned, a partial page, longer than kXferMax, an unreachable page, more
// than kMaxPrds entries): the caller then uses the slot's data area.
static unsigned hostBuildPrds (const void *buffer, uint64_t bytes, UfsPrd *prd)
{
  if (!gZeroCopy || (reinterpret_cast<uintptr_t> (buffer) & 4095) || !bytes || (bytes & 4095) || bytes > kXferMax) return 0;
  unsigned count = 0;
  for (uint64_t at = 0; at < bytes; at += 4096) {
    const uint64_t address = hostDmaAddress (static_cast<const uint8_t *> (buffer) + at);
    if (!address || (address & 4095)) return 0;
    if (count && prd[count - 1].addr + prd[count - 1].bytes == address && prd[count - 1].bytes + 4096 <= kPrdMax) {
      prd[count - 1].bytes += 4096;
    } else {
      if (count == kMaxPrds) return 0;
      prd[count].addr = address; prd[count].bytes = 4096; ++count;
    }
  }
  return count;
}

// Read `blocks` 4096-byte blocks of LUN `lun` starting at `lba` into dst: one command of up to kXferMax straight into dst when it
// qualifies for zero copy, else commands of up to kDataMax through the slot's data area.
static IOReturn hostRead4k (uint8_t lun, uint64_t lba, uint32_t blocks, void *dst)
{
  if (!gHost.ready || !dst || !blocks || lba > 0xffffffffULL - (blocks - 1)) {
    return kIOReturnBadArgument;
  }
  UfsPrd prd[kMaxPrds];
  const unsigned prds = blocks <= kXferMax / 4096 ? hostBuildPrds (dst, uint64_t (blocks) * 4096, prd) : 0;
  if (!prds && blocks > kDataMax / 4096) {
    // Beyond one command: pieces of kXferMax (each tried for zero copy again), or of kDataMax through the data area.
    const uint32_t step = blocks > kXferMax / 4096 ? kXferMax / 4096 : kDataMax / 4096;
    for (uint32_t done = 0; done < blocks; done += step) {
      const uint32_t chunk = blocks - done > step ? step : blocks - done;
      IOReturn status = hostRead4k (lun, lba + done, chunk, static_cast<uint8_t *> (dst) + uint64_t (done) * 4096);
      if (status != kIOReturnSuccess) return status;
    }
    return kIOReturnSuccess;
  }
  bool pooled;
  const UfsSlot slot = hostSlotTake (&pooled);
  gRootDiagnostic.lba = lba;
  __atomic_fetch_add (prds ? &gDirectCommands : &gStagedCommands, 1, __ATOMIC_RELAXED);
  uint8_t cdb[10] = { 0x28, 0, uint8_t (lba >> 24), uint8_t (lba >> 16), uint8_t (lba >> 8), uint8_t (lba),
                      0, uint8_t (blocks >> 8), uint8_t (blocks), 0 };
  uint32_t scsi = 0;
  IOReturn status;
  for (unsigned attempt = 0;; ++attempt) {
    status = hostScsiDir (slot, lun, cdb, 10, 2, blocks * 4096U, 1000000, &scsi, prds ? prd : nullptr, prds, prds ? dst : nullptr);
    if (!hostRetryable (status, scsi, attempt)) break;
  }
  if (status == kIOReturnSuccess && !prds) {
    memcpy (dst, slot.data, blocks * 4096U);
  }
  hostSlotGive (slot, pooled);
  return status;
}

// ---- Persistent store and log windows ------------------------------------------------------------------------------
// The only place this driver writes. At most two windows of 4096-byte LBAs on LUN 0 (the Data store and the diagnostic log),
// each opened once by its integration after it has proved that the window lies inside the userdata partition and clear of the
// root image; every write is checked against them again here, so nothing outside them can be written even if a caller is wrong.
static bool gStoreOpen = false, gLogOpen = false;
static uint64_t gStoreFirst = 0, gStoreBlocks = 0, gLogFirst = 0, gLogBlocks = 0;

static bool hostLogOpen (uint64_t first, uint64_t blocks)
{
  if (gLogOpen || !blocks || first > 0xffffffffULL || blocks > 0xffffffffULL - first) return false;
  if (gStoreOpen && first < gStoreFirst + gStoreBlocks && gStoreFirst < first + blocks) return false;
  gLogFirst = first;
  gLogBlocks = blocks;
  gLogOpen = true;
  return true;
}

static bool hostInWindow (uint64_t lba, uint32_t blocks)
{
  if (gStoreOpen && lba >= gStoreFirst && lba - gStoreFirst < gStoreBlocks && blocks <= gStoreBlocks - (lba - gStoreFirst)) return true;
  return gLogOpen && lba >= gLogFirst && lba - gLogFirst < gLogBlocks && blocks <= gLogBlocks - (lba - gLogFirst);
}

static bool hostStoreOpen (uint64_t first, uint64_t blocks)
{
  if (gStoreOpen || !blocks || first > 0xffffffffULL || blocks > 0xffffffffULL - first) return false;
  if (gLogOpen && first < gLogFirst + gLogBlocks && gLogFirst < first + blocks) return false;
  gStoreFirst = first;
  gStoreBlocks = blocks;
  gStoreOpen = true;
  return true;
}

// WRITE(10) of `blocks` 4096-byte blocks from src to lba on LUN 0; the range must lie inside one open window.
static IOReturn hostWrite4k (uint8_t lun, uint64_t lba, uint32_t blocks, const void *src, bool fua = false)
{
  if (!gHost.ready || !src || !blocks || blocks > kXferMax / 4096 || lun != 0 || !hostInWindow (lba, blocks)) {
    return kIOReturnBadArgument;
  }
  UfsPrd prd[kMaxPrds];
  const unsigned prds = hostBuildPrds (src, uint64_t (blocks) * 4096, prd);
  if (!prds && blocks > kDataMax / 4096) {
    for (uint32_t done = 0; done < blocks; done += kDataMax / 4096) {
      const uint32_t chunk = blocks - done > kDataMax / 4096 ? kDataMax / 4096 : blocks - done;
      IOReturn status = hostWrite4k (lun, lba + done, chunk, static_cast<const uint8_t *> (src) + uint64_t (done) * 4096, fua);
      if (status != kIOReturnSuccess) return status;
    }
    return kIOReturnSuccess;
  }
  bool pooled;
  const UfsSlot slot = hostSlotTake (&pooled);
  gRootDiagnostic.lba = lba;
  __atomic_fetch_add (prds ? &gDirectCommands : &gStagedCommands, 1, __ATOMIC_RELAXED);
  if (!prds) memcpy (slot.data, src, blocks * 4096U);
  uint8_t cdb[10] = { 0x2a, uint8_t (fua ? 0x08 : 0), uint8_t (lba >> 24), uint8_t (lba >> 16), uint8_t (lba >> 8), uint8_t (lba),
                      0, uint8_t (blocks >> 8), uint8_t (blocks), 0 };
  uint32_t scsi = 0;
  IOReturn status;
  for (unsigned attempt = 0;; ++attempt) {
    status = hostScsiDir (slot, 0, cdb, 10, 1, blocks * 4096U, 2000000, &scsi, prds ? prd : nullptr, prds, prds ? src : nullptr);
    if (!hostRetryable (status, scsi, attempt)) break;
  }
  hostSlotGive (slot, pooled);
  return status;
}

// SYNCHRONIZE CACHE(10) for the whole LUN: every write that completed before this call is on the medium when it returns.
static IOReturn hostSyncCache (void)
{
  if (!gHost.ready || (!gStoreOpen && !gLogOpen)) return kIOReturnNotReady;
  bool pooled;
  const UfsSlot slot = hostSlotTake (&pooled);
  uint8_t cdb[10] = { 0x35 };
  uint32_t scsi = 0;
  IOReturn status;
  for (unsigned attempt = 0;; ++attempt) {
    status = hostScsiDir (slot, 0, cdb, 10, 0, 0, 10000000, &scsi);
    if (!hostRetryable (status, scsi, attempt)) break;
  }
  hostSlotGive (slot, pooled);
  return status;
}
