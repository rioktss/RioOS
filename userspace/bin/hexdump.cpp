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

#include "../libc/ulibc.h"
/* hexdump <file> : offset, 16 hex bytes, ASCII (like hexdump -C) */
int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: hexdump <file>\n"); return 1; }
    FILE* f = fopen(argv[1], "r");
    if (!f) { printf("hexdump: cannot open %s\n", argv[1]); return 1; }
    unsigned char row[16]; unsigned long off = 0; size_t n;
    while ((n = fread(row, 1, 16, f)) > 0)
    {
        printf("%08lx ", off);
        for (size_t i = 0; i < 16; ++i) { if (i < n) printf(" %02x", row[i]); else printf("   "); }
        printf("  |");
        for (size_t i = 0; i < n; ++i) putchar(isprint(row[i]) ? row[i] : '.');
        printf("|\n");
        off += n;
    }
    fclose(f);
    printf("%08lx\n", off);
    return 0;
}
