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

/* shared command-line driver for gzip / gunzip / zcat */
#include "../libc/ulibc.h"
static unsigned char* gz_slurp(const char* path, unsigned long* n)
{
    FILE* f = fopen(path, "r"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* b = (unsigned char*)malloc((unsigned long)sz + 1); if (!b) { fclose(f); return 0; }
    size_t got = sz ? fread(b, 1, (size_t)sz, f) : 0; fclose(f);
    *n = got; return b;
}
static int gz_spit(const char* path, const void* d, unsigned long n)
{
    FILE* f = fopen(path, "w"); if (!f) return -1;
    if (n && fwrite(d, 1, n, f) != n) { fclose(f); return -1; }
    return fclose(f);
}
/* mode: 0 gzip, 1 gunzip, 2 zcat */
static int gzip_cli(int mode, int argc, char** argv)
{
    int keep = 0, to_stdout = mode == 2, level = 6, force = 0, test = 0, list = 0, verbose = 0, nfiles = 0, rc = 0;
    const char* files[32];
    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        if (a[0] == '-' && a[1] && a[1] != '-')
        {
            for (const char* c = a + 1; *c; ++c)
            {
                if (*c == 'd') mode = 1; else if (*c == 'c') to_stdout = 1; else if (*c == 'k') keep = 1; else if (*c == 'f') force = 1;
                else if (*c == 't') test = 1; else if (*c == 'l') list = 1; else if (*c == 'v') verbose = 1;
                else if (*c >= '1' && *c <= '9') level = *c - '0';
                else { printf("gzip: invalid option -%c\n", *c); return 2; }
            }
        }
        else if (!strcmp(a, "--decompress") || !strcmp(a, "--uncompress")) mode = 1;
        else if (!strcmp(a, "--stdout") || !strcmp(a, "--to-stdout")) to_stdout = 1;
        else if (!strcmp(a, "--keep")) keep = 1;
        else if (!strcmp(a, "--best")) level = 9; else if (!strcmp(a, "--fast")) level = 1;
        else if (nfiles < 32) files[nfiles++] = a;
    }
    if (!nfiles) { printf("usage: gzip [-dckftlv1-9] file...   (gunzip file.gz | zcat file.gz)\n"); return 2; }
    for (int fi = 0; fi < nfiles; ++fi)
    {
        const char* path = files[fi]; unsigned long n = 0;
        unsigned char* data = gz_slurp(path, &n);
        if (!data) { printf("gzip: %s: cannot open\n", path); rc = 1; continue; }
        size_t pl = strlen(path); int has_gz = pl > 3 && !strcmp(path + pl - 3, ".gz");
        if (list || test || mode >= 1)
        {
            unsigned char* out; unsigned long on; char nm[96];
            int r = rio_gzip_decompress(data, n, &out, &on, nm, sizeof(nm));
            if (r != 0) { printf("gzip: %s: %s\n", path, r == -1 ? "not in gzip format" : r == -3 ? "crc error" : "corrupt data"); free(data); rc = 1; continue; }
            if (list) { printf("compressed %lu uncompressed %lu ratio %lu%% %s\n", n, on, on ? (on > n ? (on - n) * 100 / on : 0) : 0, path); free(out); free(data); continue; }
            if (test) { if (verbose) printf("%s: OK\n", path); free(out); free(data); continue; }
            if (to_stdout) { fwrite(out, 1, on, stdout); fflush(stdout); }
            else
            {
                char outname[160];
                if (!has_gz) { printf("gzip: %s: unknown suffix -- ignored\n", path); free(out); free(data); rc = 1; continue; }
                strncpy(outname, path, sizeof(outname) - 1); outname[sizeof(outname) - 1] = 0; outname[pl - 3] = 0;
                struct RioStat st; if (!force && stat_path(outname, &st) == 0) { printf("gzip: %s already exists; use -f to overwrite\n", outname); free(out); free(data); rc = 1; continue; }
                if (gz_spit(outname, out, on) != 0) { printf("gzip: cannot write %s\n", outname); rc = 1; }
                else if (!keep) unlink(path);
                if (verbose) printf("%s -> %s\n", path, outname);
            }
            free(out); free(data);
        }
        else
        {
            if (has_gz) { printf("gzip: %s already has .gz suffix -- unchanged\n", path); free(data); rc = 1; continue; }
            unsigned char* out; unsigned long on;
            const char* base = path; for (const char* q = path; *q; ++q) if (*q == '/') base = q + 1;
            if (rio_gzip_compress(data, n, base, (uint32_t)time_epoch(), &out, &on, level) != 0) { printf("gzip: out of memory\n"); free(data); rc = 1; continue; }
            if (to_stdout) { fwrite(out, 1, on, stdout); fflush(stdout); }
            else
            {
                char outname[160]; snprintf(outname, sizeof(outname), "%s.gz", path);
                struct RioStat st; if (!force && stat_path(outname, &st) == 0) { printf("gzip: %s already exists; use -f to overwrite\n", outname); free(out); free(data); rc = 1; continue; }
                if (gz_spit(outname, out, on) != 0) { printf("gzip: cannot write %s\n", outname); rc = 1; }
                else if (!keep) unlink(path);
                if (verbose) printf("%s: %lu -> %lu bytes (%lu%% saved)\n", path, n, on, n ? (n > on ? (n - on) * 100 / n : 0) : 0);
            }
            free(out); free(data);
        }
    }
    return rc;
}
