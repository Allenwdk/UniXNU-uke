// SPDX-License-Identifier: MIT
// Mirror userland crash reports into the kernel message buffer. TWRP cannot
// mount the APFS Data volume, so the reports are unreadable from recovery;
// the UkeLastLog writer persists msgbuf tails to the userdata window instead.
// Reports are detected by file name, not mtime: the device clock sits at the
// epoch, so timestamps from earlier boots sort unpredictably. The probe also
// prints the launch paths of the Settings app and looks for its Data install,
// because a process that never execs leaves no crash report at all. Enabled
// with sheng-crashlog=1; started from the UkeLastLog startup entry so the
// archive member is linked.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/namei.h>
#include <sys/vnode_internal.h>
#include <sys/dirent.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <pexpert/pexpert.h>

extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation,
    void *parameter, int priority, thread_t *new_thread);
#define BASEPRI_PREEMPT 92

#define UKE_CRASH_DIR "/private/var/mobile/Library/Logs/CrashReporter"
#define UKE_APP_DIR "/private/var/containers/Bundle/Application"
#define UKE_CRASH_DIR_BYTES 4096
#define UKE_CRASH_READ_BYTES 8192
#define UKE_CRASH_SEEN 512
#define UKE_CRASH_LIST 24

struct uke_crash_scan_state {
	unsigned listed;
	unsigned total;
	unsigned newPrints;
	int baseline;
};

static uint8_t ukeCrashDirents[UKE_CRASH_DIR_BYTES] __attribute__((aligned(8)));
static char ukeCrashText[UKE_CRASH_READ_BYTES + 1] __attribute__((aligned(8)));
#define UKE_CRASH_FULL_BYTES (640 * 1024)
static char ukeCrashFull[UKE_CRASH_FULL_BYTES] __attribute__((aligned(8)));
#define UKE_CRASH_WINDOW_BYTES (12 * 1024)
static char ukeCrashWindow[UKE_CRASH_WINDOW_BYTES + 1] __attribute__((aligned(8)));
static uint64_t ukeCrashSeen[UKE_CRASH_SEEN];
static unsigned ukeCrashSeenCount;

static void
uke_crash_copy(char *dst, size_t cap, const char *src)
{
	size_t at = 0;
	while (src[at] && at + 1 < cap) {
		dst[at] = src[at];
		at++;
	}
	dst[at] = '\0';
}

static int
uke_crash_ips(const char *name)
{
	size_t length = strlen(name);
	return length > 4 && !strcmp(name + length - 4, ".ips");
}

static uint64_t
uke_crash_hash(const char *name)
{
	uint64_t hash = 1469598103934665603ULL;
	while (*name) {
		hash ^= (unsigned char)*name++;
		hash *= 1099511628211ULL;
	}
	return hash;
}

static int
uke_crash_seen(uint64_t hash)
{
	for (unsigned i = 0; i < ukeCrashSeenCount; i++) {
		if (ukeCrashSeen[i] == hash) {
			return 1;
		}
	}
	return 0;
}

static void
uke_crash_remember(uint64_t hash)
{
	if (ukeCrashSeenCount < UKE_CRASH_SEEN) {
		ukeCrashSeen[ukeCrashSeenCount++] = hash;
	}
}

typedef void (*uke_crash_visit)(vfs_context_t ctx, vnode_t dir, struct direntry *entry, void *arg);

