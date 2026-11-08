/*
 * MIT License
 *
 * Copyright (c) 2026 Rio okta kusuma
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files, to deal in the Software
 * without restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of the
 * Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#define N 10
#define SQ(x) ((x)*(x))
enum { RED, GREEN = 5, BLUE };
typedef unsigned char byte;
int g = 7;
int arr[5] = {1, 2, 3, 4, 5};
char msg[] = "hello";
char *names[] = {"ab", "cd", "ef"};
static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
int gcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }
void swap(int *a, int *b) { int t = *a; *a = *b; *b = t; }
int sum(int *p, int n) { int s = 0; for (int i = 0; i < n; i++) s += p[i]; return s; }
long fact(int n);
long fact(int n) { if (n <= 1) return 1; return n * fact(n - 1); }
int main(int argc, char **argv)
{
    int i, x = 3, y = 4;
    printf("fib(20)=%d gcd=%d fact(15)=%ld\n", fib(20), gcd(84, 36), fact(15));
    swap(&x, &y); printf("x=%d y=%d g=%d\n", x, y, g);
    printf("sum=%d len=%d %s %c\n", sum(arr, 5), (int)strlen(msg), msg, msg[1]);
    for (i = 0; i < 3; i++) printf("%s ", names[i]); printf("\n");
    printf("enum %d %d %d N=%d SQ=%d\n", RED, GREEN, BLUE, N, SQ(N+1));
    int *p = malloc(10 * sizeof(int)); for (i = 0; i < 10; i++) p[i] = i * i;
    printf("p[9]=%d *(p+3)=%d diff=%d\n", p[9], *(p + 3), (int)((p + 7) - p)); free(p);
    char buf[32]; sprintf(buf, "%05d|%-5s|%x|%5.2s|", 42, "ab", 255, "xyz"); puts(buf);
    switch (y) { case 1: puts("one"); break; case 3: puts("three"); case 4: puts("four(fall)"); break; default: puts("def"); }
    byte b = 200; b += 100; printf("b=%d sizeof(int)=%d sizeof(arr)=%d sizeof(long)=%d\n", b, (int)sizeof(int), (int)sizeof(arr), (int)sizeof(long));
    i = 0; do { i += 2; if (i == 4) continue; if (i > 8) break; } while (1); printf("i=%d\n", i);
    int k = 5; k <<= 2; k |= 1; k ^= 8; printf("k=%d %d %d\n", k, k++ + ++k, -k / 3);
    printf("%d %d %d\n", 7 / 2, -7 / 2, -7 % 3);
    printf("args=%d\n", argc); for (i = 1; i < argc; i++) printf(" [%s]", argv[i]); printf("\n");
    unsigned u = 4000000000u; printf("%u %d\n", u, (int)u);
    return 3;
}
