/* Draw a kernel panic message, with its arguments, on the frame buffer, then stop.
 *
 * Built freestanding by patch-xnu-entry-probe.py and placed in unused padding
 * of a COPY of the kernel; panic() and its common panic funnel are redirected
 * here. It runs on the kernel's
 * own mapping of the frame buffer. With the board observer it uses the retained
 * GOP mapping prefix; otherwise it uses vinfo.v_baseaddr.
 *
 * panic(fmt, ...) is variadic and the Darwin arm64 ABI passes every variadic
 * argument in an 8-byte stack slot, starting at the stack pointer on entry.
 * A small printf walks the format string over those slots (%s %x %p, with
 * # flag, 0-flag, width, precision and l/ll/z). Kernel strings are checked with an
 * active hardware translation query before reading each byte;
 * unmapped/device/user pointers print as pointers. VINFO names that mapping prefix.
 */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;

struct vc_info {
  u32 v_height, v_width, v_depth, v_rowbytes;
  u64 v_baseaddr;
};

/* 3x cells: at 1x the panic text is unreadable on this panel. The full text does
 * not have to fit on screen -- it is staged to the pram and lands in userdata. */
#define SCALE   3

extern const u64 gFont[];

/* All state lives on the stack: this code sits in the kernel's read-only text,
 * so a writable global would fault on its first store. */
struct ctx {
  u8  *base;
  u32 rb, w, h, x, y;
  u8  *log;   /* buffer that also receives the formatted message, 0 = none */
  u32 logn, logcap;
};

#ifndef PANIC_TEXT
#define PANIC_TEXT 0UL
#endif
#ifndef UKE_PANIC_FLUSH
#define UKE_PANIC_FLUSH 0UL
#endif
/* disableConsoleOutput (kern/printf.c through the platform expert): the console keeps drawing from the other CPUs onto the same frame
 * buffer this screen draws on, so the panic text has to own the panel from here on. */
#ifndef PANIC_QUIET
#define PANIC_QUIET 0UL
#endif
/* The board's panic shadow (kits/uke/LegacyDisplayProbe.cpp): the message is copied there and the probe's heartbeat thread, which
 * keeps running on another CPU, prints it into the log. A plain copy, no variadic call: the Darwin ABI passes variadic arguments on
 * the stack and a call from here reached the kernel's printf with an empty string. */
#ifndef PANIC_SHADOW
#define PANIC_SHADOW 0UL
#endif
#ifndef PANIC_SHADOW_LEN
#define PANIC_SHADOW_LEN 0UL
#endif
#define PANIC_TEXT_BYTES 256
#define PANIC_SAVE_BYTES 1024

__attribute__((noreturn)) static void park (void)
{
  for (;;) {
    __asm__ volatile ("wfe");
  }
}

static void put (struct ctx *g, u8 c)
{
  const u32 cell = 6 * SCALE, line = 14 * SCALE;
  u32 row, col, dy, dx;

  if (g->log && g->logcap && g->logn < g->logcap - 1 && c >= 9 && c <= 126) {
    g->log[g->logn++] = (c < 32) ? ' ' : c;
    g->log[g->logn] = 0;
  }

  if (c == '\n') {
    g->x = 0;
    g->y += line;
    return;
  }
  if (c == '\t') {
    g->x += 4 * cell;
    return;
  }
  if (c < 32 || c > 126) {
    return;
  }
  if (g->x + cell > g->w) {
    g->x = 0;
    g->y += line;
  }
  if (g->y + line > g->h) {
    return;
  }
  u64 glyph = gFont[c - 32];
  u32 top = (u32)(glyph >> 32), bottom = (u32)glyph;
  for (row = 0; row < 12; row++) {
    u32 data = (row < 6) ? (top >> (row * 5)) : (bottom >> ((row - 6) * 5));
    for (col = 0; col < 5; col++) {
      if ((data >> col) & 1) {
        for (dy = 0; dy < SCALE; dy++) {
          for (dx = 0; dx < SCALE; dx++) {
            *(volatile u32 *)(g->base + (u64)(g->y + row * SCALE + dy) * g->rb + (u64)(g->x + col * SCALE + dx) * 4) = 0xFFFFFFFFu;
          }
        }
      }
    }
  }
  g->x += cell;
}

static void number (struct ctx *g, u64 value, u32 base, u32 width, int zero)
{
  u8  digits[24];
  u32 n = 0, i;

  do {
    u32 d = (u32)(value % base);
    digits[n++] = (u8)(d < 10 ? '0' + d : 'a' + d - 10);
    value /= base;
  } while (value && n < 24);
  for (i = n; i < width; i++) {
    put (g, zero ? '0' : ' ');
  }
  while (n) {
    put (g, digits[--n]);
  }
}

