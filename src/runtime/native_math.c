/*
 * native_math.c - native versions of small, hot lifted math helpers (R36S).
 *
 * The lifted code does every x87 operation in double precision on the emulated stack window; these ports do
 * the same operations, in the same order, in double precision on registers, so the results are identical to
 * the lifted functions (which are what the port runs today). The lifted definitions are renamed *_orig
 * (build_guest_vm.sh wrap list); every caller, direct or through the dispatch table, reaches these.
 *   NFSU2_NATIVE_MATH=0  lifted functions
 *                     1  native (default)
 *                     2  verify: run both, compare, report mismatches ([NFSU2 MATH])
 *
 *   sub_0075BC00  MSVC _ftol2: ST(0) truncated toward zero to edx:eax, popped.
 *   sub_00440FF0  (and its jump stub sub_0043CA20) out = p * M (row vector, 4x4 float matrix, translation row 3).
 *   sub_0043D250  out = cross(b, a)  (cdecl: out, a, b).
 *   sub_00401310  4x4 matrix copy; sub_00401430 4x4 multiply (x87); sub_00689A17 D3DXMatrixMultiply (SSE);
 *   sub_0043CA30  transpose.
 *   sub_0043C310 / sub_0043C3B0  sine polynomial of a 16-bit / float angle (result pushed on ST(0)).
 *   sub_00576170  table lookup with linear interpolation (thiscall, result pushed on ST(0)).
 *   sub_005D3080  point-against-quad edge test (thiscall, result pushed on ST(0)).
 *   sub_005BBF20 / sub_005BC010  AABB transform and AABB-vs-frustum test (render culling).
 *   sub_0043BE50  the game's timer: QueryPerformanceCounter shifted, without the import dispatch (that went
 *                 through the guest scheduler's checkpoint on every physics substep).
 */
#include "recomp_types.h"
#include "nfs_exe_addr.h"   /* v10 addresses -> the lifted exe's (config/exe_functions.txt) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
extern void sub_0075BC00_orig(void);
extern void sub_00440FF0_orig(void);
extern void sub_0043CA20_orig(void);
extern void sub_0043D250_orig(void);
extern uint32_t getD3D9PresentCount(void);
#define M32(a) (*(uint32_t*)ADDR(a))
#define MF(a) (*(float*)ADDR(a))

static int g_mode = -1;
static inline int mode(void) {
    if (g_mode < 0) { const char* e = getenv("NFSU2_NATIVE_MATH"); g_mode = e ? atoi(e) : 1; }
    return g_mode;
}
static uint32_t vCalls[4], vBad[4], vFrame;
static void verifyReport(void) {
    const uint32_t now = getD3D9PresentCount();
    if (now == vFrame) return;
    vFrame = now;
    if (now % 600 == 0)
        printf("[NFSU2 MATH] verify: ftol %u calls %u bad, transform %u/%u, cross %u/%u\n", vCalls[0], vBad[0], vCalls[1], vBad[1], vCalls[2], vBad[2]);
}

/* ---- _ftol2 ---- */
static inline int64_t ftolValue(double v, int64_t shadow) {
    if (shadow && (double)shadow == v) return shadow;          /* exact value of a fild qword */
    /* as the lifted fistp (a saturating conversion) followed by _ftol2's correction: NaN 0, overflow saturates */
    if (!(v == v)) return 0;
    if (v >= 9223372036854775808.0) return INT64_MAX;
    if (v < -9223372036854775808.0) return INT64_MIN;
    return (int64_t)v;                                         /* C conversion truncates toward zero */
}
void sub_0075BC00(void) {
    const int m = mode();
    if (m == 0) { sub_0075BC00_orig(); return; }
    const int sp = g_cpu.fp_sp;
    const int64_t r = ftolValue(g_cpu.st_win[sp], g_cpu.st_i64_win[sp]);
    if (m == 2) {
        sub_0075BC00_orig();
        ++vCalls[0];
        if (g_eax != (uint32_t)r || g_edx != (uint32_t)((uint64_t)r >> 32)) {
            if (vBad[0]++ < 10) printf("[NFSU2 MATH] ftol mismatch: native %lld, lifted %08x%08x\n", (long long)r, g_edx, g_eax);
        }
        verifyReport();
        return;
    }
    /* fistp + fild + compare/correct in the original; net effect: pop ST(0), edx:eax = trunc */
    g_cpu.fp_sp = sp + 1;
    if (g_cpu.fp_sp > RECOMP_X87_SLOTS - 9) fp_window_recentre();
    g_cpu.fp_top -= 1;
    g_eax = (uint32_t)r; g_edx = (uint32_t)((uint64_t)r >> 32);
    g_esp += 4;  /* ret */
}

