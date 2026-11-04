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
    printf("libctest: %d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
