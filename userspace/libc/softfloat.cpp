/* IEEE-754 binary64 / binary32 soft-float for RioOS user space (the CPU is used with -mgeneral-regs-only, so
   the compiler lowers double/float arithmetic to the libgcc-style helpers defined here).
   Round-to-nearest-even, subnormals, Inf and NaN are handled. float arithmetic is done in double and rounded
   once to float (exact for + - * / and sqrt). Compiles on the host too (tests/softfloat_host_test.cpp). */
typedef unsigned long long u64;
typedef long long i64;
typedef unsigned int u32;
typedef int i32;
typedef unsigned __int128 u128;

#define F64_QNAN 0x7FF8000000000000ULL
#define F64_INF  0x7FF0000000000000ULL
#define SIGNBIT  0x8000000000000000ULL

namespace {

inline int clz64(u64 x) { return x ? __builtin_clzll(x) : 64; }

/* Round and pack. sig has its leading bit at bit 62 (or is smaller for subnormal inputs with e<=0);
   value = sig * 2^(e - bias - 62). F fraction bits, EMAX all-ones exponent. Returns the bit pattern without sign. */
template <int F, int EMAX>
u64 rpack(int e, u64 sig)
{
    const int rbits = 62 - F;
    const u64 rmask = (1ULL << rbits) - 1, half = 1ULL << (rbits - 1);
    if (e >= EMAX) return (u64)EMAX << F;
    if (e <= 0)
    {
        int sh = 1 - e;
        if (sh > 63) sig = sig ? 1 : 0;
        else { u64 lost = sig & ((1ULL << sh) - 1); sig = (sig >> sh) | (lost ? 1 : 0); }
        e = 0;
    }
    u64 rb = sig & rmask;
    sig >>= rbits;
    if (rb > half || (rb == half && (sig & 1))) ++sig;
    u64 bits = e == 0 ? sig : (((u64)(e - 1) << F) + sig);
    if (bits >= ((u64)EMAX << F)) bits = (u64)EMAX << F;
    return bits;
}

struct Unp { int sign; int e; u64 m; int cls; };   /* cls: 0 finite, 1 inf, 2 nan; m has hidden bit at 52 (normalized), e unbiased+1023 */

inline Unp unpack(u64 x)
{
    Unp u; u.sign = (int)(x >> 63); int e = (int)((x >> 52) & 0x7FF); u64 f = x & 0xFFFFFFFFFFFFFULL;
    u.cls = 0;
    if (e == 0x7FF) { u.cls = f ? 2 : 1; u.e = e; u.m = f; return u; }
    if (e == 0) { if (!f) { u.e = 0; u.m = 0; return u; } int sh = clz64(f) - 11; u.m = f << sh; u.e = 1 - sh; return u; }   /* normalize subnormal */
    u.e = e; u.m = f | (1ULL << 52); return u;
}

inline u64 mk(int sign, u64 bits) { return ((u64)sign << 63) | bits; }

/* result of adding magnitudes / subtracting for e,sig normalization */
u64 finish_sub(int sign, int e, u64 x)
{
    if (!x) return 0;
    while (!(x >> 62) && e > 1) { x <<= 1; --e; }
    if (!(x >> 62)) { e = 0; x <<= 1; }
    return mk(sign, rpack<52, 0x7FF>(e, x));
}

}  // namespace