/* ---- point * matrix ---- */
static inline void transform(uint32_t out, uint32_t mat, uint32_t p, float* res) {
    const double p0 = MF(p), p1 = MF(p + 4), p2 = MF(p + 8);
    double x = (double)MF(mat + 0x20) * p2;
    x = x + (double)MF(mat + 0x10) * p1;
    x = x + (double)MF(mat) * p0;
    x = x + (double)MF(mat + 0x30);
    double y = (double)MF(mat + 0x24) * p2;
    y = y + (double)MF(mat + 4) * p0;
    y = y + (double)MF(mat + 0x14) * p1;
    y = y + (double)MF(mat + 0x34);
    double z = (double)MF(mat + 0x28) * p2;
    z = z + (double)MF(mat + 8) * p0;
    z = z + (double)MF(mat + 0x18) * p1;
    z = z + (double)MF(mat + 0x38);
    res[0] = (float)x; res[1] = (float)y; res[2] = (float)z;
    (void)out;
}
static void transformEntry(void (*orig)(void)) {
    const int m = mode();
    if (m == 0) { orig(); return; }
    const uint32_t out = M32(g_esp + 4), mat = M32(g_esp + 8), p = M32(g_esp + 12);
    float res[3];
    transform(out, mat, p, res);
    if (m == 2) {
        orig();
        ++vCalls[1];
        if (memcmp(res, (const void*)ADDR(out), 12)) {
            if (vBad[1]++ < 10) printf("[NFSU2 MATH] transform mismatch: native %.9g %.9g %.9g, lifted %.9g %.9g %.9g\n", res[0], res[1], res[2], MF(out), MF(out + 4), MF(out + 8));
        }
        verifyReport();
        return;
    }
    memcpy((void*)ADDR(out), res, 12);
    g_eax = out; g_ecx = p;
    g_esp += 4;  /* ret (cdecl) */
}
void sub_00440FF0(void) { transformEntry(sub_00440FF0_orig); }
void sub_0043CA20(void) { transformEntry(sub_0043CA20_orig); }

/* ---- cross product: out = (b1*a2 - b2*a1, b2*a0 - a2... ) in the original's operand order ---- */
void sub_0043D250(void) {
    const int m = mode();
    if (m == 0) { sub_0043D250_orig(); return; }
    const uint32_t out = M32(g_esp + 4), a = M32(g_esp + 8), b = M32(g_esp + 12);
    /* ecx = b, eax = a */
    const double t0 = (double)MF(b) * MF(a + 8) - (double)MF(b + 8) * MF(a);          /* st: b0*a2 - b2*a0  */
    const double t1 = (double)MF(b + 4) * MF(a) - (double)MF(a + 4) * MF(b);          /* b1*a0 - a1*b0      */
    const double t2 = (double)MF(b + 8) * MF(a + 4) - (double)MF(b + 4) * MF(a + 8);  /* b2*a1 - b1*a2      */
    float res[3] = {(float)t2, (float)t0, (float)t1};
    if (m == 2) {
        sub_0043D250_orig();
        ++vCalls[2];
        if (memcmp(res, (const void*)ADDR(out), 12)) {
            if (vBad[2]++ < 10) printf("[NFSU2 MATH] cross mismatch: native %.9g %.9g %.9g, lifted %.9g %.9g %.9g\n", res[0], res[1], res[2], MF(out), MF(out + 4), MF(out + 8));
        }
        verifyReport();
        return;
    }
    memcpy((void*)ADDR(out), res, 12);
    g_eax = out; g_ecx = b;
    g_esp += 4;
}