static int
uke_crash_walk(vfs_context_t ctx, const char *path, uke_crash_visit visit, void *arg, unsigned limit)
{
	vnode_t dir = NULLVP;
	int error = vnode_open(path, FREAD, 0, 0, &dir, ctx);
	if (error) {
		return error;
	}
	off_t offset = 0;
	int eof = 0;
	unsigned seen = 0;
	while (!eof && (!limit || seen < limit)) {
		uio_t uio = uio_create(1, offset, UIO_SYSSPACE, UIO_READ);
		if (uio == NULL) {
			error = ENOMEM;
			break;
		}
		uio_addiov(uio, CAST_USER_ADDR_T(ukeCrashDirents), UKE_CRASH_DIR_BYTES);
		int num = 0;
		error = vnode_readdir64(dir, uio, VNODE_READDIR_EXTENDED, &eof, &num, ctx);
		uint32_t filled = (uint32_t)(UKE_CRASH_DIR_BYTES - (int)uio_resid(uio));
		offset = uio_offset(uio);
		uio_free(uio);
		if (error || filled == 0) {
			break;
		}
		for (uint32_t at = 0; at + offsetof(struct direntry, d_name) < filled && (!limit || seen < limit); ) {
			struct direntry *entry = (struct direntry *)(ukeCrashDirents + at);
			if (entry->d_reclen == 0 || at + entry->d_reclen > filled) {
				break;
			}
			at += entry->d_reclen;
			if (entry->d_ino == 0 || !strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
				continue;
			}
			visit(ctx, dir, entry, arg);
			seen++;
		}
	}
	vnode_close(dir, FREAD, ctx);
	return error;
}

static int
uke_crash_stat_child(vfs_context_t ctx, vnode_t dir, const char *name, long long *mtime, long long *size)
{
	vnode_t child = NULLVP;
	if (vnode_lookupat(name, VNODE_LOOKUP_NOFOLLOW | VNODE_LOOKUP_NOCROSSMOUNT, &child, ctx, dir)) {
		return -1;
	}
	struct vnode_attr va;
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_data_size);
	VATTR_WANTED(&va, va_modify_time);
	int error = vnode_getattr(child, &va, ctx);
	enum vtype type = vnode_vtype(child);
	vnode_put(child);
	if (error || type != VREG) {
		return -1;
	}
	*mtime = VATTR_IS_SUPPORTED(&va, va_modify_time) ? (long long)va.va_modify_time.tv_sec : 0;
	*size = VATTR_IS_SUPPORTED(&va, va_data_size) ? (long long)va.va_data_size : 0;
	return 0;
}

