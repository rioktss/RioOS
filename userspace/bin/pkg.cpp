#include "tarlib.h"
/* pkg: package manager for .rpk packages (ustar tar, optionally gzip, with a PKGINFO entry).
   pkg list | info <name|file.rpk> | files <name> | install [-f] <file.rpk|name>... | remove [-f] <name>... | upgrade
       search <text> | verify [name] | owner <path> | build [-z] <srcdir> <out.rpk> | update | help
   Database: /var/lib/pkg/<name>.info   Local repository: /var/cache/pkg (.rpk files)   Index: /var/lib/pkg/INDEX */
#define DB "/var/lib/pkg"
#define REPO "/var/cache/pkg"
struct Info { char name[64], version[32], desc[160], depends[200], conflicts[200], provides[100]; };
static int force_flag = 0;

static void mkdirs_abs(const char* dir) { char t[256]; snprintf(t, sizeof(t), "%s/x", dir); tar_mkdirs(t); }
static int valid_name(const char* n) { if (!*n || strlen(n) > 60) return 0; for (; *n; ++n) if (!(islower((unsigned char)*n) || isdigit((unsigned char)*n) || *n == '-' || *n == '_' || *n == '.' || *n == '+')) return 0; return 1; }
static void trim_line(char* s) { size_t l = strlen(s); while (l && (s[l - 1] == '\r' || s[l - 1] == ' ')) s[--l] = 0; }
static void parse_info(const char* text, Info* in)
{
    memset(in, 0, sizeof(*in));
    for (const char* p = text; *p;)
    {
        const char* e = p; while (*e && *e != '\n') ++e;
        char ln[300]; size_t n = (size_t)(e - p); if (n > 299) n = 299; memcpy(ln, p, n); ln[n] = 0; trim_line(ln);
        p = *e ? e + 1 : e;
        char* eq = strchr(ln, '='); if (!eq || ln[0] == '#') continue; *eq++ = 0;
        if (!strcmp(ln, "name")) strncpy(in->name, eq, sizeof(in->name) - 1);
        else if (!strcmp(ln, "version")) strncpy(in->version, eq, sizeof(in->version) - 1);
        else if (!strcmp(ln, "desc")) strncpy(in->desc, eq, sizeof(in->desc) - 1);
        else if (!strcmp(ln, "depends")) strncpy(in->depends, eq, sizeof(in->depends) - 1);
        else if (!strcmp(ln, "conflicts")) strncpy(in->conflicts, eq, sizeof(in->conflicts) - 1);
        else if (!strcmp(ln, "provides")) strncpy(in->provides, eq, sizeof(in->provides) - 1);
    }
}
static int vercmp(const char* a, const char* b)
{
    while (*a || *b)
    {
        long x = 0, y = 0; while (*a >= '0' && *a <= '9') x = x * 10 + *a++ - '0'; while (*b >= '0' && *b <= '9') y = y * 10 + *b++ - '0';
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.' || *a == '-') ++a; if (*b == '.' || *b == '-') ++b;
        if (*a && !(*a >= '0' && *a <= '9') && *a != '.') { if (*b == *a) { ++a; ++b; continue; } return *a < *b ? -1 : 1; }
    }
    return 0;
}
/* read a whole package (gunzipping when needed) */
static unsigned char* open_pkg(const char* path, unsigned long* n)
{
    unsigned long an; unsigned char* a = tar_slurp(path, &an); if (!a) return 0;
    if (an >= 2 && a[0] == 0x1f && a[1] == 0x8b) { unsigned char* o; unsigned long on; if (rio_gzip_decompress(a, an, &o, &on, 0, 0)) { free(a); return 0; } free(a); a = o; an = on; }
    *n = an; return a;
}
static int pkg_info_from(const unsigned char* a, unsigned long an, Info* in, char* text, size_t cap)
{
    unsigned long pos = 0; TarEntry e; int r;
    while ((r = tar_next(a, an, &pos, &e)) == 1)
        if (!strcmp(tar_safe_path(e.name) ? tar_safe_path(e.name) : "", "PKGINFO") && e.type == '0')
        { size_t n = e.size < cap - 1 ? e.size : cap - 1; memcpy(text, e.data, n); text[n] = 0; parse_info(text, in); return 0; }
    return -1;
}
static int db_path(const char* name, char* out, size_t cap) { return snprintf(out, cap, DB "/%s.info", name); }
static char* db_read(const char* name)
{
    char p[160]; db_path(name, p, sizeof(p)); FILE* f = fopen(p, "r"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET); char* b = (char*)malloc((unsigned long)sz + 1); size_t g = sz ? fread(b, 1, (size_t)sz, f) : 0; fclose(f); b[g] = 0; return b;
}
static int installed(const char* name, Info* in)
{
    char* t = db_read(name); if (!t) return 0; if (in) parse_info(t, in); free(t); return 1;
}
static char* list_names(void)   /* newline separated installed package names */
{
    char* b = (char*)malloc(65536); long n = readdir_text(DB, b, 65535); if (n < 0) { b[0] = 0; return b; } b[n] = 0;
    char* out = (char*)malloc(65536); out[0] = 0; size_t o = 0;
    for (char* ln = b; *ln;)
    {
        char* nl = ln; while (*nl && *nl != '\n') ++nl; if (*nl) *nl++ = 0;
        char* nm = ln + 1; while (*nm == ' ') ++nm; while (*nm >= '0' && *nm <= '9') ++nm; while (*nm == ' ') ++nm;
        size_t l = strlen(nm); ln = nl;
        if (l > 5 && !strcmp(nm + l - 5, ".info")) { memcpy(out + o, nm, l - 5); o += l - 5; out[o++] = '\n'; out[o] = 0; }
    }
    free(b); return out;
}
/* iterate comma separated list "a,b>=1.0" */
struct Dep { char name[64]; char op[3]; char ver[32]; };
static int next_dep(const char** pp, Dep* d)
{
    const char* p = *pp; while (*p == ',' || *p == ' ') ++p; if (!*p) return 0;
    char t[128]; int n = 0; while (*p && *p != ',' && n < 127) t[n++] = *p++; t[n] = 0; *pp = p;
    memset(d, 0, sizeof(*d)); char* o = t; while (*o && *o != '>' && *o != '<' && *o != '=') ++o;
    if (*o) { int k = 0; while ((*o == '>' || *o == '<' || *o == '=') && k < 2) d->op[k++] = *o++; strncpy(d->ver, o, sizeof(d->ver) - 1); }
    size_t nl = (size_t)(strchr(t, *t) ? 0 : 0); (void)nl;
    int l = 0; while (t[l] && t[l] != '>' && t[l] != '<' && t[l] != '=') ++l; while (l && t[l - 1] == ' ') --l;
    memcpy(d->name, t, (size_t)l < 63 ? (size_t)l : 63); return 1;
}
static int dep_ok(const Dep* d, const char* have)
{
    if (!d->op[0]) return 1; int c = vercmp(have, d->ver);
    if (!strcmp(d->op, ">=")) return c >= 0; if (!strcmp(d->op, ">")) return c > 0; if (!strcmp(d->op, "<=")) return c <= 0; if (!strcmp(d->op, "<")) return c < 0; return c == 0;
}
/* is `name` satisfied by an installed package (direct or via provides)? */
static int satisfied(const Dep* d, char* by, size_t cap)
{
    Info in; if (installed(d->name, &in)) { if (dep_ok(d, in.version)) { snprintf(by, cap, "%s", d->name); return 1; } return 0; }
    char* names = list_names(); int ok = 0;
    for (char* ln = names; *ln && !ok;)
    {
        char* nl = ln; while (*nl && *nl != '\n') ++nl; if (*nl) *nl++ = 0;
        Info o; if (installed(ln, &o)) { const char* p = o.provides; Dep pd; while (next_dep(&p, &pd)) if (!strcmp(pd.name, d->name)) { ok = 1; snprintf(by, cap, "%s", ln); break; } }
        ln = nl;
    }
    free(names); return ok;
}
/* repo lookup: returns path of best matching file for name (or exact file) */
static int repo_find(const char* name, char* out, size_t cap)
{
    char* b = (char*)malloc(65536); long n = readdir_text(REPO, b, 65535); if (n < 0) { free(b); return 0; } b[n] = 0;
    int found = 0; char best[100]; best[0] = 0; size_t nl = strlen(name);
    for (char* ln = b; *ln;)
    {
        char* e = ln; while (*e && *e != '\n') ++e; if (*e) *e++ = 0;
        char* nm = ln + 1; while (*nm == ' ') ++nm; while (*nm >= '0' && *nm <= '9') ++nm; while (*nm == ' ') ++nm; ln = e;
        size_t l = strlen(nm); if (l < 5 || strcmp(nm + l - 4, ".rpk")) continue;
        if (!strncmp(nm, name, nl) && (nm[nl] == '-' || nm[nl] == '.') && (nm[nl] == '.' ? !strcmp(nm + nl, ".rpk") : isdigit((unsigned char)nm[nl + 1])))
        { if (!found || vercmp(nm + nl + 1, best + nl + 1) > 0) { strncpy(best, nm, sizeof(best) - 1); best[sizeof(best) - 1] = 0; } found = 1; }
    }
    free(b); if (found) snprintf(out, cap, REPO "/%s", best); return found;
}
static int owner_of(const char* path, char* owner, size_t cap)
{
    char* names = list_names(); int hit = 0;
    for (char* ln = names; *ln && !hit;)
    {
        char* nl = ln; while (*nl && *nl != '\n') ++nl; if (*nl) *nl++ = 0;
        char* t = db_read(ln);
        if (t) { char key[300]; snprintf(key, sizeof(key), "\nfile=%s ", path); char* h = (char*)t; char* f = strstr(h, key); if (f) { hit = 1; snprintf(owner, cap, "%s", ln); } free(t); }
        ln = nl;
    }
    free(names); return hit;
}
static int do_remove(const char* name, int quiet, int check_rdeps);
static int do_install(const char* arg, int depth, int is_dep);

