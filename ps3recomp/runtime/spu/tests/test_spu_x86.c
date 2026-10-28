/* Resistance port: checks the x86 (AVX2) helper forms in spu_helpers.h against
 * their scalar spu_X_ref definitions, bit for bit, on random and edge inputs;
 * and the fast single-precision ops against the exact ones (ulp distance, no
 * Inf/NaN ever produced).
 *   clang -O2 -mavx2 -mfma -I runtime/spu runtime/spu/tests/test_spu_x86.c -lm */
#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* stubs for the debug probes the inline LS accessors reference */
int g_spu_ls_watch_n = 0;
void spu_ls_watch_slow(uint32_t a, int w, const uint8_t* p, uint32_t pc, uint32_t lr) { (void)a; (void)w; (void)p; (void)pc; (void)lr; }
int g_wws_read_probe, g_wws_code_probe;

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)rs; }
static uint32_t rword(void) {
    switch (rnd() % 12) {
    case 0: return 0; case 1: return 0xFFFFFFFFu; case 2: return 0x80000000u; case 3: return 0x7FFFFFFFu;
    case 4: return rnd() & 0x1F; case 5: return (uint32_t)-(int32_t)(rnd() & 0x3F);
    case 6: return rnd() & 0xFFFF; case 7: return (rnd() & 0xFF) * 0x01010101u;
    case 8: return 0x7F800000u | (rnd() & 0x807FFFFFu);   /* exponent 255 */
    case 9: return rnd() & 0x807FFFFFu;                    /* exponent 0 */
    default: return rnd();
    }
}
static u128 rq(void) { u128 r; for (int i = 0; i < 4; i++) r._u32[i] = rword(); return r; }
static int same(u128 a, u128 b) { return !memcmp(&a, &b, 16); }
static int fails = 0;
static void report(const char* nm, u128 a, u128 b, u128 c, u128 got, u128 want, int imm) {
    if (fails++ > 40) return;
    printf("FAIL %s imm=%d\n  a=%08X %08X %08X %08X\n  b=%08X %08X %08X %08X\n  c=%08X %08X %08X %08X\n got=%08X %08X %08X %08X\nwant=%08X %08X %08X %08X\n", nm, imm,
        a._u32[0],a._u32[1],a._u32[2],a._u32[3], b._u32[0],b._u32[1],b._u32[2],b._u32[3], c._u32[0],c._u32[1],c._u32[2],c._u32[3],
        got._u32[0],got._u32[1],got._u32[2],got._u32[3], want._u32[0],want._u32[1],want._u32[2],want._u32[3]);
}
#define T2(nm) { u128 g = spu_##nm(a, b), w = spu_##nm##_ref(a, b); if (!same(g, w)) report(#nm, a, b, c, g, w, 0); }
#define T3(nm) { u128 g = spu_##nm(a, b, c), w = spu_##nm##_ref(a, b, c); if (!same(g, w)) report(#nm, a, b, c, g, w, 0); }
#define TI(nm) { u128 g = spu_##nm(a, imm), w = spu_##nm##_ref(a, imm); if (!same(g, w)) report(#nm, a, b, c, g, w, imm); }
#define T1(nm) { u128 g = spu_##nm(a), w = spu_##nm##_ref(a); if (!same(g, w)) report(#nm, a, b, c, g, w, 0); }

static u128 shli_ref(u128 a, int sh) { u128 r; sh&=0x3F; for(int i=0;i<4;i++) r._u32[i]=(sh>31)?0:(a._u32[i]<<sh); return r; }
static u128 rotmi_ref(u128 a, int i7) { u128 r; int sh=(0-i7)&0x3F; for(int i=0;i<4;i++) r._u32[i]=(sh>31)?0:(a._u32[i]>>sh); return r; }

static double ulps(uint32_t x, uint32_t y) {
    float fx, fy; memcpy(&fx, &x, 4); memcpy(&fy, &y, 4);
    if (fx == fy) return 0;
    double d = fabs((double)fx - (double)fy);
    double m = fmax(fabs(fx), fabs(fy));
    if (m < 1.2e-38) return d < 1e-37 ? 0 : 1e9;
    return d / (m * 1.1920929e-7);
}
static int is_plain(uint32_t x) { uint32_t e = (x >> 23) & 0xFF; return e > 0 && e < 255; }

