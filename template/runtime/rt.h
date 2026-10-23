/* Runtime API (queue P2-8): memory map, fail-loud policy, I/O device registration, guest-function dispatch, EXE loading.
 * Policy (docs/decisions/0002 section 3.1, ai-rules/01 rule 3): anything the project has not proven is used ABORTS with a
 * message; nothing is silently ignored. The runtime is single-instance: the device and function tables are process-global. */
#ifndef PS1_RT_H
#define PS1_RT_H
#include <stddef.h>
#include <stdint.h>
#include "cpu.h"

/* ---- fail-loud ---- */
typedef void (*RtFatalFn)(const char *msg);
void rt_set_fatal_handler(RtFatalFn fn);   /* tests install a longjmp; if the handler returns the process still aborts */
void rt_fatal(const Cpu *c, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));

#define RT_YIELD_PERIOD 40000      /* taken backward branches per virtual frame; the oracle uses the same number (calibrated against a reference emulator in the ports) */
void rt_set_max_ticks(unsigned n);   /* test harness: abort (fatal) when more than n virtual frames have passed; 0 = no limit */
void rt_dma_kick(Cpu *c);        /* a DMA completion interrupt is queued: shortens the current budget so it is delivered soon (runtime/hle.c) */
void rt_yield(Cpu *c);           /* called by generated code when c->yield reaches 0: advances virtual time and delivers interrupts (runtime/hle.c) */

/* ---- lifetime ---- */
void rt_init(Cpu *c);                       /* zero the Cpu, allocate guest RAM (zero-filled: a deterministic choice, NOT hardware behaviour) */
void rt_shutdown(Cpu *c);
void rt_set_ram_mirrors(Cpu *c, int allow); /* default 0: accesses to 0x200000..0x7fffff abort (mirror use is unproven) */
void rt_allow_low_ram(Cpu *c, uint32_t lo, uint32_t hi, const char *why);   /* physical [lo,hi) inside the first 64 KiB that evidence proves the game itself uses (finding 0019); max 8 ranges */

/* ---- code protection (self-modifying code, finding 0023): stores into words that recompiled code covers abort unless the exact word is an allowed patch ---- */
/* overlays loaded at run time (e.g. CINE.EXR relocated by the game's own loader): registered here, activated lazily by the first call into [base,base+size) after check(c) confirmed the relocated code is in RAM */
void rt_overlay_add(const char *name, uint32_t base, uint32_t size, void (*code)(void), void (*hooks)(void), void (*reg)(void), int (*check)(Cpu *));
void rt_set_tick_hook(void (*fn)(Cpu *, void *), void *ud);   /* called once per virtual frame (after the hardware model advanced), before pending BIOS events and interrupts are delivered */
void rt_set_code_store_hook(int (*fn)(Cpu *, uint32_t));
void rt_code_clear_range(uint32_t lo, uint32_t hi);
void rt_fn_remove_range(uint32_t lo, uint32_t hi);          /* unregister every function whose entry is in [lo,hi) (guest addresses) */
void rt_code_range(uint32_t lo, uint32_t hi);                 /* mark [lo,hi) (guest addresses, word aligned) as recompiled code */
void rt_patch_allow(uint32_t addr, unsigned n, const uint32_t *words);   /* word stores of one of these values to addr are allowed */
void rt_code_reset(void);
void rt_code_clear_above(uint32_t addr);                       /* forget code ranges and patches at or above addr (executable switch) */

/* ---- host-side memory access (loaders, tests): same checks as guest accesses, RAM and scratchpad only ---- */
void rt_mem_write(Cpu *c, uint32_t addr, const void *src, size_t n);
void rt_mem_read(Cpu *c, uint32_t addr, void *dst, size_t n);

/* ---- I/O and expansion-2 region (physical 0x1f801000..0x1f803fff): nothing is mapped until a device is registered ---- */
typedef struct RtIoDev {
    const char *name;                       /* static string */
    uint32_t lo, hi;                        /* physical byte range [lo, hi) */
    uint32_t (*rd)(void *ctx, Cpu *c, uint32_t phys, unsigned size);                 /* returns the value zero-extended to `size` bytes */
    void (*wr)(void *ctx, Cpu *c, uint32_t phys, unsigned size, uint32_t v);
    void *ctx;
} RtIoDev;
void rt_io_register(const RtIoDev *dev);    /* aborts on overlap, bad range, full table */
void rt_io_reset(void);                     /* tests only */
void rt_io_attach_hw(Cpu *c);               /* registers the register model of runtime/io.c over the whole I/O window; anything unproven aborts with its message */
struct IoCtx *rt_io_ctx(Cpu *c);            /* the model attached to c (NULL if none) */