static int install_unit2(const unsigned char* a, unsigned long an, const char* text, const Info* in, int is_dep, char** keep);
static int install_unit(const unsigned char* a, unsigned long an, const char* text, const Info* in, int is_dep)
{ char* keep = 0; int rc = install_unit2(a, an, text, in, is_dep, &keep); free(keep); return rc; }
static int install_unit2(const unsigned char* a, unsigned long an, const char* text, const Info* in, int is_dep, char** keep)
{
    /* conflicts */
    char by[64]; const char* p = in->conflicts; Dep d;
    while (next_dep(&p, &d)) { Info o; if (installed(d.name, &o) && dep_ok(&d, o.version) && strcmp(d.name, in->name)) { printf("pkg: %s conflicts with installed package %s\n", in->name, d.name); if (!force_flag) return 1; } }
    char* names = list_names();
    for (char* ln = names; *ln;)
    {
        char* nl = ln; while (*nl && *nl != '\n') ++nl; if (*nl) *nl++ = 0;
        Info o; if (strcmp(ln, in->name) && installed(ln, &o)) { const char* q = o.conflicts; Dep c; while (next_dep(&q, &c)) if (!strcmp(c.name, in->name)) { printf("pkg: installed package %s conflicts with %s\n", ln, in->name); if (!force_flag) { free(names); return 1; } } }
        ln = nl;
    }
    free(names);
    /* file ownership check */
    unsigned long pos = 0; TarEntry e; int r;
    while ((r = tar_next(a, an, &pos, &e)) == 1)
    {
        const char* sp = tar_safe_path(e.name); if (!sp) { if (strncmp(e.name, "./", 3) && strcmp(e.name, ".")) { printf("pkg: unsafe path in package: %s\n", e.name); return 1; } continue; }
        if (e.type != '0' || !strcmp(sp, "PKGINFO")) continue;
        char abs[300]; snprintf(abs, sizeof(abs), "/%s", sp); char ow[64];
        if (owner_of(abs, ow, sizeof(ow)) && strcmp(ow, in->name)) { printf("pkg: %s is already owned by %s\n", abs, ow); if (!force_flag) return 1; }
    }
    if (r < 0) { printf("pkg: package is corrupt\n"); return 1; }
    /* upgrade: remember old file list */
    char* old = db_read(in->name);
    /* extract */
    const size_t DBCAP = 262144; char* dbtext = (char*)malloc(DBCAP); *keep = dbtext; if (!dbtext) { printf("pkg: out of memory\n"); return 1; }
    size_t dl = (size_t)snprintf(dbtext, DBCAP, "%s", text);
    if (dl && dbtext[dl - 1] != '\n') dbtext[dl++] = '\n';
    pos = 0; int files = 0; long bytes = 0;
    while ((r = tar_next(a, an, &pos, &e)) == 1)
    {
        const char* sp = tar_safe_path(e.name); if (!sp || !strcmp(sp, "PKGINFO")) continue;
        char abs[300]; snprintf(abs, sizeof(abs), "/%s", sp); size_t al = strlen(abs); while (al > 1 && abs[al - 1] == '/') abs[--al] = 0;
        /* record directories created by us */
        { char t[300]; strncpy(t, abs, sizeof(t) - 1); t[sizeof(t) - 1] = 0; int made = 0; char dirs[600]; dirs[0] = 0;
          for (char* q = t + 1; *q; ++q) if (*q == '/') { *q = 0; struct RioStat st; if (stat_path(t, &st) != 0) { if (mkdir(t, 0755)) { printf("pkg: cannot create %s\n", t); if (old) free(old); return 1; } size_t cl = strlen(dirs); snprintf(dirs + cl, sizeof(dirs) - cl, "dir=%s\n", t); ++made; } *q = '/'; }
          if (made && dl + strlen(dirs) < DBCAP - 1) { strcpy(dbtext + dl, dirs); dl += strlen(dirs); } }
        if (e.type == '5')
        { struct RioStat st; if (stat_path(abs, &st) != 0) { if (mkdir(abs, 0755)) { printf("pkg: cannot create %s\n", abs); if (old) free(old); return 1; } if (dl + strlen(abs) + 8 < DBCAP) dl += (size_t)snprintf(dbtext + dl, DBCAP - dl, "dir=%s\n", abs); } continue; }
        if (e.type != '0') { printf("pkg: %s: unsupported entry type, skipped\n", e.name); continue; }
        FILE* f = fopen(abs, "w"); if (!f) { printf("pkg: cannot write %s\n", abs); if (old) free(old); return 1; }
        if (e.size && fwrite(e.data, 1, e.size, f) != e.size) { fclose(f); printf("pkg: write error on %s\n", abs); if (old) free(old); return 1; }
        fclose(f); chmod(abs, e.mode & 0777);
        if (dl + strlen(abs) + 40 < DBCAP) dl += (size_t)snprintf(dbtext + dl, DBCAP - dl, "file=%s %lu %08x\n", abs, e.size, (unsigned)rio_crc32(0, e.data, e.size));
        else { printf("pkg: too many files for the database entry\n"); if (old) free(old); return 1; }
        ++files; bytes += (long)e.size;
    }
    /* drop files of the previous version that are no longer shipped */
    if (old)
    {
        for (const char* q = old; (q = strstr(q, "\nfile=")); ) { q += 6; const char* sp = strchr(q, ' '); if (!sp) break; char path[300]; size_t l = (size_t)(sp - q); if (l > 299) l = 299; memcpy(path, q, l); path[l] = 0; char key[310]; snprintf(key, sizeof(key), "\nfile=%s ", path); if (!strstr(dbtext, key)) unlink(path); }
        for (const char* q = old; (q = strstr(q, "\ndir=")); ) { q += 5; const char* nl = strchr(q, '\n'); if (!nl) break; char path[300]; size_t l = (size_t)(nl - q); if (l > 299) l = 299; memcpy(path, q, l); path[l] = 0; char key[310]; snprintf(key, sizeof(key), "\ndir=%s\n", path); if (!strstr(dbtext, key) && dl + l + 8 < DBCAP) dl += (size_t)snprintf(dbtext + dl, DBCAP - dl, "dir=%s\n", path); }
        free(old);
    }
    mkdirs_abs(DB);
    char dp[160]; db_path(in->name, dp, sizeof(dp)); FILE* f = fopen(dp, "w"); if (!f) { printf("pkg: cannot write database entry\n"); return 1; }
    /* the db text must start with a newline-separated info block; add a leading newline so "\nfile=" keys match the first record */
    fwrite("\n", 1, 1, f); fwrite(dbtext, 1, dl, f); fclose(f);
    (void)by; (void)is_dep;
    printf("installed %s %s (%d files, %ld bytes)\n", in->name, in->version, files, bytes);
    return 0;
}
static int do_install(const char* arg, int depth, int is_dep)
{
    if (depth > 8) { printf("pkg: dependency chain too deep\n"); return 1; }
    char path[256]; int via_repo = 0;
    if (strchr(arg, '/') || (strlen(arg) > 4 && !strcmp(arg + strlen(arg) - 4, ".rpk"))) snprintf(path, sizeof(path), "%s", arg);
    else if (repo_find(arg, path, sizeof(path))) via_repo = 1;
    else { printf("pkg: package '%s' not found in " REPO "\n", arg); return 1; }
    unsigned long an; unsigned char* a = open_pkg(path, &an); if (!a) { printf("pkg: cannot read package %s\n", path); return 1; }
    char text[4096]; Info in;
    if (pkg_info_from(a, an, &in, text, sizeof(text)) || !valid_name(in.name) || !in.version[0]) { printf("pkg: %s: missing or invalid PKGINFO (need name and version)\n", path); free(a); return 1; }
    Info cur;
    if (installed(in.name, &cur))
    {
        int c = vercmp(in.version, cur.version);
        if (c == 0 && !force_flag) { printf("%s %s is already installed\n", in.name, cur.version); free(a); return 0; }
        if (c < 0 && !force_flag) { printf("pkg: %s %s is older than installed %s (use -f to downgrade)\n", in.name, in.version, cur.version); free(a); return 1; }
    }
    /* dependencies */
    const char* p = in.depends; Dep d;
    while (next_dep(&p, &d))
    {
        char by[64]; if (satisfied(&d, by, sizeof(by))) continue;
        if (!valid_name(d.name)) { printf("pkg: bad dependency '%s'\n", d.name); free(a); return 1; }
        char rp[256]; if (repo_find(d.name, rp, sizeof(rp)))
        { printf("installing dependency %s for %s\n", d.name, in.name); if (do_install(d.name, depth + 1, 1)) { free(a); return 1; } if (satisfied(&d, by, sizeof(by))) continue; }
        printf("pkg: %s needs %s%s%s which is not installed and not in the repository\n", in.name, d.name, d.op[0] ? d.op : "", d.ver); free(a); return 1;
    }
    (void)via_repo;
    int rc = install_unit(a, an, text, &in, is_dep); free(a); return rc;
}
static int do_remove(const char* name, int quiet, int check_rdeps)
{
    char* t = db_read(name); if (!t) { printf("pkg: %s is not installed\n", name); return 1; }
    if (check_rdeps && !force_flag)
    {
        char* names = list_names(); int bad = 0;
        for (char* ln = names; *ln;)
        {
            char* nl = ln; while (*nl && *nl != '\n') ++nl; if (*nl) *nl++ = 0;
            Info o; if (strcmp(ln, name) && installed(ln, &o)) { const char* p = o.depends; Dep d; while (next_dep(&p, &d)) if (!strcmp(d.name, name)) { printf("pkg: %s is required by %s (use -f to force)\n", name, ln); bad = 1; } }
            ln = nl;
        }
        free(names); if (bad) { free(t); return 1; }
    }
    int n = 0;
    for (const char* q = t; (q = strstr(q, "\nfile=")); ) { q += 6; const char* sp = strchr(q, ' '); if (!sp) break; char path[300]; size_t l = (size_t)(sp - q); if (l > 299) l = 299; memcpy(path, q, l); path[l] = 0; if (unlink(path) == 0) ++n; }
    /* directories: remove deepest first (list order is parent-first, so collect then reverse) */
    char* dirs[256]; int nd = 0;
    for (const char* q = t; (q = strstr(q, "\ndir=")) && nd < 256; ) { q += 5; const char* nl = strchr(q, '\n'); if (!nl) break; dirs[nd] = (char*)malloc((size_t)(nl - q) + 1); memcpy(dirs[nd], q, (size_t)(nl - q)); dirs[nd][nl - q] = 0; ++nd; }
    for (int i = nd - 1; i >= 0; --i) { unlink(dirs[i]); free(dirs[i]); }
    char dp[160]; db_path(name, dp, sizeof(dp)); unlink(dp); free(t);
    if (!quiet) printf("removed %s (%d files)\n", name, n);
    return 0;
}
static void show_info(const Info* in, const char* extra)
{
    printf("Name       : %s\nVersion    : %s\nDescription: %s\nDepends    : %s\nConflicts  : %s\nProvides   : %s\n", in->name, in->version, in->desc, in->depends[0] ? in->depends : "-", in->conflicts[0] ? in->conflicts : "-", in->provides[0] ? in->provides : "-");
    if (extra) printf("%s", extra);
}
static int cmp_names(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }
static int add_tree(TarBuf* b, const char* base, const char* rel, int* errors, int* count)
{
    char full[300]; if (rel[0]) snprintf(full, sizeof(full), "%s/%s", base, rel); else snprintf(full, sizeof(full), "%s", base);
    char* list = (char*)malloc(65536); long n = readdir_text(full, list, 65535); if (n < 0) { free(list); printf("pkg: cannot read %s\n", full); ++*errors; return 0; } list[n] = 0;
    for (char* ln = list; *ln;)
    {
        char* nl = ln; while (*nl && *nl != '\n') ++nl; if (*nl) *nl++ = 0;
        char kind = ln[0]; char* nm = ln + 1; while (*nm == ' ') ++nm; while (*nm >= '0' && *nm <= '9') ++nm; while (*nm == ' ') ++nm; ln = nl;
        if (!*nm) continue;
        char r2[300]; if (rel[0]) snprintf(r2, sizeof(r2), "%s/%s", rel, nm); else snprintf(r2, sizeof(r2), "%s", nm);
        if (!rel[0] && !strcmp(nm, "PKGINFO")) continue;
        char f2[600]; snprintf(f2, sizeof(f2), "%s/%s", base, r2);
        struct RioStat st; if (stat_path(f2, &st) != 0) { ++*errors; continue; }
        if (kind == 'l') { printf("pkg: skipping symlink %s\n", r2); continue; }
        if (kind == 'd') { if (tar_add(b, r2, '5', st.mode, (unsigned long)st.mtime, 0, 0, 0, 0)) { ++*errors; continue; } add_tree(b, base, r2, errors, count); continue; }
        FILE* f = fopen(f2, "r"); if (!f) { ++*errors; continue; }
        unsigned char* d = (unsigned char*)malloc((unsigned long)st.size + 1); size_t g = st.size ? fread(d, 1, (size_t)st.size, f) : 0; fclose(f);
        if (tar_add(b, r2, '0', st.mode, (unsigned long)st.mtime, 0, 0, d, g)) { printf("pkg: name too long: %s\n", r2); ++*errors; }
        else ++*count;
        free(d);
    }
    free(list); return 0;
}
static void usage()
{
    printf("usage: pkg <command>\n"
           "  list                      installed packages\n  info <name|file.rpk>      package details\n  files <name>              files owned by a package\n"
           "  install [-f] <file|name>  install (name = from " REPO "); dependencies are installed automatically\n  remove [-f] <name>        uninstall (refuses when other packages need it)\n"
           "  upgrade                   upgrade installed packages to newer versions found in " REPO "\n  search <text>             search installed and repository packages\n"
           "  verify [name]             check installed files against recorded size/CRC\n  owner <path>              which package owns a file\n"
           "  build [-z] <dir> <out>    build a package from <dir> (must hold PKGINFO)\n  update                    rebuild the repository index (" DB "/INDEX)\n"
           "PKGINFO: name=  version=  desc=  depends=a,b>=1.0  conflicts=  provides=\n");
}
int main(int argc, char** argv)
{
    if (argc < 2 || !strcmp(argv[1], "help") || !strcmp(argv[1], "-h")) { usage(); return argc < 2 ? 2 : 0; }
    const char* cmd = argv[1]; int ai = 2;
    while (ai < argc && (!strcmp(argv[ai], "-f") || !strcmp(argv[ai], "-z"))) { if (argv[ai][1] == 'f') force_flag = 1; ++ai; }
    int zflag = 0; for (int i = 2; i < argc; ++i) if (!strcmp(argv[i], "-z")) zflag = 1;
    mkdirs_abs(DB); mkdirs_abs(REPO);
    if (!strcmp(cmd, "list"))
    {
        char* names = list_names(); int n = 0;
        for (char* ln = names; *ln;) { char* nl = ln; while (*nl && *nl != '\n') ++nl; if (*nl) *nl++ = 0; Info in; if (installed(ln, &in)) { printf("%-20s %-10s %s\n", in.name, in.version, in.desc); ++n; } ln = nl; }
        if (!n) printf("no packages installed\n"); free(names); return 0;
    }
    if (!strcmp(cmd, "info"))
    {
        if (ai >= argc) { usage(); return 2; }
        const char* a = argv[ai]; Info in;
        if (installed(a, &in)) { char* t = db_read(a); int files = 0; for (const char* q = t; (q = strstr(q, "\nfile=")); ++q) ++files; char ex[64]; snprintf(ex, sizeof(ex), "Files      : %d\nStatus     : installed\n", files); show_info(&in, ex); free(t); return 0; }
        char path[256]; if (strchr(a, '/') || strstr(a, ".rpk")) snprintf(path, sizeof(path), "%s", a); else if (!repo_find(a, path, sizeof(path))) { printf("pkg: %s: not installed and not in the repository\n", a); return 1; }
        unsigned long an; unsigned char* b = open_pkg(path, &an); char text[4096]; if (!b || pkg_info_from(b, an, &in, text, sizeof(text))) { printf("pkg: %s: unreadable package\n", path); return 1; }
        show_info(&in, "Status     : not installed\n"); free(b); return 0;
    }
    if (!strcmp(cmd, "files"))
    {
        if (ai >= argc) { usage(); return 2; } char* t = db_read(argv[ai]); if (!t) { printf("pkg: %s is not installed\n", argv[ai]); return 1; }
        for (const char* q = t; (q = strstr(q, "\nfile=")); ) { q += 6; const char* sp = strchr(q, ' '); if (!sp) break; printf("%.*s\n", (int)(sp - q), q); }
        free(t); return 0;
    }
    if (!strcmp(cmd, "install"))
    {
        if (ai >= argc) { usage(); return 2; } int rc = 0; for (int i = ai; i < argc; ++i) if (do_install(argv[i], 0, 0)) rc = 1; return rc;
    }
    if (!strcmp(cmd, "remove"))
    {
        if (ai >= argc) { usage(); return 2; } int rc = 0; for (int i = ai; i < argc; ++i) if (do_remove(argv[i], 0, 1)) rc = 1; return rc;
    }
    if (!strcmp(cmd, "upgrade"))
    {
        char* names = list_names(); int up = 0, rc = 0;
        for (char* ln = names; *ln;)
        {
            char* nl = ln; while (*nl && *nl != '\n') ++nl; if (*nl) *nl++ = 0; Info cur; char rp[256];
            if (installed(ln, &cur) && repo_find(ln, rp, sizeof(rp)))
            { unsigned long an; unsigned char* a = open_pkg(rp, &an); Info n2; char tx[4096]; if (a && !pkg_info_from(a, an, &n2, tx, sizeof(tx)) && vercmp(n2.version, cur.version) > 0) { printf("upgrading %s %s -> %s\n", ln, cur.version, n2.version); if (do_install(rp, 0, 0)) rc = 1; ++up; } free(a); }
            ln = nl;
        }
        free(names); if (!up) printf("everything is up to date\n"); return rc;
    }
    if (!strcmp(cmd, "search"))
    {
        if (ai >= argc) { usage(); return 2; } int hits = 0; const char* term = argv[ai];
        char* names = list_names();
        for (char* ln = names; *ln;) { char* nl = ln; while (*nl && *nl != '\n') ++nl; if (*nl) *nl++ = 0; Info in; if (installed(ln, &in) && (strcasestr(in.name, term) || strcasestr(in.desc, term))) { printf("[installed] %-18s %-8s %s\n", in.name, in.version, in.desc); ++hits; } ln = nl; }
        free(names);
        char* b = (char*)malloc(65536); long n = readdir_text(REPO, b, 65535); if (n < 0) n = 0; b[n] = 0;
        for (char* ln = b; *ln;)
        {
            char* e = ln; while (*e && *e != '\n') ++e; if (*e) *e++ = 0; char* nm = ln + 1; while (*nm == ' ') ++nm; while (*nm >= '0' && *nm <= '9') ++nm; while (*nm == ' ') ++nm; ln = e;
            size_t l = strlen(nm); if (l < 5 || strcmp(nm + l - 4, ".rpk")) continue;
            char path[256]; snprintf(path, sizeof(path), REPO "/%s", nm); unsigned long an; unsigned char* a = open_pkg(path, &an); Info in; char tx[4096];
            if (a && !pkg_info_from(a, an, &in, tx, sizeof(tx)) && (strcasestr(in.name, term) || strcasestr(in.desc, term))) { Info cur; printf("[%s] %-18s %-8s %s\n", installed(in.name, &cur) ? "installed" : "available", in.name, in.version, in.desc); ++hits; }
            free(a);
        }
        free(b); if (!hits) printf("no matches for '%s'\n", term); return hits ? 0 : 1;
    }
    if (!strcmp(cmd, "owner"))
    {
        if (ai >= argc) { usage(); return 2; } char ow[64]; if (owner_of(argv[ai], ow, sizeof(ow))) { printf("%s is owned by %s\n", argv[ai], ow); return 0; } printf("%s is not owned by any package\n", argv[ai]); return 1;
    }
    if (!strcmp(cmd, "verify"))
    {
        char* names = list_names(); int bad = 0, checked = 0, pk = 0;
        for (char* ln = names; *ln;)
        {
            char* nl = ln; while (*nl && *nl != '\n') ++nl; if (*nl) *nl++ = 0;
            if (ai < argc && strcmp(ln, argv[ai])) { ln = nl; continue; }
            char* t = db_read(ln); ++pk;
            for (const char* q = t; q && (q = strstr(q, "\nfile=")); )
            {
                q += 6; const char* sp = strchr(q, ' '); if (!sp) break; char path[300]; size_t l = (size_t)(sp - q); if (l > 299) l = 299; memcpy(path, q, l); path[l] = 0;
                unsigned long sz = strtoul(sp + 1, 0, 10); const char* cp = strchr(sp + 1, ' '); unsigned crc = cp ? (unsigned)strtoul(cp + 1, 0, 16) : 0; ++checked;
                unsigned long fn; unsigned char* d = tar_slurp(path, &fn);
                if (!d) { printf("%s: MISSING (%s)\n", path, ln); ++bad; continue; }
                if (fn != sz) { printf("%s: SIZE differs (%s)\n", path, ln); ++bad; } else if ((unsigned)rio_crc32(0, d, fn) != crc) { printf("%s: MODIFIED (%s)\n", path, ln); ++bad; }
                free(d);
            }
            free(t); ln = nl;
        }
        free(names);
        if (ai < argc && !pk) { printf("pkg: %s is not installed\n", argv[ai]); return 1; }
        printf("%d files checked, %d problems\n", checked, bad); return bad ? 1 : 0;
    }
    if (!strcmp(cmd, "build"))
    {
        if (argc - ai < 2) { usage(); return 2; } const char* dir = argv[ai]; const char* out = argv[ai + 1];
        char ip[300]; snprintf(ip, sizeof(ip), "%s/PKGINFO", dir); unsigned long in_n; unsigned char* it = tar_slurp(ip, &in_n);
        if (!it) { printf("pkg: %s not found\n", ip); return 1; }
        char text[4096]; size_t cp = in_n < sizeof(text) - 1 ? in_n : sizeof(text) - 1; memcpy(text, it, cp); text[cp] = 0; Info in; parse_info(text, &in);
        if (!valid_name(in.name) || !in.version[0]) { printf("pkg: PKGINFO needs name= (a-z 0-9 - _ . +) and version=\n"); free(it); return 1; }
        TarBuf b = { 0, 0, 0 }; int errors = 0, count = 0;
        tar_add(&b, "PKGINFO", '0', 0644, (unsigned long)time_epoch(), 0, 0, it, in_n); free(it);
        add_tree(&b, dir, "", &errors, &count); tar_finish(&b);
        unsigned char* o = b.p; unsigned long on = b.len;
        if (zflag) { unsigned char* c; unsigned long cn; if (rio_gzip_compress(b.p, b.len, 0, (uint32_t)time_epoch(), &c, &cn, 6)) { printf("pkg: compress failed\n"); return 1; } free(b.p); o = c; on = cn; }
        FILE* f = fopen(out, "w"); if (!f || fwrite(o, 1, on, f) != on) { printf("pkg: cannot write %s\n", out); return 1; } fclose(f);
        printf("built %s %s -> %s (%d files, %lu bytes)\n", in.name, in.version, out, count, on); return errors ? 1 : 0;
    }
    if (!strcmp(cmd, "update"))
    {
        char* b = (char*)malloc(65536); long n = readdir_text(REPO, b, 65535); if (n < 0) n = 0; b[n] = 0;
        char* lines[256]; int nlines = 0;
        for (char* ln = b; *ln && nlines < 256;)
        {
            char* e = ln; while (*e && *e != '\n') ++e; if (*e) *e++ = 0; char* nm = ln + 1; while (*nm == ' ') ++nm; while (*nm >= '0' && *nm <= '9') ++nm; while (*nm == ' ') ++nm; ln = e;
            size_t l = strlen(nm); if (l < 5 || strcmp(nm + l - 4, ".rpk")) continue;
            char path[256]; snprintf(path, sizeof(path), REPO "/%s", nm); unsigned long an; unsigned char* a = open_pkg(path, &an); Info in; char tx[4096];
            if (a && !pkg_info_from(a, an, &in, tx, sizeof(tx)) && valid_name(in.name)) { char* row = (char*)malloc(400); snprintf(row, 400, "%s %s %s | %s", in.name, in.version, nm, in.desc); lines[nlines++] = row; }
            else printf("pkg: skipping unreadable %s\n", nm);
            free(a);
        }
        qsort(lines, (size_t)nlines, sizeof(char*), cmp_names);
        FILE* f = fopen(DB "/INDEX", "w"); if (!f) { printf("pkg: cannot write index\n"); return 1; }
        for (int i = 0; i < nlines; ++i) { fwrite(lines[i], 1, strlen(lines[i]), f); fwrite("\n", 1, 1, f); free(lines[i]); }
        fclose(f); free(b); printf("index updated: %d packages in " REPO "\n", nlines); return 0;
    }
    printf("pkg: unknown command '%s'\n", cmd); usage(); return 2;
}