/* Probe each string byte before dereferencing it. Heap strings (process name
 * and exit reason) live outside the linked image. AT performs a hardware
 * translation query only: no table edits, mappings, or exception bypass.
 * Refuse user pointers, translation faults and device-memory attributes. */
static int readable (const char *s, const u64 *args)
{
  u64 a = (u64)s, par;
  (void)args;
  if (a < 0xffff000000000000UL) return 0;
  __asm__ volatile ("at s1e1r, %1; isb; mrs %0, par_el1" : "=r" (par) : "r" (a) : "memory");
  return !(par & 1) && ((par >> 60) != 0);
}

static void quiet_console (void)
{
#if PANIC_QUIET
  *(volatile int *)(PANIC_QUIET) = 1;
  __asm__ volatile("dsb sy" ::: "memory");
#endif
}

static void format (struct ctx *g, const char *fmt, const u64 *args)
{
  u32 i, used = 0;

  for (i = 0; i < 2000 && fmt[i]; i++) {
    u8 c = (u8)fmt[i];
    if (c != '%') {
      put (g, c);
      continue;
    }
    u32 width = 0;
    int zero = 0, longs = 0, hash = 0;
    c = (u8)fmt[++i];
    for (;;) {
      if (c == '#') {
        hash = 1;
        c = (u8)fmt[++i];
      } else if (c == '0') {
        zero = 1;
        c = (u8)fmt[++i];
      } else {
        break;
      }
    }
    while (c >= '0' && c <= '9') {
      width = width * 10 + (c - '0');
      c = (u8)fmt[++i];
    }
    u32 precision = 4000;
    if (c == '.') {
      precision = 0;
      c = (u8)fmt[++i];
      if (c == '*') {
        precision = (u32)args[used++];
        c = (u8)fmt[++i];
      } else {
        while (c >= '0' && c <= '9') {
          if (precision < 4000) precision = precision * 10 + (c - '0');
          c = (u8)fmt[++i];
        }
      }
      if (precision > 4000) precision = 4000;
    }
    if (width > 256) width = 256;
    while (c == 'l' || c == 'z' || c == 'h') {
      longs += (c != 'h');
      c = (u8)fmt[++i];
    }
    if (c == '%') {
      put (g, '%');
      continue;
    }
    if (c == 0) {
      break;
    }
    u64 v = args[used++];
    if (c == 's') {
      const char *s = (const char *)v;
      u32 k;
      for (k = 0; k < precision; k++) {
        if (!readable (s + k, args)) {
          put (g, '<');
          number (g, (u64)(s + k), 16, 0, 0);
          put (g, '>');
          break;
        }
        u8 ch = (u8)s[k];
        if (!ch) break;
        put (g, ch);
      }
    } else if (c == 'x' || c == 'X') {
      if (hash) {
        put (g, '0');
        put (g, 'x');
      }
      number (g, longs ? v : (v & 0xFFFFFFFFu), 16, width, zero);
    } else if (c == 'u' || c == 'd') {
      number (g, longs ? v : (v & 0xFFFFFFFFu), 10, width, zero);
    } else if (c == 'p') {
      put (g, '0');
      put (g, 'x');
      number (g, v, 16, width, zero);
    }
  }
}

static void text (struct ctx *g, const char *s)
{
  while (*s) {
    put (g, (u8)*s++);
  }
}

static void hex (struct ctx *g, const char *label, u64 v)
{
  text (g, label);
  put (g, '0');
  put (g, 'x');
  number (g, v, 16, 16, 1);
}