/* ---- guest function table: generated functions and HLE hooks, keyed by guest address (segment bits ignored) ---- */
typedef void (*RtFn)(Cpu *c);
typedef enum { RT_FN_GENERATED = 1, RT_FN_HLE = 2 } RtFnKind;
void rt_fn_register(uint32_t addr, RtFn fn, const char *name, RtFnKind kind);   /* aborts on duplicate address or unaligned address */
void rt_fn_register_p(uint32_t addr, RtFn fn, const char *name, RtFnKind kind, uint32_t param, uint32_t param2);   /* same, with a hook parameter */
const char *rt_fn_name(uint32_t addr);      /* NULL if unregistered */
size_t rt_fn_count(void);
void rt_fn_reset(void);                     /* tests only */
void rt_fn_reset_above(uint32_t phys);      /* drops every entry at or above this physical address (executable switch) */
void rt_interp_call(Cpu *c, uint32_t target);     /* interpreter tier (runtime/interp.c): runs guest code that has no generated function until it returns to $ra */
unsigned long long rt_interp_count(void);
unsigned rt_interp_depth(void);                  /* nesting of interpreter sessions (saved/restored around the longjmp that leaves an interrupt handler) */
void rt_interp_set_depth(unsigned d);
void rt_interp_dump(unsigned n);                 /* debug aid: the last interpreted pcs */        /* instructions interpreted so far */
void rt_call(Cpu *c, uint32_t target);      /* indirect call (jalr / jr rX): aborts on unaligned or unregistered target */

/* ---- raw blob placement (relocated overlay image, queue P2-5) ---- */
void rt_load_raw(Cpu *c, const char *path, uint32_t addr);    /* copies the whole file to guest RAM at addr (same checks as any guest write) */

/* ---- PS-X EXE loading (header fields: pc 0x10, gp 0x14, load address 0x18, size 0x1c, SP base 0x30, SP offset 0x34 (psx-spx, offsets checked in finding 0003), text at 0x800) ---- */
typedef struct { uint32_t pc, gp, addr, size, sp_base, sp_off; } RtExeInfo;
void rt_exe_load_mem(Cpu *c, const uint8_t *img, size_t n, const char *what, RtExeInfo *out);   /* same, from a PS-X EXE already in memory (header + text) */
void rt_exe_load(Cpu *c, const char *path, RtExeInfo *out);
uint8_t *rt_hle_disc_file(const char *iso_path, size_t *n);   /* malloc'd contents of a file of the disc image (e.g. "\\SLUS_002.36;1"), NULL when absent */   /* copies the text into guest RAM; does NOT touch registers or the stack */

/* ---- trace (differential testing against tools/recomp/oracle.py; textual, one event per line) ---- */
void rt_trace_open(const char *path);                       /* never called = tracing off */
void rt_trace_close(void);
void rt_trace_store(const Cpu *c, uint32_t addr, unsigned size, uint32_t value);
void rt_trace_hle(const Cpu *c, const char *name);

/* ---- HLE of the launcher's library calls (runtime/hle.c; contracts in tools/recomp/hooks.py) ---- */
void rt_io_set_disc(const char *raw_bin_path);               /* disc image for the CD-ROM controller model (used by rt_io_attach_hw) */
int rt_hle_init(const char *disc_bin);                       /* opens the disc image; 0 on failure */
void rt_hle_bind(uint32_t addr, const char *name, uint32_t param, uint32_t param2);
typedef void (*RtBiosFn)(Cpu *c, char table, unsigned number);
void rt_set_bios_handler(RtBiosFn fn);                      /* jumps/calls to the BIOS entry points 0xA0/0xB0/0xC0 (t1 = number) are routed here by rt_call */   /* registers one hook (called by the generated gen_hooks_<exe>()) */
void rt_hle_add_module(const char *file_name, void (*code)(void), void (*hooks)(void), void (*reg)(void));   /* a recompiled executable that DoExecute may start */
void rt_hle_set_chain(int on);                              /* 1: DoExecute runs the next executable and returns 1 when it returns */
int rt_run_entry(Cpu *c, uint32_t entry);                   /* runs a recompiled entry; 1 = stopped by DoExecute, 0 = the program returned */
void rt_pad_replay_load(const char *path);                       /* recorded input: lines "tick hexword" (see frontend --record) */
void rt_pad_fuzz(unsigned seed, unsigned first_frame);          /* random pad input from first_frame on, when no scripted entry applies (soak tests) */
void rt_pad_script_add(const char *spec);                     /* "cross+start@first-last": scripted input by virtual frame (tests/demos) */
void rt_pad_set(uint32_t word);                               /* input provider: pad word as B(16h) returns it (pad 1 in the low halfword, active low); default 0xFFFFFFFF = no buttons */
void rt_hle_set_card(const char *path);                      /* virtual memory card backing file (NULL = no card inserted) */
int rt_hle_card_save(void);
const uint8_t *rt_hle_card_image(void);
uint8_t *rt_ram_ptr(Cpu *c);                                  /* host pointer to the 2 MiB guest RAM (HLE modules that DMA into guest memory) */
const char *rt_hle_stdout(size_t *n);
void rt_hle_last_exec(uint32_t out[3]);                     /* DoExecute arguments (headerbuf, p1, p2) */
#endif