static void
uke_crash_show(vfs_context_t ctx, const char *name, long long mtime, long long size)
{
	char path[192];
	uke_crash_copy(path, sizeof path, UKE_CRASH_DIR "/");
	uke_crash_copy(path + strlen(path), sizeof path - strlen(path), name);
	vnode_t vp = NULLVP;
	int error = vnode_open(path, FREAD, 0, 0, &vp, ctx);
	if (error) {
		printf("UkeCrash: open %s error %d\n", path, error);
		return;
	}
	int resid = 0;
	error = vn_rdwr(UIO_READ, vp, (caddr_t)ukeCrashText, UKE_CRASH_READ_BYTES, 0, UIO_SYSSPACE,
	    IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	int length = error ? 0 : UKE_CRASH_READ_BYTES - resid;
	vnode_close(vp, FREAD, ctx);
	if (error || length <= 0) {
		printf("UkeCrash: read %s error %d\n", name, error);
		return;
	}
	ukeCrashText[length] = '\0';
	printf("UkeCrash: BEGIN %s mtime %lld size %lld read %d\n%s\nUkeCrash: END %s\n",
	    name, mtime, size, length, ukeCrashText, name);
}

static void
uke_crash_scan_visit(vfs_context_t ctx, vnode_t dir, struct direntry *entry, void *arg)
{
	struct uke_crash_scan_state *state = arg;
	const char *name = entry->d_name;
	if (!uke_crash_ips(name)) {
		return;
	}
	long long mtime = 0, size = 0;
	if (uke_crash_stat_child(ctx, dir, name, &mtime, &size)) {
		return;
	}
	state->total++;
	if (state->listed < UKE_CRASH_LIST) {
		printf("UkeCrash: entry %s mtime %lld size %lld\n", name, mtime, size);
		state->listed++;
	}
	uint64_t hash = uke_crash_hash(name);
	if (uke_crash_seen(hash)) {
		return;
	}
	uke_crash_remember(hash);
	if (!state->baseline && state->newPrints < 2) {
		uke_crash_show(ctx, name, mtime, size);
		state->newPrints++;
	}
}

// First scan: reports that matter for the current bring-up, whether or not
// they predate this boot (the Settings and compositor crashes repeat across
// boots and their reports persist in the directory). One walk per prefix so
// the compositor reports are not crowded out by the Settings ones, and a
// pause after each print so every report gets its own userdata snapshot
// (the writer copies the msgbuf tail every two seconds).
struct uke_crash_first_state {
	const char *prefix;
	unsigned prints;
};

static void
uke_crash_sleep(unsigned ms);

static void
uke_crash_first_visit(vfs_context_t ctx, vnode_t dir, struct direntry *entry, void *arg)
{
	struct uke_crash_first_state *state = arg;
	const char *name = entry->d_name;
	if (state->prints >= 2 || !uke_crash_ips(name)) {
		return;
	}
	if (strncmp(name, state->prefix, strlen(state->prefix)) != 0) {
		return;
	}
	long long mtime = 0, size = 0;
	if (uke_crash_stat_child(ctx, dir, name, &mtime, &size)) {
		return;
	}
	uke_crash_show(ctx, name, mtime, size);
	state->prints++;
	uke_crash_sleep(2500);
}

// The persisted Preferences report carries the block that stalled the scene;
// print the stackshot symbol names so the log names the call (APFS, mach_msg,
// an XPC service) without the 470-648 KiB JSON.
static const char *
uke_crash_find(const char *hay, const char *end, const char *needle, size_t nlen)
{
	for (const char *p = hay; p + nlen <= end; p++) {
		if (!memcmp(p, needle, nlen)) {
			return p;
		}
	}
	return NULL;
}

static int
uke_crash_symbol_value(const char *at, const char *end, char *out, size_t cap)
{
	while (at < end && *at != ':') {
		at++;
	}
	if (at >= end) {
		return 0;
	}
	at++;
	while (at < end && (*at == ' ' || *at == '\t')) {
		at++;
	}
	if (at >= end || *at != '"') {
		return 0;
	}
	at++;
	size_t n = 0;
	while (at < end && *at != '"' && n + 1 < cap) {
		out[n++] = *at++;
	}
	out[n] = '\0';
	return n > 0;
}

static void
uke_crash_symbols(vfs_context_t ctx, const char *name)
{
	char path[192];
	uke_crash_copy(path, sizeof path, UKE_CRASH_DIR "/");
	uke_crash_copy(path + strlen(path), sizeof path - strlen(path), name);
	vnode_t vp = NULLVP;
	int error = vnode_open(path, FREAD, 0, 0, &vp, ctx);
	if (error) {
		printf("UkeCrash: symbols open %s error %d\n", name, error);
		return;
	}
	int resid = 0;
	error = vn_rdwr(UIO_READ, vp, (caddr_t)ukeCrashFull, UKE_CRASH_FULL_BYTES, 0, UIO_SYSSPACE,
	    IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
	int length = error ? 0 : UKE_CRASH_FULL_BYTES - resid;
	vnode_close(vp, FREAD, ctx);
	if (error || length <= 0) {
		printf("UkeCrash: symbols read %s error %d\n", name, error);
		return;
	}
	const char *end = ukeCrashFull + length;
	// The report names its faulting thread by index; its frames sit past the
	// first thread, so print windows 8-32 KiB in two snapshots.
	for (unsigned chunk = 0; chunk < 2; chunk++) {
		size_t off = 8192 + (size_t)chunk * UKE_CRASH_WINDOW_BYTES;
		if (off >= (size_t)length) {
			break;
		}
		size_t n = (size_t)length - off;
		if (n > UKE_CRASH_WINDOW_BYTES) {
			n = UKE_CRASH_WINDOW_BYTES;
		}
		memcpy(ukeCrashWindow, ukeCrashFull + off, n);
		ukeCrashWindow[n] = '\0';
		printf("UkeCrash: window off=%lu\n%s\n", (unsigned long)off, ukeCrashWindow);
		uke_crash_sleep(2500);
	}
	// The frames may not be symbolicated (no "symbol" keys), so print the raw
	// frames and usedImages excerpts too; the image names plus the offsets
	// name the library a blocked frame lives in.
	static char ukeCrashExcerpt[4600];
	static const struct { const char *key; size_t len; unsigned cap; } keys[] = {
		{ "\"frames\"", 8, 4096 },
		{ "\"usedImages\"", 12, 2048 },
		{ "\"symbol\"", 8, 512 },
	};
	for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
		const char *hit = uke_crash_find(ukeCrashFull, end, keys[i].key, keys[i].len);
		if (hit == NULL) {
			printf("UkeCrash: key %s absent\n", keys[i].key);
			continue;
		}
		size_t n = (size_t)(end - (hit + keys[i].len));
		if (n > keys[i].cap) {
			n = keys[i].cap;
		}
		memcpy(ukeCrashExcerpt, hit, n);
		ukeCrashExcerpt[n] = '\0';
		printf("UkeCrash: key %s at %ld\n%s\n", keys[i].key, (long)(hit - ukeCrashFull), ukeCrashExcerpt);
	}
	static uint64_t seen[96];
	unsigned seenCount = 0, printed = 0;
	const char *p = ukeCrashFull;
	while ((p = uke_crash_find(p, end, "\"symbol\"", 8)) != NULL && printed < 80) {
		char sym[96];
		if (uke_crash_symbol_value(p + 8, end, sym, sizeof sym)) {
			uint64_t hash = uke_crash_hash(sym);
			int known = 0;
			for (unsigned i = 0; i < seenCount; i++) {
				if (seen[i] == hash) {
					known = 1;
					break;
				}
			}
			if (!known) {
				if (seenCount < sizeof seen / sizeof seen[0]) {
					seen[seenCount++] = hash;
				}
				printf("UkeCrash: sym %u %s\n", printed, sym);
				printed++;
			}
		}
		p += 8;
	}
	printf("UkeCrash: symbols %s printed=%u bytes=%d\n", name, printed, length);
}

// Newest Preferences report by name (the device clock is at the epoch, the
// names still order the reports; the newest one is the latest stall).
struct uke_crash_pref_state {
	char name[128];
	long long mtime;
	long long size;
};

static void
uke_crash_pref_visit(vfs_context_t ctx, vnode_t dir, struct direntry *entry, void *arg)
{
	struct uke_crash_pref_state *state = arg;
	const char *name = entry->d_name;
	if (!uke_crash_ips(name) || strncmp(name, "Preferences-", 12) != 0) {
		return;
	}
	if (state->name[0] && strcmp(name, state->name) <= 0) {
		return;
	}
	long long mtime = 0, size = 0;
	if (uke_crash_stat_child(ctx, dir, name, &mtime, &size)) {
		return;
	}
	uke_crash_copy(state->name, sizeof state->name, name);
	state->mtime = mtime;
	state->size = size;
}

static void
uke_crash_list_visit(vfs_context_t ctx, vnode_t dir, struct direntry *entry, void *arg)
{
	(void)ctx;
	(void)dir;
	printf("UkeCrash: %s/%s\n", (const char *)arg, entry->d_name);
}

static void
uke_crash_stat(vfs_context_t ctx, const char *path)
{
	vnode_t vp = NULLVP;
	int error = vnode_lookup(path, 0, &vp, ctx);
	if (error) {
		printf("UkeCrash: stat %s error %d\n", path, error);
		return;
	}
	struct vnode_attr va;
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_mode);
	VATTR_WANTED(&va, va_data_size);
	int gerror = vnode_getattr(vp, &va, ctx);
	printf("UkeCrash: stat %s type %d mode %o size %lld error %d\n", path, (int)vnode_vtype(vp),
	    (unsigned)(VATTR_IS_SUPPORTED(&va, va_mode) ? (va.va_mode & 07777) : 0u),
	    (long long)(VATTR_IS_SUPPORTED(&va, va_data_size) ? va.va_data_size : 0), gerror);
	vnode_put(vp);
}