/* ---- matrices ---- */
extern void sub_00401310_orig(void);
extern void sub_00401430_orig(void);
extern void sub_00689A17_orig(void);
static uint32_t mCalls[3], mBad[3];
static void matReport(void) {
    static uint32_t f;
    const uint32_t now = getD3D9PresentCount();
    if (now == f) return;
    f = now;
    if (now % 600 == 0) printf("[NFSU2 MATH] verify: copy %u/%u, mul x87 %u/%u, mul sse %u/%u\n", mCalls[0], mBad[0], mCalls[1], mBad[1], mCalls[2], mBad[2]);
}
static void matCheck(int k, const float* t, uint32_t out) {
    ++mCalls[k];
    if (memcmp(t, (const void*)ADDR(out), 64) && mBad[k]++ < 10) {
        const float* o = (const float*)ADDR(out);
        printf("[NFSU2 MATH] matrix %d mismatch: native %.9g %.9g %.9g %.9g, lifted %.9g %.9g %.9g %.9g\n", k, t[0], t[1], t[2], t[3], o[0], o[1], o[2], o[3]);
    }
    matReport();
}
/* sub_00401310(out, in): 4x4 copy (cdecl) */
void sub_00401310(void) {
    const int m = mode();
    if (m == 0) { sub_00401310_orig(); return; }
    const uint32_t out = M32(g_esp + 4), in = M32(g_esp + 8);
    float t[16]; memcpy(t, (const void*)ADDR(in), 64);
    if (m == 2) { sub_00401310_orig(); matCheck(0, t, out); return; }
    memcpy((void*)ADDR(out), t, 64);
    g_eax = out; g_ecx = M32(in + 0x30); g_edx = M32(in + 0x3c);
    M32(g_esp + 8) = g_edx;   /* the original uses its argument slot as scratch */
    g_esp += 4;
}
/* sub_00401430(out, A, B): out = A * B, x87 (cdecl); the original's term order per element */
void sub_00401430(void) {
    const int m = mode();
    if (m == 0) { sub_00401430_orig(); return; }
    const uint32_t out = M32(g_esp + 4);
    const float* A = (const float*)ADDR(M32(g_esp + 8));
    const float* B = (const float*)ADDR(M32(g_esp + 12));
    float t[16];
    t[0] = (float)(((((double)A[0] * B[0]) + (double)A[3] * B[12]) + (double)A[2] * B[8]) + (double)B[4] * A[1]);
    t[1] = (float)(((((double)B[13] * A[3]) + (double)A[1] * B[5]) + (double)B[9] * A[2]) + (double)B[1] * A[0]);
    t[2] = (float)(((((double)B[10] * A[2]) + (double)B[2] * A[0]) + (double)A[3] * B[14]) + (double)B[6] * A[1]);
    t[3] = (float)(((((double)B[15] * A[3]) + (double)A[2] * B[11]) + (double)A[0] * B[3]) + (double)B[7] * A[1]);
    t[4] = (float)(((((double)A[6] * B[8]) + (double)A[7] * B[12]) + (double)B[4] * A[5]) + (double)A[4] * B[0]);
    t[5] = (float)(((((double)A[6] * B[9]) + (double)A[4] * B[1]) + (double)A[5] * B[5]) + (double)A[7] * B[13]);
    t[6] = (float)(((((double)A[7] * B[14]) + (double)A[6] * B[10]) + (double)B[6] * A[5]) + (double)A[4] * B[2]);
    t[7] = (float)(((((double)B[15] * A[7]) + (double)A[6] * B[11]) + (double)A[4] * B[3]) + (double)B[7] * A[5]);
    t[8] = (float)(((((double)A[10] * B[8]) + (double)A[11] * B[12]) + (double)B[4] * A[9]) + (double)A[8] * B[0]);
    t[9] = (float)(((((double)A[10] * B[9]) + (double)A[8] * B[1]) + (double)A[9] * B[5]) + (double)A[11] * B[13]);
    t[10] = (float)(((((double)A[11] * B[14]) + (double)A[10] * B[10]) + (double)B[6] * A[9]) + (double)A[8] * B[2]);
    t[11] = (float)(((((double)B[15] * A[11]) + (double)A[10] * B[11]) + (double)A[8] * B[3]) + (double)B[7] * A[9]);
    t[12] = (float)(((((double)A[14] * B[8]) + (double)A[15] * B[12]) + (double)B[4] * A[13]) + (double)A[12] * B[0]);
    t[13] = (float)(((((double)A[14] * B[9]) + (double)A[12] * B[1]) + (double)A[13] * B[5]) + (double)A[15] * B[13]);
    t[14] = (float)(((((double)A[15] * B[14]) + (double)A[14] * B[10]) + (double)B[6] * A[13]) + (double)A[12] * B[2]);
    t[15] = (float)(((((double)B[15] * A[15]) + (double)A[14] * B[11]) + (double)A[12] * B[3]) + (double)B[7] * A[13]);
    if (m == 2) { sub_00401430_orig(); matCheck(1, t, out); return; }
    memcpy((void*)ADDR(out), t, 64);
    g_eax = out;
    g_esp += 4;
}
/* sub_00689A17(out, A, B): D3DXMatrixMultiply, SSE single precision (stdcall); the original's order */
void sub_00689A17(void) {
    const int m = mode();
    if (m == 0) { sub_00689A17_orig(); return; }
    const uint32_t out = M32(g_esp + 4);
    const float* a = (const float*)ADDR(M32(g_esp + 8));
    const float* b = (const float*)ADDR(M32(g_esp + 12));
    float t[16];
    for (int c = 0; c < 4; ++c) {
        const float b0 = b[c], b1 = b[4 + c], b2 = b[8 + c], b3 = b[12 + c];
        { const float x4 = a[0] * b0, x5 = a[1] * b1, x6 = a[2] * b2, x7 = a[3] * b3; t[c] = (x4 + x5) + (x6 + x7); }
        { float x5 = a[4] * b0; const float x6 = a[5] * b1, x7 = a[6] * b2; x5 = x5 + x6; x5 = x5 + x7; t[4 + c] = x5 + a[7] * b3; }
        { float x6 = a[8] * b0; x6 = x6 + a[9] * b1; x6 = x6 + a[10] * b2; t[8 + c] = x6 + a[11] * b3; }
        { const float x0 = b0 * a[12], x1 = b1 * a[13], x2 = b2 * a[14], x3 = b3 * a[15]; t[12 + c] = (x0 + x1) + (x2 + x3); }
    }
    if (m == 2) { sub_00689A17_orig(); matCheck(2, t, out); return; }
    memcpy((void*)ADDR(out), t, 64);
    g_eax = out;
    g_esp += 16;  /* ret 0xc */
}

