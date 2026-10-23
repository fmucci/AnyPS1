/* Tests of the runtime skeleton (queue P2-8): memory map, fail-loud policy, I/O registration, dispatch, EXE loader.
 * Every "must abort" case installs a fatal handler that longjmps and checks the message. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "tmp_path.h"

/* the interpreter tier (runtime/interp.c) counts virtual time through rt_yield, which lives in hle.c; these unit tests do not link it */
void rt_yield(Cpu *c) { c->yield = RT_YIELD_PERIOD; }
void rt_dma_kick(Cpu *c) { (void)c; }

static jmp_buf jb;
static char last[700];
static void on_fatal(const char *m) { snprintf(last, sizeof last, "%s", m); longjmp(jb, 1); }
static int fails, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)
#define EXPECT_FATAL(stmt, sub) do { last[0] = 0; if (setjmp(jb) == 0) { stmt; checks++; fails++; printf("FAIL line %d: no abort: %s\n", __LINE__, #stmt); } \
    else { checks++; if (!strstr(last, sub)) { fails++; printf("FAIL line %d: message lacks '%s': %s\n", __LINE__, sub, last); } } } while (0)

static uint32_t io_reg[4];
static unsigned io_last_size;
static uint32_t dev_rd(void *ctx, Cpu *c, uint32_t p, unsigned sz) { (void)c; io_last_size = sz; return ((uint32_t *)ctx)[(p - 0x1F801100) / 4] & (sz == 4 ? 0xFFFFFFFFu : sz == 2 ? 0xFFFFu : 0xFFu); }
static void dev_wr(void *ctx, Cpu *c, uint32_t p, unsigned sz, uint32_t v) { (void)c; io_last_size = sz; ((uint32_t *)ctx)[(p - 0x1F801100) / 4] = v; }
static void fn_a(Cpu *c) { c->r[2] = 0xA; }
static void fn_b(Cpu *c) { c->r[2] = 0xB; }

