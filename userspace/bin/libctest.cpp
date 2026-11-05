#include "../libc/ulibc.h"
/* libc part 2 self-test: stdio files, sscanf, strtok, integer math. Prints one line per check and a summary. */
static int fails = 0, total = 0;
static void check(const char* name, int ok) { ++total; if (!ok) ++fails; printf("  %s %s\n", ok ? "PASS" : "FAIL", name); }
int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    FILE* f = fopen("/tmp/libctest.txt", "w");
    check("fopen w", f != 0);
    if (f)
    {
        fprintf(f, "line %d\nvalue=%s\n", 1, "abc");
        fputs("third\n", f);
        check("fclose", fclose(f) == 0);
    }
    f = fopen("/tmp/libctest.txt", "r");
    char buf[64];
    check("fopen r", f != 0);
    if (f)
    {
        check("fgets 1", fgets(buf, sizeof(buf), f) && strcmp(buf, "line 1\n") == 0);
        int n = 0; char word[16];
        check("fgets 2", fgets(buf, sizeof(buf), f) && sscanf(buf, "value=%s", word) == 1 && strcmp(word, "abc") == 0);
        (void)n;
        long pos = ftell(f);
        check("ftell", pos == 7 + 10);
        fseek(f, 0, SEEK_SET);
        check("rewind read", fgetc(f) == 'l');
        fseek(f, 0, SEEK_END);
        check("fseek end", ftell(f) == 23);
        check("eof", fgetc(f) < 0 && feof(f));
        fclose(f);
    }
    f = fopen("/tmp/libctest.txt", "a");
    if (f) { fputs("appended\n", f); fclose(f); }
    f = fopen("/tmp/libctest.txt", "r");
    int lines = 0; if (f) { while (fgets(buf, sizeof(buf), f)) ++lines; fclose(f); }
    check("append (4 lines)", lines == 4);
    int a = 0; unsigned x = 0; char s[16];
    check("sscanf", sscanf("12 ff hello", "%d %x %s", &a, &x, s) == 3 && a == 12 && x == 255 && strcmp(s, "hello") == 0);
    char text[] = "a,b,,c"; char* sv = 0;
    char* t1 = strtok_r(text, ",", &sv); char* t2 = strtok_r(0, ",", &sv); char* t3 = strtok_r(0, ",", &sv);
    check("strtok_r", t1 && t2 && t3 && strcmp(t1, "a") == 0 && strcmp(t2, "b") == 0 && strcmp(t3, "c") == 0);
    check("strcasecmp", strcasecmp("AbC", "aBc") == 0 && strncasecmp("abcd", "ABxx", 2) == 0);
    check("strspn/strcspn", strspn("aabbc", "ab") == 4 && strcspn("hello world", " ") == 5);
    check("isqrt_u", isqrt_u(144) == 12 && isqrt_u(99) == 9);
    check("gcd/lcm/ipow", gcd_l(12, 18) == 6 && lcm_l(4, 6) == 12 && ipow_l(3, 4) == 81);
    static int arr[6] = { 1, 3, 5, 7, 9, 11 };
    int key = 7; 
    int cmp(const void*, const void*);
    check("bsearch", bsearch(&key, arr, 6, sizeof(int), [](const void* p, const void* q) { return *(const int*)p - *(const int*)q; }) == &arr[3]);
    check("strerror", strcmp(strerror(ENOENT), "no such file or directory") == 0);
    /* ---- libc part 3: floating point, printf, scanf, locale, time, env, malloc ---- */
    char fb[128];
    snprintf(fb, sizeof(fb), "%f", 3.14159265358979); check("printf %f", strcmp(fb, "3.141593") == 0);
    snprintf(fb, sizeof(fb), "%.3f|%8.2f|%-8.1f|%+.0f", 2.0 / 3.0, 3.14159, 2.55, 2.5); check("printf widths", strcmp(fb, "0.667|    3.14|2.5     |+2") == 0);
    snprintf(fb, sizeof(fb), "%e %E", 12345.6789, 0.000123); check("printf %e", strcmp(fb, "1.234568e+04 1.230000E-04") == 0);
    snprintf(fb, sizeof(fb), "%g %g %g %g", 100000.0, 1000000.0, 0.0001, 0.00001); check("printf %g", strcmp(fb, "100000 1e+06 0.0001 1e-05") == 0);
    snprintf(fb, sizeof(fb), "%.2f %.0f %.1f", 0.125, 0.5, 0.25); check("printf round-half-even", strcmp(fb, "0.12 0 0.2") == 0);
    snprintf(fb, sizeof(fb), "%f %f %f", 1.0 / 0.0, -1.0 / 0.0, 0.0 / 0.0); check("printf inf/nan", strcmp(fb, "inf -inf nan") == 0);
    snprintf(fb, sizeof(fb), "%.15g %.17g", 0.1, 0.1); check("printf %.17g exact", strcmp(fb, "0.1 0.10000000000000001") == 0);
    snprintf(fb, sizeof(fb), "%08.3f|%x", -3.14159, 255); check("printf zero pad", strcmp(fb, "-003.142|ff") == 0);
    snprintf(fb, sizeof(fb), "%f", 1e22); check("printf big", strcmp(fb, "10000000000000000000000.000000") == 0);
    snprintf(fb, sizeof(fb), "%.3e", 0.0); check("printf zero %e", strcmp(fb, "0.000e+00") == 0);
    double d1 = 1.5, d2 = 2.25;
    check("double arith", d1 * d2 == 3.375 && d2 / d1 == 1.5 && d1 + d2 == 3.75 && d2 - d1 == 0.75 && d1 < d2 && d2 >= d1);
    check("int<->double", (int)(7.9) == 7 && (int)(-7.9) == -7 && (double)12345 == 12345.0 && (long)1e10 == 10000000000L);
    float f1 = 0.1f, f2 = 0.2f; check("float arith", (f1 + f2) >= 0.3f && (f1 + f2) < 0.30001f && (int)(f1 * 10.0f + 0.5f) == 1);
    check("strtod", strtod("3.5e2", 0) == 350.0 && strtod("-0.125", 0) == -0.125 && atof("2.5") == 2.5 && strtod("1e-3", 0) == 0.001);
    char* endp; double dv = strtod("12.5abc", &endp); check("strtod end", dv == 12.5 && strcmp(endp, "abc") == 0);
    check("strtod hex/inf", strtod("0x1.8p1", 0) == 3.0 && isinf(strtod("-inf", 0)) && isnan(strtod("nan", 0)));
    check("sqrt/pow", sqrt(2.0) * sqrt(2.0) > 1.9999999999 && pow(2.0, 10.0) == 1024.0 && pow(2.0, 0.5) > 1.41421356 && pow(2.0, 0.5) < 1.41421357);
    check("floor/ceil/round", floor(-2.5) == -3.0 && ceil(-2.5) == -2.0 && round(2.5) == 3.0 && trunc(-2.9) == -2.0 && fmod(7.5, 2.0) == 1.5 && fabs(-3.25) == 3.25);
    check("exp/log", exp(0.0) == 1.0 && fabs(exp(1.0) - M_E) < 1e-15 && fabs(log(M_E) - 1.0) < 1e-15 && fabs(log10(1000.0) - 3.0) < 1e-14);
    check("sin/cos/atan", fabs(sin(M_PI / 6) - 0.5) < 1e-15 && fabs(cos(M_PI / 3) - 0.5) < 1e-15 && fabs(atan(1.0) * 4 - M_PI) < 1e-15 && fabs(atan2(1.0, -1.0) - 3 * M_PI / 4) < 1e-15);
    double fa = 0, fbv = 0; int ia = 0; char sw[16];
    check("sscanf float", sscanf("2.5 -1e2 42 word", "%lf %lf %d %s", &fa, &fbv, &ia, sw) == 4 && fa == 2.5 && fbv == -100.0 && ia == 42 && strcmp(sw, "word") == 0);
    char set[16], rest[16];
    check("sscanf %[ ] %n", sscanf("abc123 rest", "%[a-z]%d %n%s", set, &ia, &a, rest) >= 3 && strcmp(set, "abc") == 0 && ia == 123 && strcmp(rest, "rest") == 0);
    check("sscanf width/suppress", sscanf("12345 67", "%2d%*d %d", &a, &ia) == 2 && a == 12 && ia == 67);
    f = fopen("/tmp/libctest.txt", "w"); if (f) { fprintf(f, "7 8.5 name\nsecond line\n"); fclose(f); }
    f = fopen("/tmp/libctest.txt", "r"); int fi = 0; double fd = 0; char nm[16] = "";
    check("fscanf", f && fscanf(f, "%d %lf %s", &fi, &fd, nm) == 3 && fi == 7 && fd == 8.5 && strcmp(nm, "name") == 0);
    char* line = 0; size_t cap = 0; ssize_t ln = f ? getline(&line, &cap, f) : -1; ln = f ? getline(&line, &cap, f) : -1;
    check("getline", ln == 12 && line && strcmp(line, "second line\n") == 0); free(line); if (f) fclose(f);
    check("setlocale", strcmp(setlocale(LC_ALL, 0), "C") == 0 && setlocale(LC_ALL, "") != 0 && setlocale(LC_ALL, "xx_YY") == 0 && localeconv()->decimal_point[0] == '.');
    check("setenv/getenv", setenv("RIO_T", "val", 1) == 0 && strcmp(getenv("RIO_T"), "val") == 0 && setenv("RIO_T", "new", 0) == 0 && strcmp(getenv("RIO_T"), "val") == 0 && unsetenv("RIO_T") == 0 && getenv("RIO_T") == 0);
    time_t tt = 86400L * 365 + 3661; struct tm* g = gmtime(&tt);
    check("gmtime", g->tm_year == 71 && g->tm_mon == 0 && g->tm_mday == 1 && g->tm_hour == 1 && g->tm_min == 1 && g->tm_sec == 1 && g->tm_wday == 5);
    struct tm mt = { 0, 0, 0, 29, 1, 124, 0, 0, 0 };    /* 2024-02-29 */
    time_t mk = mktime(&mt); check("mktime", mk == 1709164800L && mt.tm_wday == 4);
    char tb[64]; strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S %a %b %j", gmtime(&mk)); check("strftime", strcmp(tb, "2024-02-29 00:00:00 Thu Feb 060") == 0);
    check("time()", time(0) > 1700000000L);
    void* m1 = malloc(100); memset(m1, 0xAB, 100); void* m2 = realloc(m1, 5000); check("realloc grow keeps data", m2 && ((unsigned char*)m2)[99] == 0xAB);
    m2 = realloc(m2, 50); check("realloc shrink", m2 && ((unsigned char*)m2)[49] == 0xAB); free(m2);
    void* al = aligned_alloc(256, 1000); check("aligned_alloc", al && ((unsigned long)al % 256) == 0); if (al) { memset(al, 1, 1000); free(al); }
    char* big = (char*)malloc(300000); check("malloc 300 KB (mmap)", big != 0); if (big) { big[299999] = 1; free(big); }
    div_t dd = div(-7, 2); check("div/isgraph/strlcpy", dd.quot == -3 && dd.rem == -1 && isgraph('a') && !isgraph(' ') && strlcpy(tb, "hey", 2) == 3 && strcmp(tb, "h") == 0);
    printf("libctest: %d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