/* ---- sin approximation: sub_0043C310(u16 angle) / sub_0043C3B0(float angle) push the result on ST(0) ---- */
extern void sub_0043C310_orig(void);
extern void sub_0043C3B0_orig(void);
static uint32_t sCalls, sBad;
static inline double sinPoly(uint32_t a16) {
    double x = (double)(int32_t)a16 * MF(0x784240);
    double s = MF(0x784250);
    if (!(x < (double)MF(0x786da0))) x = x - MF(0x786d9c);
    else if (!(x < (double)MF(0x784254))) { x = x - MF(0x786d98); s = MF(0x7842a0); }
    const double x2 = x * x, x3 = x2 * x, x5 = x3 * x2, x7 = x5 * x2;
    double y = x - x3 * MF(0x786d94);
    y = y + x5 * MF(0x786d90);
    y = y - x7 * MF(0x786d8c);
    const double x9 = x7 * x2;
    y = y + x9 * MF(0x786d88);
    return y * s;
}
static inline void pushSt(double v) {
    if (g_cpu.fp_sp <= 8) fp_window_recentre();
    g_cpu.fp_sp -= 1;
    g_cpu.st_win[g_cpu.fp_sp] = v; g_cpu.st_i64_win[g_cpu.fp_sp] = 0;
    g_cpu.fp_top += 1;
}
static void sinCheck(double v) {
    ++sCalls;
    const double l = g_cpu.st_win[g_cpu.fp_sp];
    if (memcmp(&l, &v, 8) && sBad++ < 10) printf("[NFSU2 MATH] sin mismatch: native %.17g lifted %.17g\n", v, l);
    static uint32_t f;
    const uint32_t now = getD3D9PresentCount();
    if (now != f) { f = now; if (now % 600 == 0) printf("[NFSU2 MATH] verify: sin %u/%u\n", sCalls, sBad); }
}
void sub_0043C310(void) {
    const int m = mode();
    if (m == 0) { sub_0043C310_orig(); return; }
    const uint32_t a = M32(g_esp + 4) & 0xFFFFu;
    const double v = sinPoly(a);
    if (m == 2) { sub_0043C310_orig(); sinCheck(v); return; }
    M32(g_esp + 4) = a;
    pushSt(v);
    g_eax = a;
    g_esp += 4;
}
void sub_0043C3B0(void) {
    const int m = mode();
    if (m == 0) { sub_0043C3B0_orig(); return; }
    /* fld [esp+4]; fmul [0x78423c]; call _ftol; the low word is the angle */
    const double f = (double)MF(g_esp + 4) * MF(0x78423c);
    const uint32_t a = (uint32_t)ftolValue(f, 0) & 0xFFFFu;
    const double v = sinPoly(a);
    if (m == 2) { sub_0043C3B0_orig(); sinCheck(v); return; }
    M32(g_esp + 4) = a;
    pushSt(v);
    g_eax = a; g_edx = (uint32_t)((uint64_t)ftolValue(f, 0) >> 32);
    g_esp += 4;
}

