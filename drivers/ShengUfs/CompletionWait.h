// SPDX-License-Identifier: BSD-2-Clause-Patent
// What a command's completion wait does between two polls (hostWaitCompletion). The port supplies the elapsed time of the wait,
// whether the completion interrupt serves the host, whether this thread may block, and whether the scheduler wants this CPU.
//
// Modes (boot-arg native-ufs-wait):
//   Spin (1, default)  poll without blocking for the spin budget, then sleep until the completion interrupt. A pause between two polls
//                      is one WFE, which the counter's event stream ends within about a microsecond, as XNU's own spun delays. The
//                      spin ends early, and the wait sleeps, when a preemption is pending on this CPU.
//   Paced (0)          pause with IODelay (2) for the spin budget, then sleep. XNU blocks such a delay on a timer unless the CPU
//                      registered an idle exit latency above it (ml_delay_should_spin), so each pause then lasts a timer wakeup.
//   Sleep (2)          sleep until the completion interrupt from the first pause on.
// Without the interrupt, with interrupts masked or with preemption disabled the wait cannot sleep and pauses with IODelay, which
// spins in those contexts.
enum class UfsWaitStep { Pause, Spin, Sleep };
static constexpr uint32_t kUfsWaitPaced = 0, kUfsWaitSpin = 1, kUfsWaitSleep = 2;

static inline UfsWaitStep ufsWaitStep (uint32_t mode, bool interruptMode, bool canBlock, bool preemptPending, uint64_t elapsed,
                                       uint64_t spinTicks)
{
  if (!interruptMode || !canBlock) return UfsWaitStep::Pause;
  if (mode == kUfsWaitSleep || elapsed >= spinTicks) return UfsWaitStep::Sleep;
  if (mode == kUfsWaitPaced) return UfsWaitStep::Pause;
  return preemptPending ? UfsWaitStep::Sleep : UfsWaitStep::Spin;
}

// Completion time ranges of the log's wait histogram: under 50, 100, 200, 400 us, longer.
static constexpr unsigned kUfsWaitRanges = 5;
static inline unsigned ufsWaitRange (uint64_t us)
{
  return us < 50 ? 0 : us < 100 ? 1 : us < 200 ? 2 : us < 400 ? 3 : 4;
}