extern "C" {

u64 sf_add(u64 a, u64 b);

u64 sf_neg(u64 a) { return a ^ SIGNBIT; }

u64 sf_add(u64 a, u64 b)
{
    int ea0 = (int)((a >> 52) & 0x7FF), eb0 = (int)((b >> 52) & 0x7FF);
    if (ea0 == 0x7FF || eb0 == 0x7FF)
    {
        if ((ea0 == 0x7FF && (a << 12)) || (eb0 == 0x7FF && (b << 12))) return F64_QNAN;
        if (ea0 == 0x7FF && eb0 == 0x7FF) return (a == b) ? a : F64_QNAN;
        return ea0 == 0x7FF ? a : b;
    }
    if ((a & ~SIGNBIT) < (b & ~SIGNBIT)) { u64 t = a; a = b; b = t; }   /* |a| >= |b| */
    int sa = (int)(a >> 63), sb = (int)(b >> 63);
    int ea = (int)((a >> 52) & 0x7FF), eb = (int)((b >> 52) & 0x7FF);
    u64 ma = a & 0xFFFFFFFFFFFFFULL, mb = b & 0xFFFFFFFFFFFFFULL;
    if (ea) ma |= 1ULL << 52; else ea = 1;
    if (eb) mb |= 1ULL << 52; else eb = 1;
    if (!ma && !mb) return (sa && sb) ? SIGNBIT : 0;     /* +0 + -0 = +0 */
    u64 xa = ma << 10, xb = mb << 10;
    int d = ea - eb;
    if (d > 0) { if (d >= 63) xb = xb ? 1 : 0; else { u64 lost = xb & ((1ULL << d) - 1); xb = (xb >> d) | (lost ? 1 : 0); } }
    int e = ea;
    if (sa == sb)
    {
        u64 s = xa + xb;
        if (s >> 63) { s = (s >> 1) | (s & 1); ++e; }
        if (!(s >> 62))                                 /* subnormal sum */
        { if (e > 1) { /* cannot happen: xa has bit 62 when e>1 */ } s <<= 1; return mk(sa, rpack<52, 0x7FF>(0, s)); }
        return mk(sa, rpack<52, 0x7FF>(e, s));
    }
    u64 diff = xa - xb;
    if (!diff) return 0;
    return finish_sub(sa, e, diff);
}

u64 sf_sub(u64 a, u64 b) { return sf_add(a, b ^ SIGNBIT); }

u64 sf_mul(u64 a, u64 b)
{
    Unp x = unpack(a), y = unpack(b); int s = x.sign ^ y.sign;
    if (x.cls == 2 || y.cls == 2) return F64_QNAN;
    if (x.cls == 1 || y.cls == 1) { if ((x.cls == 0 && !x.m) || (y.cls == 0 && !y.m)) return F64_QNAN; return mk(s, F64_INF); }
    if (!x.m || !y.m) return mk(s, 0);
    u128 p = (u128)x.m * y.m;
    int e = x.e + y.e - 1023, sh = 42;
    if ((p >> 105) & 1) { sh = 43; ++e; }
    u128 mask = (((u128)1) << sh) - 1;
    u64 sig = (u64)(p >> sh) | ((p & mask) ? 1 : 0);
    return mk(s, rpack<52, 0x7FF>(e, sig));
}

u64 sf_div(u64 a, u64 b)
{
    Unp x = unpack(a), y = unpack(b); int s = x.sign ^ y.sign;
    if (x.cls == 2 || y.cls == 2) return F64_QNAN;
    if (x.cls == 1) return y.cls == 1 ? F64_QNAN : mk(s, F64_INF);
    if (y.cls == 1) return mk(s, 0);
    if (!y.m) return x.m ? mk(s, F64_INF) : F64_QNAN;
    if (!x.m) return mk(s, 0);
    int e = x.e - y.e + 1023;
    u64 r = x.m, q = 0;
    if (r < y.m) { r <<= 1; --e; }
    for (int i = 0; i < 63; ++i)
    {
        q <<= 1;
        if (r >= y.m) { r -= y.m; q |= 1; }
        r <<= 1;
    }
    q |= r ? 1 : 0;
    return mk(s, rpack<52, 0x7FF>(e, q));
}

u64 sf_sqrt(u64 a)
{
    Unp x = unpack(a);
    if (x.cls == 2) return F64_QNAN;
    if (x.cls == 1) return x.sign ? F64_QNAN : a;
    if (!x.m) return a;
    if (x.sign) return F64_QNAN;
    u64 m = x.m; int E = x.e - 1075;
    if (E & 1) { m <<= 1; --E; }
    u128 M = (u128)m << 62; int E2 = E - 62;
    u128 res = 0, bit = ((u128)1) << 126;
    while (bit > M) bit >>= 2;
    while (bit)
    {
        if (M >= res + bit) { M -= res + bit; res = (res >> 1) + bit; } else res >>= 1;
        bit >>= 2;
    }
    u64 root = (u64)res;
    u64 sig = (root << 5) | (M ? 1 : 0);
    int e = 1023 + 57 + E2 / 2;
    return rpack<52, 0x7FF>(e, sig);
}

/* compare: -1 less, 0 equal, 1 greater, 2 unordered */
int sf_cmp(u64 a, u64 b)
{
    if (((a & ~SIGNBIT) > F64_INF) || ((b & ~SIGNBIT) > F64_INF)) return 2;
    if (!((a | b) & ~SIGNBIT)) return 0;
    int sa = (int)(a >> 63), sb = (int)(b >> 63);
    if (sa != sb) return sa ? -1 : 1;
    if (a == b) return 0;
    int lt = (a & ~SIGNBIT) < (b & ~SIGNBIT);
    return sa ? (lt ? 1 : -1) : (lt ? -1 : 1);
}

u64 sf_from_u64(u64 v, int neg)
{
    if (!v) return 0;
    int n = 63 - clz64(v);
    u64 sig = n > 62 ? ((v >> 1) | (v & 1)) : (v << (62 - n));
    return mk(neg, rpack<52, 0x7FF>(1023 + n, sig));
}

/* truncate toward zero; saturates; returns magnitude in *mag, sign in return (1 negative) */
static int sf_to_u64(u64 a, u64* mag, int* overflow)
{
    Unp x = unpack(a); *overflow = 0; *mag = 0;
    if (x.cls == 2) { *overflow = 1; return 0; }
    if (x.cls == 1) { *overflow = 1; *mag = ~0ULL; return x.sign; }
    if (!x.m || x.e < 1023) return x.sign;
    int sh = x.e - 1023 - 52;
    if (sh >= 12) { *overflow = 1; *mag = ~0ULL; return x.sign; }
    *mag = sh >= 0 ? (x.m << sh) : (x.m >> (-sh));
    return x.sign;
}

/* ---- float <-> double ---- */
u64 sf_f32_to_f64(u32 f)
{
    int s = (int)(f >> 31), e = (int)((f >> 23) & 0xFF); u64 m = f & 0x7FFFFF;
    if (e == 0xFF) return m ? F64_QNAN : mk(s, F64_INF);
    if (e == 0) { if (!m) return mk(s, 0); int sh = clz64(m) - 40; m <<= sh; e = 1 - sh; m &= 0x7FFFFF; return mk(s, ((u64)(e + 896) << 52) | (m << 29)); }
    return mk(s, ((u64)(e + 896) << 52) | (m << 29));
}

u32 sf_f64_to_f32(u64 a)
{
    Unp x = unpack(a); u32 s = (u32)x.sign << 31;
    if (x.cls == 2) return 0x7FC00000u;
    if (x.cls == 1) return s | 0x7F800000u;
    if (!x.m) return s;
    u64 sig = x.m << 10;
    return s | (u32)rpack<23, 0xFF>(x.e - 1023 + 127, sig);
}

}  // extern "C"