/* ---- sub_0043CA30(out, in): 4x4 transpose (cdecl) ---- */
extern void sub_0043CA30_orig(void);
void sub_0043CA30(void) {
    const int m = mode();
    if (m == 0) { sub_0043CA30_orig(); return; }
    const uint32_t out = M32(g_esp + 4), in = M32(g_esp + 8);
    uint32_t s[16], t[16];
    memcpy(s, (const void*)ADDR(in), 64);
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) t[j * 4 + i] = s[i * 4 + j];
    if (m == 2) { sub_0043CA30_orig(); matCheck(0, (const float*)t, out); return; }
    memcpy((void*)ADDR(out), t, 64);
    g_eax = out; g_ecx = s[15]; g_edx = s[14];
    M32(g_esp + 8) = s[3];
    g_esp += 4;
}

/* ---- sub_0043BE50: QueryPerformanceCounter >> [0x8284e4] (the game's timer), without the import call ---- */
extern void sub_0043BE50_orig(void);
extern uint64_t nfsQpcNow(void);
void sub_0043BE50(void) {
    const int m = mode();
    if (m == 0 || m == 2) { sub_0043BE50_orig(); return; }   /* time: not comparable */
    const int64_t now = (int64_t)nfsQpcNow();
    const uint32_t cl = M32(0x8284e4) & 0xFFu;
    int64_t r;
    if (cl >= 64) r = now < 0 ? -1 : 0;
    else r = now >> cl;                                      /* sar, as sub_0075C320 */
    g_eax = (uint32_t)r; g_edx = (uint32_t)((uint64_t)r >> 32);
    g_ecx = M32(0x8284e4);
    g_esp += 4;
}

/* ---- sub_00576170 (thiscall, ret 4): piecewise-linear table lookup, result pushed on ST(0) ----
 * this: [0] count, [4] min, [0xc] scale, [0x10] float table. t = (x - min) * scale; below 0 -> table[0],
 * from count-1 on -> table[count-1], else lerp(table[i], table[i+1], t - i). */
extern void sub_00576170_orig(void);
static uint32_t tCalls, tBad;
static inline double tableLookup(uint32_t self, uint32_t argSlot, float* tfOut) {
    const double t = ((double)MF(argSlot) - MF(self + 4)) * MF(self + 0xc);
    const float tf = (float)t;
    *tfOut = tf;
    const int32_t i = (int32_t)(uint32_t)ftolValue(t, 0);
    const uint32_t table = M32(self + 0x10);
    if (i < 0) return MF(table);
    if ((double)tf < (double)MF(NFS_VA(0x007D7D3C))) return MF(table);
    const int32_t count = (int32_t)M32(self);
    if (!(i < count - 1)) return MF(table + (uint32_t)count * 4 - 4);
    double f = (double)i;
    if (f > (double)tf) f = f - MF(0x784250);   /* the jne skips this on f <= tf or unordered */
    const double frac = (double)tf - f;
    const double w0 = (double)MF(0x784250) - frac;
    const uint32_t e = table + (uint32_t)i * 4;
    return w0 * MF(e) + frac * MF(e + 4);
}
void sub_00576170(void) {
    const int m = mode();
    if (m == 0) { sub_00576170_orig(); return; }
    float tf;
    const double v = tableLookup(g_ecx, g_esp + 4, &tf);
    if (m == 2) {
        sub_00576170_orig();
        ++tCalls;
        const double l = g_cpu.st_win[g_cpu.fp_sp];
        if (memcmp(&l, &v, 8) && tBad++ < 10) printf("[NFSU2 MATH] table mismatch: native %.17g lifted %.17g\n", v, l);
        static uint32_t f;
        const uint32_t now = getD3D9PresentCount();
        if (now != f) { f = now; if (now % 600 == 0) printf("[NFSU2 MATH] verify: table %u/%u\n", tCalls, tBad); }
        return;
    }
    memcpy((void*)ADDR(g_esp + 4), &tf, 4);   /* fst [esp+0x10]: the argument slot */
    pushSt(v);
    g_eax = M32(g_ecx + 0x10);
    g_esp += 8;  /* ret 4 */
}