/* x0 = fmt as for panic(); x1 is set by the stub to the entry stack pointer. */
__attribute__((noreturn, used, section(".text.b_main"))) void panic_screen_main (const char *fmt, const u64 *args)
{
  volatile struct vc_info *v = (volatile struct vc_info *)VINFO;
  struct ctx g;
  u32 row, col, saved, saved_h;
  u8 save[PANIC_SAVE_BYTES];
  u8 *banner = 0;

  __asm__ volatile ("msr daifset, #15");
  g.base = (u8 *)v->v_baseaddr;
  g.rb   = v->v_rowbytes;
  g.w    = v->v_width;
  g.h    = v->v_height;
  if (g.base == 0 || v->v_depth != 32 || fmt == 0) {
    park ();
  }

  /* First panic wins: a marker in the last row's first two pixels means a panic already drew
   * the screen; a later panic (another thread or exception) must not overwrite it. */
  volatile u32 *mark = (volatile u32 *)(g.base + (u64)(g.h - 1) * g.rb);
  if (mark[0] == 0xC0FFEE11u && mark[1] == 0x5CEEDA7Au) {
    park ();
  }
  g.x = 0;
  g.y = g.h / 8;
  g.log = 0;
  g.logn = 0;
  g.logcap = 0;
  save[0] = 0;
  for (row = g.y; row < g.h; row++) {
    for (col = 0; col < g.w; col++) {
      *(volatile u32 *)(g.base + (u64)row * g.rb + (u64)col * 4) = 0;
    }
  }
  g.y += 8;
  /* The board's live banner keeps showing this text after the next swap paints over the screen (BootProgress.c,
   * boot_progress_panic_text). The first panic's text is kept: a buffer that already holds one is left alone. */
  if (PANIC_TEXT && *(volatile u8 *)PANIC_TEXT == 0) {
    banner = (u8 *)PANIC_TEXT;
  }
  quiet_console ();
  g.log = save;
  g.logn = 0;
  g.logcap = sizeof(save);
  /* First pass formats the whole message into the buffer and draws nothing: the stack after the assertion would fill the panel and
   * anything drawn over it would collide with it. The stack still reaches the pram through the flush below. */
  saved_h = g.h;
  g.h = 0;
  format (&g, fmt, args);
  saved = g.logn;
  g.h = saved_h;
#if PANIC_TEXT
  /* The board's boot-page banner buffer, for kernels that have one (the GPU configuration has no BootProgress: PANIC_TEXT is 0 and
   * this copy is compiled out, which is what keeps the screen inside the kernel's padding). */
  if (banner) {
    u32 n = saved < PANIC_TEXT_BYTES - 1 ? saved : PANIC_TEXT_BYTES - 1;
    u32 i;
    for (i = 0; i < n; i++) {
      banner[i] = save[i];
    }
    banner[n] = 0;
  }
#endif
  __asm__ volatile ("dsb sy");
  /* Sample the active CPU translation regime, without changing it. This is
   * panic-time evidence only; it does not establish early mappings or EL0 ABI. */
  /* The register and memory samples this screen once printed (TCR, SCTLR, pages, free) are gone: the code has to stay inside the
   * kernel's padding and what a panic on this board is read for is the message's own first lines, which the block below redraws. The
   * memory counters still reach the log from the probe's UKE MEM rows while the system is alive. */
  g.log = 0;
  /* The message, drawn once and alone from its start: the assertion and its first frames are what a photograph of a panic is read
   * for. Whatever does not fit the panel is still in the pram (the flush below) and in the next boot's log. */
  if (saved) {
    u32 i;
    g.x = 0;
    g.y = 0;
    for (i = 0; i < saved; i++) {
      put (&g, (u8)save[i]);
    }
    __asm__ volatile ("dsb sy");
  }
  /* The raw argument slots are not dumped: the formatted message above already carries them. */
  mark[0] = 0xC0FFEE11u;
  mark[1] = 0x5CEEDA7Au;
  __asm__ volatile ("dsb sy");
#if PANIC_SHADOW
  if (saved) {
    volatile char *shadow = (volatile char *)PANIC_SHADOW;
    u32 n = saved < 1023 ? saved : 1023, i;
    for (i = 0; i < n; i++) {
      shadow[i] = save[i];
    }
    shadow[n] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
#if PANIC_SHADOW_LEN
    *(volatile u32 *)PANIC_SHADOW_LEN = n;
#endif
  }
#endif
  /* Hand the saved text to the boot-log writer's pram staging block. The marker
   * above is already set, so a fault here parks a nested panic instead of
   * repainting over this one. */
#if UKE_PANIC_FLUSH
  if (saved) {
    ((void (*)(const char *))UKE_PANIC_FLUSH)((const char *)save);
  }
#endif
  park ();
}

/* Entry from panic(): keep the stack pointer, which addresses the variadic arguments. */
__attribute__((naked, noreturn, section(".text.a_entry"))) void panic_screen (const char *fmt)
{
  __asm__ volatile ("mov x1, sp\n\tb panic_screen_main");
}

/* The real panic_trap_to_debugger signature takes (fmt, va_list *, ...).
 * Darwin ARM64 va_list is a stack pointer; its address is passed in x1.
 * The wrapper has already established its frame, so SP is NOT the argument
 * list here. Follow exactly the one pointer supplied by the real wrapper.
 */
__attribute__((naked, noreturn, section(".text.a_common"))) void panic_screen_common (void)
{
  __asm__ volatile ("ldr x1, [x1]\n\tb panic_screen_main");
}

#include "Font.h"
__attribute__((section(".text.z_font"))) const u64 gFont[] = GLYPH_FONT;