/* ======== libgcc / compiler-rt entry points ======== */
static inline u64 d2u(double d) { u64 r; __builtin_memcpy(&r, &d, 8); return r; }
static inline double u2d(u64 r) { double d; __builtin_memcpy(&d, &r, 8); return d; }
static inline u32 f2u(float f) { u32 r; __builtin_memcpy(&r, &f, 4); return r; }
static inline float u2f(u32 r) { float f; __builtin_memcpy(&f, &r, 4); return f; }
extern "C" {

double __adddf3(double a, double b) { u64 r = sf_add(d2u(a), d2u(b)); return u2d(r); }
double __subdf3(double a, double b) { u64 r = sf_sub(d2u(a), d2u(b)); return u2d(r); }
double __muldf3(double a, double b) { u64 r = sf_mul(d2u(a), d2u(b)); return u2d(r); }
double __divdf3(double a, double b) { u64 r = sf_div(d2u(a), d2u(b)); return u2d(r); }
double __negdf2(double a) { u64 r = sf_neg(d2u(a)); return u2d(r); }
double sqrt(double a) { u64 r = sf_sqrt(d2u(a)); return u2d(r); }

static inline int dcmp(double a, double b) { return sf_cmp(d2u(a), d2u(b)); }
int __eqdf2(double a, double b) { int c = dcmp(a, b); return c == 0 ? 0 : 1; }
int __nedf2(double a, double b) { int c = dcmp(a, b); return c == 0 ? 0 : 1; }
int __ltdf2(double a, double b) { int c = dcmp(a, b); return c == 2 ? 1 : c; }
int __ledf2(double a, double b) { int c = dcmp(a, b); return c == 2 ? 1 : c; }
int __gtdf2(double a, double b) { int c = dcmp(a, b); return c == 2 ? -1 : c; }
int __gedf2(double a, double b) { int c = dcmp(a, b); return c == 2 ? -1 : c; }
int __unorddf2(double a, double b) { return dcmp(a, b) == 2; }

double __floatsidf(i32 v) { u64 r = sf_from_u64(v < 0 ? (u64)(-(i64)v) : (u64)v, v < 0); return u2d(r); }
double __floatunsidf(u32 v) { u64 r = sf_from_u64(v, 0); return u2d(r); }
double __floatdidf(i64 v) { u64 r = sf_from_u64(v < 0 ? (u64)0 - (u64)v : (u64)v, v < 0); return u2d(r); }
double __floatundidf(u64 v) { u64 r = sf_from_u64(v, 0); return u2d(r); }

i64 __fixdfdi(double a)
{
    u64 mag; int ov; int neg = sf_to_u64(d2u(a), &mag, &ov);
    if (ov) return neg ? (i64)SIGNBIT : (i64)0x7FFFFFFFFFFFFFFFLL;
    if (neg) { if (mag > SIGNBIT) return (i64)SIGNBIT; return (i64)(0 - mag); }
    if (mag > 0x7FFFFFFFFFFFFFFFULL) return 0x7FFFFFFFFFFFFFFFLL;
    return (i64)mag;
}
u64 __fixunsdfdi(double a)
{
    u64 mag; int ov; int neg = sf_to_u64(d2u(a), &mag, &ov);
    if (neg) return 0;
    return mag;
}
i32 __fixdfsi(double a) { i64 v = __fixdfdi(a); if (v > 2147483647LL) return 2147483647; if (v < -2147483648LL) return (i32)0x80000000; return (i32)v; }
u32 __fixunsdfsi(double a) { u64 v = __fixunsdfdi(a); return v > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (u32)v; }

/* float */
double __extendsfdf2(float f) { u32 b = f2u(f); u64 r = sf_f32_to_f64(b); return u2d(r); }
float __truncdfsf2(double d) { u32 r = sf_f64_to_f32(d2u(d)); return u2f(r); }
float __addsf3(float a, float b) { return __truncdfsf2(__adddf3(__extendsfdf2(a), __extendsfdf2(b))); }
float __subsf3(float a, float b) { return __truncdfsf2(__subdf3(__extendsfdf2(a), __extendsfdf2(b))); }
float __mulsf3(float a, float b) { return __truncdfsf2(__muldf3(__extendsfdf2(a), __extendsfdf2(b))); }
float __divsf3(float a, float b) { return __truncdfsf2(__divdf3(__extendsfdf2(a), __extendsfdf2(b))); }
float __negsf2(float a) { u32 b = f2u(a) ^ 0x80000000u; return u2f(b); }
int __eqsf2(float a, float b) { return __eqdf2(__extendsfdf2(a), __extendsfdf2(b)); }
int __nesf2(float a, float b) { return __nedf2(__extendsfdf2(a), __extendsfdf2(b)); }
int __ltsf2(float a, float b) { return __ltdf2(__extendsfdf2(a), __extendsfdf2(b)); }
int __lesf2(float a, float b) { return __ledf2(__extendsfdf2(a), __extendsfdf2(b)); }
int __gtsf2(float a, float b) { return __gtdf2(__extendsfdf2(a), __extendsfdf2(b)); }
int __gesf2(float a, float b) { return __gedf2(__extendsfdf2(a), __extendsfdf2(b)); }
int __unordsf2(float a, float b) { return __unorddf2(__extendsfdf2(a), __extendsfdf2(b)); }
float __floatsisf(i32 v) { return __truncdfsf2(__floatsidf(v)); }
float __floatunsisf(u32 v) { return __truncdfsf2(__floatunsidf(v)); }
float __floatdisf(i64 v) { return __truncdfsf2(__floatdidf(v)); }
float __floatundisf(u64 v) { return __truncdfsf2(__floatundidf(v)); }
i32 __fixsfsi(float a) { return __fixdfsi(__extendsfdf2(a)); }
u32 __fixunssfsi(float a) { return __fixunsdfsi(__extendsfdf2(a)); }
i64 __fixsfdi(float a) { return __fixdfdi(__extendsfdf2(a)); }
u64 __fixunssfdi(float a) { return __fixunsdfdi(__extendsfdf2(a)); }

}  // extern "C"