/* ---- sub_005D3080 (thiscall, ret 4): edge-side test of point p against the quad at this+0x30.., result
 * pushed on ST(0); the argument slot ends up holding the last edge value (as the original's scratch). ---- */
extern void sub_005D3080_orig(void);
static uint32_t eCalls, eBad;
static inline double edgeVal(uint32_t c, uint32_t p, uint32_t ax, uint32_t ay, uint32_t bx, uint32_t by) {
    const double a = ((double)MF(c + by) - MF(c + ay)) * ((double)MF(p) - MF(c + ax));
    const double b = ((double)MF(p + 4) - MF(c + ay)) * ((double)MF(c + bx) - MF(c + ax));
    return a - b;
}
static inline double quadTest(uint32_t c, uint32_t p, float* last) {
    const double C = MF(0x7842a0), Z = MF(NFS_VA(0x007D7D3C));
    double m;
    float s = (float)edgeVal(c, p, 0x30, 0x34, 0x40, 0x44);
    if (C > (double)s) m = C;
    else {
        if ((double)s > Z) { *last = s; return s; }
        m = s;
    }
    s = (float)edgeVal(c, p, 0x40, 0x44, 0x50, 0x54);
    if (!(m > (double)s)) m = s;
    if (m > Z) { *last = s; return m; }
    s = (float)edgeVal(c, p, 0x60, 0x64, 0x30, 0x34);
    if (!(m > (double)s)) m = s;
    if (m > Z) { *last = s; return m; }
    if (*(uint8_t*)ADDR(c + 9) != 4) { *last = s; return m; }
    s = (float)edgeVal(c, p, 0x50, 0x54, 0x60, 0x64);
    if (!(m > (double)s)) m = s;
    *last = s;
    return m;
}
void sub_005D3080(void) {
    const int m = mode();
    if (m == 0) { sub_005D3080_orig(); return; }
    float last;
    const uint32_t p = M32(g_esp + 4);
    const double v = quadTest(g_ecx, p, &last);
    if (m == 2) {
        sub_005D3080_orig();
        ++eCalls;
        const double l = g_cpu.st_win[g_cpu.fp_sp];
        if ((memcmp(&l, &v, 8) || memcmp((const void*)ADDR(g_esp - 4), &last, 4)) && eBad++ < 10)
            printf("[NFSU2 MATH] quad mismatch: native %.17g lifted %.17g\n", v, l);
        static uint32_t f;
        const uint32_t now = getD3D9PresentCount();
        if (now != f) { f = now; if (now % 600 == 0) printf("[NFSU2 MATH] verify: quad %u/%u\n", eCalls, eBad); }
        return;
    }
    memcpy((void*)ADDR(g_esp + 4), &last, 4);
    pushSt(v);
    g_edx = p;
    g_esp += 8;  /* ret 4 */
}

/* ---- culling: sub_005BBF20(M, mn, mx) transforms an AABB in place (cdecl); sub_005BC010 (thiscall, ret 0xc)
 * tests an AABB (optionally transformed) against the 6 frustum planes at [this]+0xc0: 0 outside, 1 intersecting,
 * 2 inside. The original's asymmetry is kept: the min-side product stays double, the max-side one is rounded
 * to float before the compare and the add. ---- */
