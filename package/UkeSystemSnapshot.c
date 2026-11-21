// SPDX-License-Identifier: MIT
// One CPU, process and VM report per 30-second window when log-sys=2.
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/counter.h>
#include <kern/host.h>
#include <kern/recount.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <mach/mach_host.h>
#include <mach/processor_info.h>
#include <mach/vm_statistics.h>
#include <sys/proc_internal.h>
#include <sys/proc_private.h>
#include <vm/vm_compressor_backing_store_xnu.h>

#define UKE_CPU_COUNT 8
#define UKE_TOP_COUNT 10
#define UKE_PROCESS_SLOTS 2048
#define UKE_MIB (1024ULL * 1024)

extern char bootsessionuuid_string[37];
extern uint64_t sane_size;
extern processor_t cpu_to_processor(int cpu);
extern void processor_cpu_load_info(processor_t processor, natural_t ticks[static CPU_STATE_MAX]);
extern uint64_t vm_swap_get_used_space(void);
extern unsigned sheng_cpufreq_snapshot(uint32_t state_mhz[3], uint32_t vote_mhz[3]);
SCALABLE_COUNTER_DECLARE(vm_swapin_iotier_grants);

void uke_system_snapshot_start(void);

struct process_counter {
	uint64_t key;
	uint64_t time;
};

struct process_window {
	struct process_counter *before;
	struct process_counter *after;
	unsigned missed_before;
	unsigned missed_after;
	unsigned count;
	struct {
		uint64_t time;
		pid_t pid;
		char name[32];
	} top[UKE_TOP_COUNT];
};

static struct process_counter *
process_slot(struct process_counter *table, uint64_t key)
{
	for (unsigned step = 0; step < UKE_PROCESS_SLOTS; ++step) {
		struct process_counter *slot = &table[(key + step) & (UKE_PROCESS_SLOTS - 1)];
		if (!slot->key || slot->key == key) {
			return slot;
		}
	}
	return NULL;
}

static int
sample_process(proc_t proc, void *context)
{
	struct process_window *window = context;
	task_t task = proc_task(proc);
	if (!task) {
		return PROC_RETURNED;
	}
	uint64_t key = proc_uniqueid(proc) + 1;
	struct process_counter *after = process_slot(window->after, key);
	if (!after) {
		++window->missed_after;
		return PROC_RETURNED;
	}
	const struct recount_times_mach times = recount_task_times(task);
	const uint64_t total = times.rtm_user + times.rtm_system;
	*after = (struct process_counter){.key = key, .time = total};
	++window->count;
	if (!window->before) {
		return PROC_RETURNED;
	}
	const struct process_counter *before = process_slot(window->before, key);
	const bool seen = before && before->key == key;
	if (!seen && window->missed_before) {
		return PROC_RETURNED;
	}
	// New processes contribute only their lifetime since the previous sample.
	const uint64_t used = seen && total >= before->time ? total - before->time : total;
	if (!used) {
		return PROC_RETURNED;
	}
	for (unsigned rank = 0; rank < UKE_TOP_COUNT; ++rank) {
		if (used <= window->top[rank].time) {
			continue;
		}
		for (unsigned i = UKE_TOP_COUNT - 1; i > rank; --i) {
			window->top[i] = window->top[i - 1];
		}
		window->top[rank].time = used;
		window->top[rank].pid = proc_pid(proc);
		strlcpy(window->top[rank].name, proc_best_name(proc), sizeof(window->top[rank].name));
		break;
	}
	return PROC_RETURNED;
}

struct cpu_sample {
	natural_t ticks[UKE_CPU_COUNT][CPU_STATE_MAX];
	unsigned valid;
};

static void
sample_cpus(struct cpu_sample *sample)
{
	*sample = (struct cpu_sample){0};
	for (unsigned cpu = 0; cpu < UKE_CPU_COUNT; ++cpu) {
		processor_t processor = cpu_to_processor((int)cpu);
		if (processor) {
			processor_cpu_load_info(processor, sample->ticks[cpu]);
			sample->valid |= 1u << cpu;
		}
	}
}

static void
log_cpus(unsigned seq, const struct cpu_sample *before, const struct cpu_sample *after)
{
	unsigned busy_pm[UKE_CPU_COUNT] = {0};
	unsigned valid = before->valid & after->valid;
	uint64_t busy_sum = 0, total_sum = 0;
	for (unsigned cpu = 0; cpu < UKE_CPU_COUNT; ++cpu) {
		if (!(valid & (1u << cpu))) {
			continue;
		}
		natural_t diff[CPU_STATE_MAX];
		for (unsigned state = 0; state < CPU_STATE_MAX; ++state) {
			diff[state] = after->ticks[cpu][state] - before->ticks[cpu][state];
		}
		const uint64_t busy = (uint64_t)diff[CPU_STATE_USER] + diff[CPU_STATE_SYSTEM] + diff[CPU_STATE_NICE];
		const uint64_t total = busy + diff[CPU_STATE_IDLE];
		if (!total) {
			valid &= ~(1u << cpu);
			continue;
		}
		busy_pm[cpu] = (unsigned)(busy * 1000 / total);
		busy_sum += busy;
		total_sum += total;
	}
	IOLog("UKE SYS CPU: seq=%u busy_pm=%u cores_valid=0x%x core_pm=%u/%u/%u/%u/%u/%u/%u/%u\n",
	    seq, total_sum ? (unsigned)(busy_sum * 1000 / total_sum) : 0, valid,
	    busy_pm[0], busy_pm[1], busy_pm[2], busy_pm[3], busy_pm[4], busy_pm[5], busy_pm[6], busy_pm[7]);
	uint32_t state_mhz[3] = {0}, vote_mhz[3] = {0};
	const unsigned domains = sheng_cpufreq_snapshot(state_mhz, vote_mhz);
	IOLog("UKE SYS FREQ: seq=%u domains_valid=0x%x state_mhz=%u/%u/%u vote_mhz=%u/%u/%u\n",
	    seq, domains, state_mhz[0], state_mhz[1], state_mhz[2], vote_mhz[0], vote_mhz[1], vote_mhz[2]);
}

