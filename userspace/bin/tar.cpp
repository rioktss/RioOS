#include "tarlib.h"
/* tar -c|-x|-t [-v] [-z] [-C dir] -f archive [paths...]   (ustar; -z = gzip; -f - = stdin/stdout) */
int main(int argc, char** argv)
{
    int mode = 0, verbose = 0, gz = 0; const char* arch = 0; const char* dir = 0; const char* paths[28]; int np = 0;
    for (int i = 1; i < argc; ++i)
    {
        const char* a = argv[i];
        if (a[0] == '-' || (i == 1 && a[0] != '/' && a[0] != '.' && (strchr(a, 'f') || strchr(a, 'x') || strchr(a, 'c') || strchr(a, 't'))))
        {
            if (!strcmp(a, "-C") && i + 1 < argc) { dir = argv[++i]; continue; }
            if (!strcmp(a, "-f") && i + 1 < argc) { arch = argv[++i]; continue; }
            for (const char* c = a + (a[0] == '-'); *c; ++c)
            {
                if (*c == 'c' || *c == 'x' || *c == 't') mode = *c; else if (*c == 'v') verbose = 1; else if (*c == 'z') gz = 1;
                else if (*c == 'f') { if (i + 1 < argc) arch = argv[++i]; }
                else if (*c == 'C') { if (i + 1 < argc) dir = argv[++i]; }
                else { printf("tar: unknown option -%c\n", *c); return 2; }
            }
        }
        else if (np < 28) paths[np++] = a;
    }
    if (!mode || !arch) { printf("usage: tar -c|-x|-t [-v] [-z] [-C dir] -f archive [files...]\n       tar czvf out.tar.gz dir | tar xzf in.tar.gz -C /tmp | tar tf a.tar\n"); return 2; }
    if (!gz) { size_t l = strlen(arch); if (l > 3 && (!strcmp(arch + l - 3, ".gz") || !strcmp(arch + l - 4 > arch ? arch + l - 4 : arch, ".tgz"))) gz = 1; }
    int errors = 0;
    if (mode == 'c')
    {
        if (!np) { printf("tar: no files given\n"); return 2; }
        TarBuf b = { 0, 0, 0 };
        if (dir) { printf("tar: -C is only supported when extracting\n"); return 2; }
        for (int i = 0; i < np; ++i) if (tar_add_path(&b, paths[i], verbose, &errors)) { printf("tar: out of memory\n"); return 2; }
        tar_finish(&b);
        unsigned char* out = b.p; unsigned long on = b.len;
        if (gz) { unsigned char* c; unsigned long cn; if (rio_gzip_compress(b.p, b.len, 0, (uint32_t)time_epoch(), &c, &cn, 6)) { printf("tar: compress failed\n"); return 2; } free(b.p); out = c; on = cn; }
        FILE* f = strcmp(arch, "-") ? fopen(arch, "w") : stdout;
        if (!f) { printf("tar: cannot create %s\n", arch); return 2; }
        if (fwrite(out, 1, on, f) != on) { printf("tar: write error\n"); return 2; }
        if (f != stdout) fclose(f); else fflush(stdout);
        return errors ? 1 : 0;
    }
    unsigned long an; unsigned char* a = tar_slurp(arch, &an);
    if (!a) { printf("tar: cannot read %s\n", arch); return 2; }
    if (an >= 2 && a[0] == 0x1f && a[1] == 0x8b)
    {
        unsigned char* o; unsigned long on; int r = rio_gzip_decompress(a, an, &o, &on, 0, 0);
        if (r) { printf("tar: %s: bad gzip data\n", arch); return 2; }
        free(a); a = o; an = on;
    }
    if (an > 0 && an < 512) { printf("tar: archive is corrupt (bad checksum or truncated)\n"); return 2; }
    unsigned long pos = 0; TarEntry e; int r, count = 0;
    char cwd_prefix[200]; cwd_prefix[0] = 0;
    if (mode == 'x' && dir)
    {
        struct RioStat st; if (stat_path(dir, &st) != 0 && mkdir(dir, 0755) != 0) { printf("tar: cannot create directory %s\n", dir); return 2; }
        snprintf(cwd_prefix, sizeof(cwd_prefix), "%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/");
    }
    while ((r = tar_next(a, an, &pos, &e)) == 1)
    {
        if (np) { int hit = 0; for (int i = 0; i < np; ++i) { size_t pl = strlen(paths[i]); if (!strncmp(e.name, paths[i], pl) && (e.name[pl] == 0 || e.name[pl] == '/' || paths[i][pl - 1] == '/')) hit = 1; } if (!hit) continue; }
        ++count;
        if (mode == 't')
        {
            if (verbose) printf("%c%04o %5u/%-5u %8lu %s\n", e.type == '5' ? 'd' : '-', e.mode & 07777, e.uid, e.gid, e.size, e.name); else printf("%s\n", e.name);
            continue;
        }
        const char* safe = tar_safe_path(e.name);
        if (!safe) { printf("tar: skipping unsafe path %s\n", e.name); ++errors; continue; }
        char target[300]; snprintf(target, sizeof(target), "%s%s", cwd_prefix[0] ? cwd_prefix : (e.name[0] == '/' ? "/" : ""), safe);
        size_t tl = strlen(target); while (tl > 1 && target[tl - 1] == '/') target[--tl] = 0;
        if (verbose) printf("%s\n", e.name);
        if (tar_mkdirs(target)) { printf("tar: %s: cannot create parent directories\n", target); ++errors; continue; }
        if (e.type == '5') { struct RioStat st; if (stat_path(target, &st) != 0 && mkdir(target, 0755) != 0) { printf("tar: %s: cannot create directory\n", target); ++errors; } else chmod(target, e.mode & 0777); continue; }
        if (e.type != '0' && e.type != '7') { printf("tar: %s: unsupported entry type '%c' skipped\n", e.name, e.type); continue; }
        FILE* f = fopen(target, "w");
        if (!f) { printf("tar: %s: cannot create file\n", target); ++errors; continue; }
        if (e.size && fwrite(e.data, 1, e.size, f) != e.size) { printf("tar: %s: write error\n", target); ++errors; }
        fclose(f); chmod(target, e.mode & 0777);
    }
    if (r < 0) { printf("tar: archive is corrupt (bad checksum or truncated)\n"); return 2; }
    free(a);
    if (np && !count) { printf("tar: none of the requested files are in the archive\n"); return 1; }
    return errors ? 1 : 0;
}