// Does a Data bundle container hold the app? Prints the first hit per app.
struct uke_crash_container_state {
	unsigned containers;
	int pref;
	int clock;
};

static void
uke_crash_container_visit(vfs_context_t ctx, vnode_t dir, struct direntry *entry, void *arg)
{
	(void)dir;
	struct uke_crash_container_state *state = arg;
	const char *suffixes[] = { "Preferences.app/Preferences", "MobileTimer.app/MobileTimer" };
	int *found[] = { &state->pref, &state->clock };
	const char *labels[] = { "settings", "clock" };
	char path[256];
	uke_crash_copy(path, sizeof path, UKE_APP_DIR "/");
	uke_crash_copy(path + strlen(path), sizeof path - strlen(path), entry->d_name);
	size_t at = strlen(path);
	state->containers++;
	for (unsigned i = 0; i < 2; i++) {
		if (*found[i] || at + 1 + strlen(suffixes[i]) >= sizeof path) {
			continue;
		}
		path[at] = '/';
		uke_crash_copy(path + at + 1, sizeof path - at - 1, suffixes[i]);
		vnode_t vp = NULLVP;
		if (!vnode_lookup(path, 0, &vp, ctx)) {
			printf("UkeCrash: installed %s %s\n", labels[i], path);
			vnode_put(vp);
			*found[i] = 1;
		}
		path[at] = '\0';
	}
}