extern void sub_005BBF20_orig(void);
extern void sub_005BC010_orig(void);
static uint32_t cCalls[2], cBad[2];
static void cullReport(void) {
    static uint32_t f;
    const uint32_t now = getD3D9PresentCount();
    if (now == f) return;
    f = now;
    if (now % 600 == 0) printf("[NFSU2 MATH] verify: aabb %u/%u, frustum %u/%u\n", cCalls[0], cBad[0], cCalls[1], cBad[1]);
}
static void aabbXform(uint32_t M, float* mn, float* mx) {
    double a = MF(M + 0x30), b = MF(M + 0x34), c = MF(M + 0x38), d = c, e = b, f = a;
    for (int i = 0; i < 3; ++i) {
        const uint32_t row = M + 0x10u * (uint32_t)i;
        const double lo = mn[i], hi = mx[i];
        double p; float q;
        p = lo * MF(row);     q = (float)(hi * MF(row));
        if (p < (double)q) { a += p; f += q; } else { a += q; f += p; }
        p = lo * MF(row + 4); q = (float)(hi * MF(row + 4));
        if (p < (double)q) { b += p; e += q; } else { b += q; e += p; }
        p = lo * MF(row + 8); q = (float)(hi * MF(row + 8));
        if (p < (double)q) { c += p; d += q; } else { c += q; d += p; }
    }
    mn[0] = (float)a; mn[1] = (float)b; mn[2] = (float)c;
    mx[0] = (float)f; mx[1] = (float)e; mx[2] = (float)d;
}
void sub_005BBF20(void) {
    const int m = mode();
    if (m == 0) { sub_005BBF20_orig(); return; }
    const uint32_t M = M32(g_esp + 4), pmn = M32(g_esp + 8), pmx = M32(g_esp + 12);
    float mn[3], mx[3];
    memcpy(mn, (const void*)ADDR(pmn), 12); memcpy(mx, (const void*)ADDR(pmx), 12);
    aabbXform(M, mn, mx);
    if (m == 2) {
        sub_005BBF20_orig();
        ++cCalls[0];
        if ((memcmp(mn, (const void*)ADDR(pmn), 12) || memcmp(mx, (const void*)ADDR(pmx), 12)) && cBad[0]++ < 10)
            printf("[NFSU2 MATH] aabb mismatch: native %.9g %.9g %.9g / %.9g %.9g %.9g, lifted %.9g %.9g %.9g / %.9g %.9g %.9g\n",
                   mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], MF(pmn), MF(pmn + 4), MF(pmn + 8), MF(pmx), MF(pmx + 4), MF(pmx + 8));
        cullReport();
        return;
    }
    memcpy((void*)ADDR(pmn), mn, 12); memcpy((void*)ADDR(pmx), mx, 12);
    g_eax = M;
    g_esp += 4;
}
static uint32_t frustumTest(uint32_t self, uint32_t A, uint32_t B, uint32_t Mx) {
    float mn[3], mx[3];
    memcpy(mn, (const void*)ADDR(A), 12); memcpy(mx, (const void*)ADDR(B), 12);
    if (Mx) aabbXform(Mx, mn, mx);
    const double k = MF(0x784260), Z = MF(NFS_VA(0x007D7D3C));
    float c[3], h[3];
    for (int j = 0; j < 3; ++j) c[j] = (float)(((double)mx[j] + mn[j]) * k);
    for (int j = 0; j < 3; ++j) h[j] = (float)((double)mx[j] - c[j]);
    uint32_t p = M32(self) + 0xc4;
    int inter = 0;
    for (int i = 0; i < 6; ++i, p += 0x10) {
        const double n0 = MF(p - 4), n1 = MF(p), n2 = MF(p + 4);
        double r = fabs(n2) * h[2];
        r = r + fabs(n0) * h[0];
        r = r + fabs(n1) * h[1];
        double s = (double)c[2] * n2;
        s = s + (double)c[0] * n0;
        s = s + (double)c[1] * n1;
        s = s + MF(p + 8);
        if (s + r < Z) return 0;
        if (s - r < Z) inter = 1;
    }
    return inter ? 1u : 2u;
}
void sub_005BC010(void) {
    const int m = mode();
    if (m == 0) { sub_005BC010_orig(); return; }
    const uint32_t r = frustumTest(g_ecx, M32(g_esp + 4), M32(g_esp + 8), M32(g_esp + 12));
    if (m == 2) {
        sub_005BC010_orig();
        ++cCalls[1];
        if (g_eax != r && cBad[1]++ < 10) printf("[NFSU2 MATH] frustum mismatch: native %u lifted %u\n", r, g_eax);
        cullReport();
        return;
    }
    g_eax = r;
    g_esp += 16;  /* ret 0xc */
}

/* Local x87 window verification (arm64_guest.h NFS_LW_CHECK builds): a call whose real depth change differs
 * from the one ports/r36s/lw/analyze.py assumed. The first 64 are logged; NFSU2 LW summary every 600. */
void nfs_lw_mismatch(uint32_t va, int expected, int actual) {
    static uint32_t n;
    const uint32_t k = __atomic_add_fetch(&n, 1, __ATOMIC_RELAXED);
    if (k <= 64) printf("[NFSU2 LW] mismatch in %08x: call changed the x87 depth by %d, assumed %d\n", va, actual, expected);
    else if (k % 600 == 0) printf("[NFSU2 LW] %u mismatches so far\n", k);
}
