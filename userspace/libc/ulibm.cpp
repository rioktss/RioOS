/* RioOS libm: double-precision math on top of the soft-float core (userspace/libc/softfloat.cpp).
   Accuracy target: within a few ulp for ordinary arguments (tests/libm_host_test.cpp). */
typedef unsigned long long u64;
typedef long long i64;
typedef unsigned int u32;

static inline u64 d2u(double d) { u64 r; __builtin_memcpy(&r, &d, 8); return r; }
static inline double u2d(u64 r) { double d; __builtin_memcpy(&d, &r, 8); return d; }

#define M_PI_D   3.14159265358979323846
#define M_LN2_D  0.693147180559945309417
#define M_LN2_HI 0.693147180369123816490
#define M_LN2_LO 1.90821492927058770002e-10
#define M_LN10_D 2.30258509299404568402

extern "C" {

double fabs(double x) { return u2d(d2u(x) & 0x7FFFFFFFFFFFFFFFULL); }
double copysign(double x, double y) { return u2d((d2u(x) & 0x7FFFFFFFFFFFFFFFULL) | (d2u(y) & 0x8000000000000000ULL)); }
int isnan_d(double x) { return (d2u(x) & 0x7FFFFFFFFFFFFFFFULL) > 0x7FF0000000000000ULL; }
int isinf_d(double x) { return (d2u(x) & 0x7FFFFFFFFFFFFFFFULL) == 0x7FF0000000000000ULL; }
int isfinite_d(double x) { return (d2u(x) & 0x7FF0000000000000ULL) != 0x7FF0000000000000ULL; }
int signbit_d(double x) { return (int)(d2u(x) >> 63); }

double ldexp(double x, int n)
{
    u64 b = d2u(x); int e = (int)((b >> 52) & 0x7FF);
    if (e == 0x7FF || !(b & 0x7FFFFFFFFFFFFFFFULL)) return x;
    if (e == 0) { x = x * 18014398509481984.0; b = d2u(x); e = (int)((b >> 52) & 0x7FF) - 54; }   /* subnormal: scale by 2^54 */
    long ne = (long)e + n;
    if (ne >= 0x7FF) return u2d((b & 0x8000000000000000ULL) | 0x7FF0000000000000ULL);
    if (ne > 0) return u2d((b & 0x800FFFFFFFFFFFFFULL) | ((u64)ne << 52));
    if (ne <= -54) return u2d(b & 0x8000000000000000ULL);
    b = (b & 0x800FFFFFFFFFFFFFULL) | (1ULL << 52);   /* gradual underflow: build normal 2^(ne+54) then scale down exactly once */
    double m = u2d((b & 0x800FFFFFFFFFFFFFULL) | ((u64)(ne + 54) << 52));
    return m * 5.55111512312578270212e-17;             /* 2^-54 */
}

double frexp(double x, int* e)
{
    u64 b = d2u(x); int ex = (int)((b >> 52) & 0x7FF);
    if (ex == 0x7FF || !(b & 0x7FFFFFFFFFFFFFFFULL)) { *e = 0; return x; }
    int adj = 0;
    if (ex == 0) { x = x * 18014398509481984.0; b = d2u(x); ex = (int)((b >> 52) & 0x7FF); adj = -54; }
    *e = ex - 1022 + adj;
    return u2d((b & 0x800FFFFFFFFFFFFFULL) | (1022ULL << 52));
}

double trunc(double x)
{
    u64 b = d2u(x); int e = (int)((b >> 52) & 0x7FF) - 1023;
    if (e >= 52) return x;
    if (e < 0) return u2d(b & 0x8000000000000000ULL);
    return u2d(b & ~((1ULL << (52 - e)) - 1));
}
double floor(double x) { double t = trunc(x); return (x < t) ? t - 1.0 : t; }
double ceil(double x) { double t = trunc(x); return (x > t) ? t + 1.0 : t; }
double round(double x) { double t = trunc(x); double d = x - t; if (d >= 0.5) return t + 1.0; if (d <= -0.5) return t - 1.0; return t; }
double rint(double x)
{
    double t = floor(x); double d = x - t;
    if (d < 0.5) return t;
    if (d > 0.5) return t + 1.0;
    return (trunc(t / 2.0) * 2.0 == t) ? t : t + 1.0;
}
double modf(double x, double* ip) { double t = trunc(x); *ip = t; return isinf_d(x) ? copysign(0.0, x) : x - t; }

double fmod(double x, double y)
{
    if (isnan_d(x) || isnan_d(y) || isinf_d(x) || y == 0.0) return u2d(0x7FF8000000000000ULL);
    if (isinf_d(y)) return x;
    double ax = fabs(x), ay = fabs(y);
    while (ax >= ay)
    {
        int ex, ey; frexp(ax, &ex); frexp(ay, &ey);
        double t = ldexp(ay, ex - ey);
        if (t > ax) t = t / 2.0;
        ax = ax - t;
    }
    return copysign(ax, x);
}

double fma(double a, double b, double c) { return a * b + c; }   /* not fused; keeps the symbol the compiler may emit */
float fmaf(float a, float b, float c) { return (float)((double)a * (double)b + (double)c); }
double fmin(double a, double b) { if (isnan_d(a)) return b; if (isnan_d(b)) return a; return a < b ? a : b; }
double fmax(double a, double b) { if (isnan_d(a)) return b; if (isnan_d(b)) return a; return a > b ? a : b; }

static double exp_dd(double x, double lo)
{
    if (isnan_d(x)) return x;
    if (x > 709.782712893384) return u2d(0x7FF0000000000000ULL);
    if (x < -745.2) return 0.0;
    double kd = floor(x / M_LN2_D + 0.5);
    int k = (int)kd;
    double r = ((x - kd * M_LN2_HI) - kd * M_LN2_LO) + lo;
    double term = 1.0, sum = 1.0;
    for (int i = 1; i < 26; ++i) { term = term * r / (double)i; sum += term; }
    return ldexp(sum, k);
}
double exp(double x) { return exp_dd(x, 0.0); }

static double log_dd(double x, double* lo_out)
{
    if (isnan_d(x) || x < 0.0) return u2d(0x7FF8000000000000ULL);
    if (x == 0.0) return u2d(0xFFF0000000000000ULL);
    if (isinf_d(x)) return x;
    int k; double m = frexp(x, &k);                    /* m in [0.5, 1) */
    if (m < 0.70710678118654752440) { m *= 2.0; --k; }
    double f = (m - 1.0) / (m + 1.0), f2 = f * f, term = f, sum = 0.0;
    for (int i = 1; i < 60; i += 2)
    {
        double add = term / (double)i;
        sum += add;
        term *= f2;
        if (fabs(add) < 1e-18 * fabs(sum)) break;
    }
    double a = (double)k * M_LN2_HI, b = 2.0 * sum;
    double hi = a + b, bb = hi - a, err = (a - (hi - bb)) + (b - bb);
    *lo_out = err + (double)k * M_LN2_LO;
    return hi;
}
double log(double x)
{
    if (isnan_d(x) || x < 0.0) return u2d(0x7FF8000000000000ULL);
    if (x == 0.0) return u2d(0xFFF0000000000000ULL);
    if (isinf_d(x)) return x;
    double lo; double hi = log_dd(x, &lo);
    return hi + lo;
}
double log2(double x) { return log(x) / M_LN2_D; }
double log10(double x) { return log(x) / M_LN10_D; }
double log1p(double x) { return log(1.0 + x); }
double expm1(double x) { return exp(x) - 1.0; }

double pow(double x, double y)
{
    if (y == 0.0) return 1.0;
    if (isnan_d(x) || isnan_d(y)) return u2d(0x7FF8000000000000ULL);
    if (x == 1.0) return 1.0;
    double ty = trunc(y);
    int yint = (ty == y);
    if (yint && fabs(y) <= 4096.0)
    {
        long n = (long)ty; int neg = n < 0; if (neg) n = -n;
        double r = 1.0, b = x;
        while (n) { if (n & 1) r *= b; b *= b; n >>= 1; }
        return neg ? 1.0 / r : r;
    }
    if (x == 0.0) return y < 0.0 ? u2d(0x7FF0000000000000ULL) : 0.0;
    if (x < 0.0) { if (!yint) return u2d(0x7FF8000000000000ULL); double r = pow(-x, y); return (trunc(y / 2.0) * 2.0 != ty) ? -r : r; }
    double lo; double lh = log_dd(x, &lo);
    double ph = y * lh;
    /* Dekker two-product error of y*lh */
    double c = 134217729.0 * y, yh = c - (c - y), yl = y - yh;
    c = 134217729.0 * lh; double lhh = c - (c - lh), lhl = lh - lhh;
    double perr = ((yh * lhh - ph) + yh * lhl + yl * lhh) + yl * lhl;
    if (ph > 709.8) return u2d(0x7FF0000000000000ULL);
    if (ph < -745.2) return 0.0;
    return exp_dd(ph, perr + y * lo);
}

double sqrt(double x);   /* softfloat.cpp */
double cbrt(double x)
{
    if (x == 0.0 || isnan_d(x) || isinf_d(x)) return x;
    double a = fabs(x), r = exp(log(a) / 3.0);
    r = r - (r * r * r - a) / (3.0 * r * r);
    r = r - (r * r * r - a) / (3.0 * r * r);
    return copysign(r, x);
}
double hypot(double a, double b) { return sqrt(a * a + b * b); }

/* ---- trigonometry ---- */
static const double PIO2_1 = 1.57079632673412561417e+00, PIO2_1T = 6.07710050650619224932e-11;

static void sincos_kernel(double r, double* s, double* c)   /* |r| <= pi/4 */
{
    double r2 = r * r, t = r, sum = r;
    for (int i = 1; i < 14; ++i) { t = -t * r2 / (double)((2 * i) * (2 * i + 1)); sum += t; }
    *s = sum;
    t = 1.0; sum = 1.0;
    for (int i = 1; i < 14; ++i) { t = -t * r2 / (double)((2 * i - 1) * (2 * i)); sum += t; }
    *c = sum;
}
static int reduce_pio2(double x, double* r)
{
    double n = floor(x * 0.63661977236758134308 + 0.5);
    *r = (x - n * PIO2_1) - n * PIO2_1T;
    double m = fmod(n, 4.0); if (m < 0.0) m += 4.0;
    return (int)m;
}
double sin(double x)
{
    if (isnan_d(x) || isinf_d(x)) return u2d(0x7FF8000000000000ULL);
    double r, s, c; int q = reduce_pio2(x, &r); sincos_kernel(r, &s, &c);
    switch (q) { case 0: return s; case 1: return c; case 2: return -s; default: return -c; }
}
double cos(double x)
{
    if (isnan_d(x) || isinf_d(x)) return u2d(0x7FF8000000000000ULL);
    double r, s, c; int q = reduce_pio2(x, &r); sincos_kernel(r, &s, &c);
    switch (q) { case 0: return c; case 1: return -s; case 2: return -c; default: return s; }
}
double tan(double x) { return sin(x) / cos(x); }

double atan(double x)
{
    if (isnan_d(x)) return x;
    double a = fabs(x), res; int inv = 0, shift = 0;
    if (a > 1.0) { a = 1.0 / a; inv = 1; }
    if (a > 0.2679491924311227) { a = (a * 1.7320508075688772 - 1.0) / (1.7320508075688772 + a); shift = 1; }   /* tan(pi/12) */
    double a2 = a * a, t = a, sum = a;
    for (int i = 1; i < 60; ++i) { t = -t * a2; double add = t / (double)(2 * i + 1); sum += add; if (fabs(add) < 1e-19) break; }
    res = sum + (shift ? 0.52359877559829887308 : 0.0);
    if (inv) res = 1.57079632679489661923 - res;
    return x < 0.0 ? -res : res;
}
double atan2(double y, double x)
{
    if (isnan_d(x) || isnan_d(y)) return u2d(0x7FF8000000000000ULL);
    if (x == 0.0) { if (y == 0.0) return signbit_d(x) ? (signbit_d(y) ? -M_PI_D : M_PI_D) : y; return y > 0.0 ? 1.57079632679489661923 : -1.57079632679489661923; }
    double a = atan(y / x);
    if (x > 0.0) return a;
    return y >= 0.0 ? a + M_PI_D : a - M_PI_D;
}
double asin(double x) { if (x > 1.0 || x < -1.0 || isnan_d(x)) return u2d(0x7FF8000000000000ULL); return atan2(x, sqrt((1.0 - x) * (1.0 + x))); }
double acos(double x) { if (x > 1.0 || x < -1.0 || isnan_d(x)) return u2d(0x7FF8000000000000ULL); return 2.0 * atan2(sqrt(1.0 - x), sqrt(1.0 + x)); }
double sinh(double x)
{
    if (fabs(x) < 0.5) { double x2 = x * x, t = x, sum = x; for (int i = 1; i < 12; ++i) { t = t * x2 / (double)((2 * i) * (2 * i + 1)); sum += t; } return sum; }
    double e = exp(x); return (e - 1.0 / e) / 2.0;
}
double cosh(double x) { double e = exp(x); return (e + 1.0 / e) / 2.0; }
double tanh(double x) { if (fabs(x) < 0.5) return sinh(x) / cosh(x); if (x > 20.0) return 1.0; if (x < -20.0) return -1.0; double e = exp(2.0 * x); return (e - 1.0) / (e + 1.0); }

/* float wrappers */
float sqrtf(float x) { return (float)sqrt((double)x); }
float fabsf(float x) { return (float)fabs((double)x); }
float floorf(float x) { return (float)floor((double)x); }
float ceilf(float x) { return (float)ceil((double)x); }
float sinf(float x) { return (float)sin((double)x); }
float cosf(float x) { return (float)cos((double)x); }
float tanf(float x) { return (float)tan((double)x); }
float atanf(float x) { return (float)atan((double)x); }
float atan2f(float y, float x) { return (float)atan2((double)y, (double)x); }
float expf(float x) { return (float)exp((double)x); }
float logf(float x) { return (float)log((double)x); }
float powf(float x, float y) { return (float)pow((double)x, (double)y); }
float fmodf(float x, float y) { return (float)fmod((double)x, (double)y); }

}  // extern "C"