static void
log_memory(unsigned seq)
{
	vm_statistics64_data_t vm = {0};
	mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
	kern_return_t status = host_statistics64(host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &count);
	if (status != KERN_SUCCESS) {
		IOLog("UKE SYS MEM: seq=%u status=%d\n", seq, status);
		return;
	}
	IOLog("UKE SYS MEM: seq=%u ram_mib=%llu page_bytes=%lu free=%u active=%u inactive=%u wired=%u speculative=%u purgeable=%u file=%u anon=%u compressor=%u uncompressed=%llu\n",
	    seq, sane_size / UKE_MIB, (unsigned long)PAGE_SIZE, vm.free_count, vm.active_count, vm.inactive_count,
	    vm.wire_count, vm.speculative_count, vm.purgeable_count, vm.external_page_count, vm.internal_page_count,
	    vm.compressor_page_count, vm.total_uncompressed_pages_in_compressor);
	IOLog("UKE SYS SWAP: seq=%u total_bytes=%llu used_bytes=%llu swapins=%llu swapouts=%llu compressions=%llu decompressions=%llu iotier_grants=%llu\n",
	    seq, vm_swap_get_total_space(), vm_swap_get_used_space(), vm.swapins, vm.swapouts,
	    vm.compressions, vm.decompressions, counter_load(&vm_swapin_iotier_grants));
}

static void
system_snapshot_worker(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;
	const size_t bytes = UKE_PROCESS_SLOTS * sizeof(struct process_counter);
	struct process_counter *tables = IOMallocZeroData(2 * bytes);
	if (!tables) {
		IOLog("UKE SYS: process snapshot allocation failed\n");
		return;
	}
	struct process_window window = {.after = tables};
	struct cpu_sample before, after;
	uint64_t interval;
	nanoseconds_to_absolutetime(30ULL * NSEC_PER_SEC, &interval);
	uint64_t previous = mach_absolute_time();
	uint64_t deadline = previous + interval;
	sample_cpus(&before);
	proc_iterate(PROC_ALLPROCLIST | PROC_NOWAITTRANS, sample_process, &window, NULL, NULL);
	window.before = window.after;
	window.after = tables + UKE_PROCESS_SLOTS;
	window.missed_before = window.missed_after;
	IOLog("UKE SYS: enabled period_s=30 boot=%.36s\n", bootsessionuuid_string);

	for (unsigned seq = 1;; ++seq) {
		while (mach_absolute_time() < deadline) {
			if (assert_wait_deadline((event_t)tables, THREAD_UNINT, deadline) == THREAD_WAITING) {
				thread_block(THREAD_CONTINUE_NULL);
			}
		}
		const uint64_t now = mach_absolute_time();
		const uint64_t elapsed = now - previous;
		previous = now;
		uint64_t elapsed_ns = 0;
		absolutetime_to_nanoseconds(elapsed, &elapsed_ns);
		sample_cpus(&after);
		bzero(window.after, bytes);
		bzero(window.top, sizeof(window.top));
		window.count = window.missed_after = 0;
		proc_iterate(PROC_ALLPROCLIST | PROC_NOWAITTRANS, sample_process, &window, NULL, NULL);
		IOLog("UKE SYS: seq=%u window_ms=%llu processes=%u skipped=%u previous_skipped=%u boot=%.36s\n",
		    seq, elapsed_ns / NSEC_PER_MSEC, window.count, window.missed_after, window.missed_before,
		    bootsessionuuid_string);
		log_cpus(seq, &before, &after);
		log_memory(seq);
		for (unsigned rank = 0; rank < UKE_TOP_COUNT && window.top[rank].time; ++rank) {
			const uint64_t cpu_pm = window.top[rank].time * 1000 / elapsed;
			IOLog("UKE SYS TOP: seq=%u rank=%u pid=%d cpu_one_core_pm=%llu proc=%.31s\n",
			    seq, rank + 1, window.top[rank].pid, cpu_pm, window.top[rank].name);
		}
		struct process_counter *old = window.before;
		window.before = window.after;
		window.after = old;
		window.missed_before = window.missed_after;
		before = after;
		const uint64_t end = mach_absolute_time();
		// After a long delay, skip expired periods without emitting a burst.
		deadline += ((end - deadline) / interval + 1) * interval;
	}
}

void
uke_system_snapshot_start(void)
{
	thread_t thread;
	kern_return_t status = kernel_thread_start(system_snapshot_worker, NULL, &thread);
	if (status != KERN_SUCCESS) {
		IOLog("UKE SYS: worker start failed status=%d\n", status);
		return;
	}
	thread_set_thread_name(thread, "uke_system_snapshot");
	thread_deallocate(thread);
}