int main(void) {
    setenv("PS1_NO_INTERP", "1", 1);                         /* these checks are about the fatal stop for an unregistered target; the interpreter tier has its own test (test_interp.c) */
    rt_set_fatal_handler(on_fatal);
    Cpu cpu, *c = &cpu;
    rt_init(c);

    /* RAM: little-endian layout, extension, mirrors across segments */
    rt_sw(c, 0x80010000, 0x11223344u);
    CHECK(rt_lbu(c, 0x80010000) == 0x44 && rt_lbu(c, 0x80010003) == 0x11);
    CHECK(rt_lhu(c, 0x80010002) == 0x1122 && rt_lw(c, 0x00010000) == 0x11223344u && rt_lw(c, 0xA0010000) == 0x11223344u);
    rt_sb(c, 0x80010010, 0x80); rt_sh(c, 0x80010012, 0x8001);
    CHECK(rt_lb(c, 0x80010010) == 0xFFFFFF80u && rt_lbu(c, 0x80010010) == 0x80);
    CHECK(rt_lh(c, 0x80010012) == 0xFFFF8001u && rt_lhu(c, 0x80010012) == 0x8001);
    rt_sw(c, 0x801FFFFC, 0xCAFEBABEu); CHECK(rt_lw(c, 0xA01FFFFC) == 0xCAFEBABEu);   /* last word of RAM */
    rt_sh(c, 0x80010020, 0x1234); rt_sb(c, 0x80010020, 0xFF); CHECK(rt_lhu(c, 0x80010020) == 0x12FF);   /* sub-word stores don't clobber neighbours */

    /* fail-loud: alignment, BIOS-reserved, mirrors, segments, unmapped */
    EXPECT_FATAL(rt_lw(c, 0x80010002), "misaligned");
    EXPECT_FATAL(rt_lh(c, 0x80010001), "misaligned");
    EXPECT_FATAL(rt_sw(c, 0x80010001, 0), "misaligned");
    EXPECT_FATAL(rt_sh(c, 0x80010003, 0), "misaligned");
    EXPECT_FATAL(rt_lw(c, 0x00000000), "BIOS");
    EXPECT_FATAL(rt_lbu(c, 0x8000FFFF), "BIOS");
    rt_allow_low_ram(c, 0x9000, 0xA000, "test");
    rt_sw(c, 0x80009FF8, 0x5A5A5A5Au); CHECK(rt_lw(c, 0x00009FF8) == 0x5A5A5A5Au);
    EXPECT_FATAL(rt_lw(c, 0x80008FFC), "BIOS");
    EXPECT_FATAL(rt_lw(c, 0x8000A000), "BIOS");
    EXPECT_FATAL(rt_lw(c, 0x80009FFE), "misaligned");
    EXPECT_FATAL(rt_allow_low_ram(c, 0xF000, 0x11000, "bad"), "bad range");
    EXPECT_FATAL(rt_lw(c, 0x80200000), "mirror");
    EXPECT_FATAL(rt_lw(c, 0x40000000), "above 512 MiB");
    EXPECT_FATAL(rt_lw(c, 0xFFFE0130), "KSEG2");
    EXPECT_FATAL(rt_lw(c, 0xC0000000), "KSEG2");
    EXPECT_FATAL(rt_lw(c, 0x9F000000), "unmapped");   /* expansion 1 */
    EXPECT_FATAL(rt_lw(c, 0xBFC00000), "unmapped");   /* BIOS ROM */
    EXPECT_FATAL(rt_lw(c, 0x1F800400), "unmapped");   /* just past the scratchpad */
    rt_set_ram_mirrors(c, 1);
    rt_sw(c, 0x80200000, 0xAABBCCDDu); CHECK(rt_lw(c, 0x80000000 + 0x00200000) == 0xAABBCCDDu);   /* mirror aliases 0x0 when allowed ... */
    EXPECT_FATAL(rt_lw(c, 0x80000000), "BIOS");        /* ... but the BIOS-reserved check still applies to the base alias */
    CHECK(rt_lw(c, 0x80010000) == 0x11223344u);
    rt_set_ram_mirrors(c, 0);

    /* scratchpad: KUSEG and KSEG0 only */
    rt_sw(c, 0x1F800000, 0x01020304u); rt_sw(c, 0x1F8003FC, 0x0A0B0C0Du);
    CHECK(rt_lw(c, 0x9F800000) == 0x01020304u && rt_lw(c, 0x9F8003FC) == 0x0A0B0C0Du && rt_lbu(c, 0x1F800001) == 0x03);
    EXPECT_FATAL(rt_lw(c, 0xBF800000), "KSEG1");
    EXPECT_FATAL(rt_sw(c, 0xBF800000, 1), "KSEG1");

    /* I/O: nothing proven until a device is registered */
    EXPECT_FATAL(rt_lw(c, 0x1F801070), "unproven I/O");
    EXPECT_FATAL(rt_sw(c, 0xBF801810, 1), "unproven I/O");
    EXPECT_FATAL(rt_sb(c, 0x1F802041, 1), "unproven I/O");   /* expansion 2 */
    RtIoDev d = {"testdev", 0x1F801100, 0x1F801110, dev_rd, dev_wr, io_reg};
    rt_io_register(&d);
    rt_sw(c, 0xBF801104, 0x80808080u); CHECK(io_reg[1] == 0x80808080u && io_last_size == 4);
    CHECK(rt_lw(c, 0x1F801104) == 0x80808080u && io_last_size == 4);
    CHECK(rt_lh(c, 0x1F801104) == 0xFFFF8080u && io_last_size == 2);
    CHECK(rt_lb(c, 0x1F801104) == 0xFFFFFF80u && rt_lbu(c, 0x1F801104) == 0x80 && io_last_size == 1);
    rt_sh(c, 0x1F801108, 0x12345678u); CHECK(io_reg[2] == 0x5678 && io_last_size == 2);   /* value masked to the access size */
    EXPECT_FATAL(rt_lw(c, 0x1F801110), "unproven I/O");   /* one past the device */
    EXPECT_FATAL(rt_lw(c, 0x1F8010FC), "unproven I/O");
    RtIoDev ov = {"overlap", 0x1F80110C, 0x1F801120, dev_rd, dev_wr, io_reg};
    EXPECT_FATAL(rt_io_register(&ov), "overlaps");
    RtIoDev bad = {"badrange", 0x1F800000, 0x1F800010, dev_rd, dev_wr, io_reg};
    EXPECT_FATAL(rt_io_register(&bad), "bad device");
    uint8_t blk[4] = {1, 2, 3, 4};
    EXPECT_FATAL(rt_mem_write(c, 0x1F801100, blk, 4), "I/O space");
    rt_io_reset();
    EXPECT_FATAL(rt_lw(c, 0x1F801104), "unproven I/O");

    /* host block access */
    rt_mem_write(c, 0x80020000, blk, 4); CHECK(rt_lw(c, 0x80020000) == 0x04030201u);
    uint8_t back[4]; rt_mem_read(c, 0x80020000, back, 4); CHECK(memcmp(back, blk, 4) == 0);
    EXPECT_FATAL(rt_mem_write(c, 0x801FFFFE, blk, 4), "mirror");   /* runs past the end of RAM */

    /* function table and rt_call */
    rt_fn_reset();
    rt_fn_register(0x80030000, fn_a, "fn_a", RT_FN_GENERATED);
    rt_fn_register(0x80010800, fn_b, "fn_b", RT_FN_HLE);
    CHECK(rt_fn_count() == 2 && strcmp(rt_fn_name(0x00030000), "fn_a") == 0 && strcmp(rt_fn_name(0xA0010800), "fn_b") == 0);
    CHECK(rt_fn_name(0x80030004) == NULL);
    rt_call(c, 0x80030000); CHECK(c->r[2] == 0xA);
    rt_call(c, 0xA0010800); CHECK(c->r[2] == 0xB);       /* other segment of the same physical address */
    EXPECT_FATAL(rt_call(c, 0x80030004), "no generated function");
    EXPECT_FATAL(rt_call(c, 0x80030002), "unaligned");
    EXPECT_FATAL(rt_call(c, 0), "no generated function");
    EXPECT_FATAL(rt_fn_register(0xA0030000, fn_b, "dup", RT_FN_HLE), "already registered");
    EXPECT_FATAL(rt_fn_register(0x80030002, fn_b, "odd", RT_FN_HLE), "bad entry");
    for (uint32_t i = 0; i < 1000; i++) rt_fn_register(0x80100000 + 4 * i, (i & 1) ? fn_a : fn_b, "bulk", RT_FN_GENERATED);   /* growth + sorted insertion */
    CHECK(rt_fn_count() == 1002);
    for (uint32_t i = 0; i < 1000; i++) { rt_call(c, 0x80100000 + 4 * (999 - i)); CHECK(c->r[2] == (((999 - i) & 1) ? 0xA : 0xB)); }

    /* COP0 / exceptions / GTE */
    cop0_mtc0(c, 12, 0x0000003Cu); CHECK(cop0_mfc0(c, 12) == 0x3C);
    cop0_rfe(c); CHECK(cop0_mfc0(c, 12) == 0x3F);   /* low 6 bits: mode stack shifts right by 2, top pair kept */
    EXPECT_FATAL(cop0_mfc0(c, 5), "unproven COP0");
    EXPECT_FATAL(cop0_mtc0(c, 13, 1), "unproven COP0");
    EXPECT_FATAL(rt_exception(c, 0x80010000, 8, 0), "syscall");
    CHECK(c->exc == 8 && c->exc_pc == 0x80010000);
    EXPECT_FATAL(rt_exception(c, 0x80010004, 9, 0x40), "break");
    gte_cmd(c, 0x4A180001u);   /* RTPS runs now (finding 0024); unknown commands still abort */
    EXPECT_FATAL(gte_cmd(c, 0x0000003Bu), "not implemented");
    gte_mtc2(c, 0, 0x00020001u); CHECK(gte_mfc2(c, 0) == 0x00020001u);      /* register file works */
    gte_mtc2(c, 1, 0x8001u); CHECK(gte_mfc2(c, 1) == 0xFFFF8001u);          /* VZ0 reads sign-extended */
    gte_mtc2(c, 15, 0x11u); gte_mtc2(c, 15, 0x22u); CHECK(gte_mfc2(c, 14) == 0x22u && gte_mfc2(c, 13) == 0x11u);   /* SXYP pushes the FIFO */
    gte_mtc2(c, 30, 0xFFFFFFF0u); CHECK(gte_mfc2(c, 31) == 28u);              /* LZCR counts the leading sign bits */
    gte_ctc2(c, 4, 0x8000u); CHECK(gte_cfc2(c, 4) == 0xFFFF8000u);
    c->pc = 0x80012344; EXPECT_FATAL(rt_lw(c, 0x80010001), "last recorded pc=80012344");

    /* EXE loader on a synthetic file (no game data) */
    char path[512]; tmp_path(path, sizeof path, "rt_exe");
    FILE *f = fopen(path, "wb");
    uint8_t hdr[0x800] = "PS-X EXE";
    uint32_t w[4] = {0x80010800, 0x80050000, 0x80010800, 8};
    memcpy(hdr + 0x10, w, 16);
    fwrite(hdr, 1, sizeof hdr, f); fwrite("\x01\x02\x03\x04\x05\x06\x07\x08", 1, 8, f); fclose(f);
    RtExeInfo ei;
    rt_exe_load(c, path, &ei);
    CHECK(ei.pc == 0x80010800 && ei.gp == 0x80050000 && ei.addr == 0x80010800 && ei.size == 8 && ei.sp_base == 0 && ei.sp_off == 0);
    CHECK(rt_lw(c, 0x80010800) == 0x04030201u && rt_lw(c, 0x80010804) == 0x08070605u);
    w[3] = 16; { FILE *g = fopen(path, "r+b"); fseek(g, 0x10, SEEK_SET); fwrite(w, 1, 16, g); fclose(g); }
    EXPECT_FATAL(rt_exe_load(c, path, &ei), "shorter");
    w[3] = 6; { FILE *g = fopen(path, "r+b"); fseek(g, 0x10, SEEK_SET); fwrite(w, 1, 16, g); fclose(g); }
    EXPECT_FATAL(rt_exe_load(c, path, &ei), "bad load address");
    w[2] = 0x80000000; w[3] = 8; { FILE *g = fopen(path, "r+b"); fseek(g, 0x10, SEEK_SET); fwrite(w, 1, 16, g); fclose(g); }
    EXPECT_FATAL(rt_exe_load(c, path, &ei), "BIOS");
    { FILE *g = fopen(path, "r+b"); fwrite("NOT-AN-EXE", 1, 8, g); fclose(g); }
    EXPECT_FATAL(rt_exe_load(c, path, &ei), "not a PS-X EXE");
    EXPECT_FATAL(rt_exe_load(c, "/nonexistent/x.exe", &ei), "cannot open");
    remove(path);
    { char p2[512]; tmp_path(p2, sizeof p2, "rt_raw"); FILE *g = fopen(p2, "wb"); fwrite("\xAA\xBB\xCC\xDD", 1, 4, g); fclose(g);
      rt_load_raw(c, p2, 0x80100000); CHECK(rt_lw(c, 0x80100000) == 0xDDCCBBAAu); EXPECT_FATAL(rt_load_raw(c, p2, 0x801FFFFE), "mirror"); remove(p2); }

    rt_shutdown(c);
    printf("runtime tests: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