int main(int argc, char** argv) {
    long N = argc > 1 ? atol(argv[1]) : 2000000;
    for (long it = 0; it < N; it++) {
        u128 a = rq(), b = rq(), c = rq();
        int imm = (int)(rnd() % 1024) - 512;
        if (rnd() & 1) { for (int i = 0; i < 16; i++) c._u8[i] = (uint8_t)(rnd() & ((rnd() & 3) ? 0x1F : 0xFF)); }  /* shufb controls */
        T2(a) T2(sf) T2(and) T2(or) T2(xor) T2(ceq) T2(cgt) T2(clgt) T3(selb)
        TI(ai) TI(sfi) TI(andi) TI(ori) TI(xori) TI(ceqi) TI(cgti) TI(clgti)
        TI(shlqbyi) TI(rotqbyi) TI(rotqmbyi) TI(cwd)
        T2(shlqby) T2(rotqby) T2(shlqbybi) T2(rotqbybi) T2(rotqmby) T2(rotqmbybi)
        { u128 g = spu_shufb(a, b, c), w = spu_shufb_ref(a, b, c); if (!same(g, w)) report("shufb", a, b, c, g, w, 0); }
        { u128 g = spu_cbd(a, imm), w = spu_cbd_pos((int)a._u32[0] + imm); if (!same(g, w)) report("cbd", a, b, c, g, w, imm); }
        { u128 g = spu_chd(a, imm), w = spu_chd_pos((int)a._u32[0] + imm); if (!same(g, w)) report("chd", a, b, c, g, w, imm); }
        { u128 g = spu_cdd(a, imm), w = spu_cdd_pos((int)a._u32[0] + imm); if (!same(g, w)) report("cdd", a, b, c, g, w, imm); }
        { u128 g = spu_cbx(a, b), w = spu_cbd_pos((int)(a._u32[0] + b._u32[0])); if (!same(g, w)) report("cbx", a, b, c, g, w, 0); }
        { u128 g = spu_chx(a, b), w = spu_chd_pos((int)(a._u32[0] + b._u32[0])); if (!same(g, w)) report("chx", a, b, c, g, w, 0); }
        { u128 g = spu_cwx(a, b), w = spu_cwd_pos((int)(a._u32[0] + b._u32[0])); if (!same(g, w)) report("cwx", a, b, c, g, w, 0); }
        { u128 g = spu_cdx(a, b), w = spu_cdd_pos((int)(a._u32[0] + b._u32[0])); if (!same(g, w)) report("cdx", a, b, c, g, w, 0); }
        { u128 g = spu_shli(a, imm), w = shli_ref(a, imm); if (!same(g, w)) report("shli", a, b, c, g, w, imm); }
        { u128 g = spu_rotmi(a, imm), w = rotmi_ref(a, imm); if (!same(g, w)) report("rotmi", a, b, c, g, w, imm); }
        T2(ceqb) T2(cgtb) T2(clgtb) T2(ceqh) T2(cgth) T2(clgth) T2(ah) T2(sfh)
        T2(shl) T2(rot) T2(rotm) T2(rotma) T2(cg) T2(bg)
        T2(mpy) T2(mpyu) T2(mpyh) T2(mpyhh) T2(mpyhhu) T2(mpys) T3(mpya)
        TI(ceqbi) TI(cgtbi) TI(clgtbi) TI(ceqhi) TI(cgthi) TI(clgthi) TI(ahi) TI(sfhi) TI(mpyi) TI(mpyui)
        TI(roti) TI(rotmai) TI(rotmahi)
        T1(xsbh) T1(xshw)
        { int i8 = rnd() & 0xFF;
          u128 g = spu_cflts(a, i8), w = spu_cflts_ref(a, i8); if (!same(g, w)) report("cflts", a, b, c, g, w, i8);
          g = spu_cfltu(a, i8); w = spu_cfltu_ref(a, i8); if (!same(g, w)) report("cfltu", a, b, c, g, w, i8);
          g = spu_csflt(a, i8); w = spu_csflt_ref(a, i8); if (!same(g, w)) report("csflt", a, b, c, g, w, i8);
          g = spu_cuflt(a, i8); w = spu_cuflt_ref(a, i8); if (!same(g, w)) report("cuflt", a, b, c, g, w, i8); }
    }
    printf("integer/permute/convert: %d failures over %ld iterations\n", fails, N);

    /* fast float vs exact */
    double worst[6] = {0}; long nonfinite = 0, big[6] = {0}, exact_miss[6] = {0}, zero_miss = 0;
    const char* fn[6] = {"fa", "fs", "fm", "fma", "fms", "fnms"};
    for (long it = 0; it < N; it++) {
        u128 a = rq(), b = rq(), c = rq();
        const unsigned csr0 = _mm_getcsr();
        _mm_setcsr((csr0 & ~0x6000u) | 0x8040u | 0x6000u);   /* the SPU threads' mode: FTZ|DAZ|RZ */
        u128 g[6] = { spu_fa(a, b), spu_fs(a, b), spu_fm(a, b), spu_fma(a, b, c), spu_fms(a, b, c), spu_fnms(a, b, c) };
        _mm_setcsr(csr0);
        u128 w[6] = { spu_fa_ref(a, b), spu_fs_ref(a, b), spu_fm_ref(a, b), spu_fma_ref(a, b, c), spu_fms_ref(a, b, c), spu_fnms_ref(a, b, c) };
        for (int k = 0; k < 6; k++) for (int l = 0; l < 4; l++) {
            uint32_t x = g[k]._u32[l];
            if (((x >> 23) & 0xFF) == 0xFF) nonfinite++;
            /* compare only where every operand and the exact result are ordinary */
            if (!is_plain(a._u32[l]) || !is_plain(b._u32[l]) || (k >= 3 && !is_plain(c._u32[l]))) continue;
            if (w[k]._u32[l] == 0) { if (x != 0) { if (zero_miss++ < 3) printf("  %s zero mismatch got=%08X\n", fn[k], x); } continue; }
            if (!is_plain(w[k]._u32[l])) continue;
            double u = ulps(x, w[k]._u32[l]);
            if (x != w[k]._u32[l]) { exact_miss[k]++; if (exact_miss[k] <= 3) printf("  %s lane mismatch a=%08X b=%08X c=%08X got=%08X want=%08X\n", fn[k], a._u32[l], b._u32[l], c._u32[l], x, w[k]._u32[l]); }
            if (u > worst[k]) worst[k] = u;
            if (u > 2.5) big[k]++;
        }
    }
    for (int k = 0; k < 6; k++) printf("%-5s worst %.2f ulp, >2.5ulp: %ld, bit mismatches: %ld\n", fn[k], worst[k], big[k], exact_miss[k]);
    printf("zero-result mismatches: %ld\n", zero_miss);
    printf("fast float outputs with exponent 255 (Inf/NaN on x86): %ld\n", nonfinite);
    return fails || nonfinite;
}