static void
uke_crash_scan(int *baseline)
{
	vfs_context_t ctx = vfs_context_kernel();
	struct uke_crash_scan_state state;
	memset(&state, 0, sizeof state);
	state.baseline = !*baseline;
	int error = uke_crash_walk(ctx, UKE_CRASH_DIR, uke_crash_scan_visit, &state, 0);
	if (error) {
		if (!*baseline) {
			printf("UkeCrash: %s walk %d\n", UKE_CRASH_DIR, error);
		}
		return;
	}
	if (!*baseline) {
		printf("UkeCrash: ready entries=%u seen=%u\n", state.total, ukeCrashSeenCount);
		uke_crash_walk(ctx, "/Applications", uke_crash_list_visit, "/Applications", 64);
		uke_crash_stat(ctx, "/Applications/Preferences.app");
		uke_crash_stat(ctx, "/Applications/Preferences.app/Preferences");
		uke_crash_stat(ctx, UKE_APP_DIR);
		struct uke_crash_container_state containers = {0};
		uke_crash_walk(ctx, UKE_APP_DIR, uke_crash_container_visit, &containers, 512);
		printf("UkeCrash: containers=%u settings=%d clock=%d\n", containers.containers,
		    containers.pref, containers.clock);
		static const char *const firstPrefixes[] = { "backboardd-", "SpringBoard-" };
		for (unsigned i = 0; i < sizeof firstPrefixes / sizeof firstPrefixes[0]; i++) {
			struct uke_crash_first_state first = { firstPrefixes[i], 0 };
			uke_crash_walk(ctx, UKE_CRASH_DIR, uke_crash_first_visit, &first, 0);
			printf("UkeCrash: first %s prints=%u\n", firstPrefixes[i], first.prints);
		}
		struct uke_crash_pref_state pref = {{0}, 0, 0};
		uke_crash_walk(ctx, UKE_CRASH_DIR, uke_crash_pref_visit, &pref, 0);
		if (pref.name[0]) {
			printf("UkeCrash: newest Preferences %s\n", pref.name);
			uke_crash_show(ctx, pref.name, pref.mtime, pref.size);
			uke_crash_symbols(ctx, pref.name);
			uke_crash_sleep(2500);
		}
		*baseline = 1;
		return;
	}
	if (state.newPrints || state.total != state.listed) {
		printf("UkeCrash: scan entries=%u new=%u\n", state.total, state.newPrints);
	}
}

static void
uke_crash_sleep(unsigned ms)
{
	assert_wait_timeout((event_t)&uke_crash_scan, THREAD_UNINT, ms, NSEC_PER_MSEC);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
uke_crash_log_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;

	int baseline = 0;
	uke_crash_sleep(60000);
	for (;;) {
		uke_crash_scan(&baseline);
		uke_crash_sleep(90000);
	}
}

void
uke_crash_log_start(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("sheng-crashlog", &enabled, (int)sizeof(enabled)) || enabled != 1) {
		return;
	}
	if (kernel_thread_start_priority(uke_crash_log_thread, NULL, BASEPRI_PREEMPT, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
