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
