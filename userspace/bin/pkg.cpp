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
        if (*b && !(*b >= '0' && *b <= '9') && *b != '.') return *a < *b ? -1 : 1;   /* never loop on a trailing non-numeric part */
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

/* ---- network repositories --------------------------------------------------------------
   /etc/pkg/remotes : one repository per line: "<url> [-k] [--cacert <file>]"   (url = http[s]://host[:port]/dir)
   <url>/INDEX      : lines "name version file.rpk sha256hex | description"   (made by 'pkg mkindex <dir>')
   /var/lib/pkg/REMOTE : merged, validated copy written by 'pkg update' ("name version file sha256 N | desc", N = remote number)
   Every download is checked against the sha256 of the index; HTTPS certificates are verified (kernel TLS) unless -k. */
#define REMOTES "/etc/pkg/remotes"
#define RIDX DB "/REMOTE"
#define NET_MAX (1024 * 1024)
struct Remote { char url[200]; int insecure; int unsigned_ok; char ca[100]; };
static int remotes_load(Remote* r, int max)
{
    FILE* f = fopen(REMOTES, "r"); if (!f) return 0; int n = 0; char ln[400];
    while (n < max && fgets(ln, sizeof(ln), f))
    {
        trim_line(ln); size_t l = strlen(ln); while (l && ln[l - 1] == '\n') ln[--l] = 0; trim_line(ln);
        char* tok = strtok(ln, " "); if (!tok || tok[0] == '#') continue;
        memset(&r[n], 0, sizeof(Remote)); strncpy(r[n].url, tok, sizeof(r[n].url) - 1);
        while ((tok = strtok(0, " ")))
        {
            if (!strcmp(tok, "-k")) r[n].insecure = 1;
            else if (!strcmp(tok, "--unsigned")) r[n].unsigned_ok = 1;
            else if (!strcmp(tok, "--cacert") && (tok = strtok(0, " "))) strncpy(r[n].ca, tok, sizeof(r[n].ca) - 1);
        }
        ++n;
    }
    fclose(f); return n;
}
static int remotes_save(const Remote* r, int n)
{
    mkdirs_abs("/etc/pkg"); FILE* f = fopen(REMOTES, "w"); if (!f) return -1;
    for (int i = 0; i < n; ++i) { fprintf(f, "%s", r[i].url); if (r[i].insecure) fprintf(f, " -k"); if (r[i].unsigned_ok) fprintf(f, " --unsigned"); if (r[i].ca[0]) fprintf(f, " --cacert %s", r[i].ca); fprintf(f, "\n"); }
    fclose(f); return 0;
}
static int url_ok(const char* u) { return !strncmp(u, "http://", 7) || !strncmp(u, "https://", 8); }
static int is_sha(const char* h) { if (strlen(h) != 64) return 0; for (int i = 0; i < 64; ++i) if (!isxdigit((unsigned char)h[i]) || isupper((unsigned char)h[i])) return 0; return 1; }
static void to_hex(const unsigned char* d, char* o) { static const char* x = "0123456789abcdef"; for (int i = 0; i < 32; ++i) { o[2 * i] = x[d[i] >> 4]; o[2 * i + 1] = x[d[i] & 15]; } o[64] = 0; }
static int safe_file(const char* n) { size_t l = strlen(n); if (l < 5 || l > 90 || strcmp(n + l - 4, ".rpk") || strstr(n, "..")) return 0; for (; *n; ++n) if (!(islower((unsigned char)*n) || isdigit((unsigned char)*n) || *n == '-' || *n == '_' || *n == '.' || *n == '+')) return 0; return 1; }
static void join_url(const Remote* r, const char* file, char* out, size_t cap)
{ size_t l = strlen(r->url); while (l && r->url[l - 1] == '/') --l; snprintf(out, cap, "%.*s/%s", (int)l, r->url, file); }
static long remote_get(const Remote* r, const char* file, unsigned char* buf, size_t cap)
{ char u[320]; join_url(r, file, u, sizeof(u)); return http_get(u, buf, cap, r->insecure, r->ca[0] ? r->ca : 0); }


/* ---- signatures (Ed25519) ----------------------------------------------------------------------------------------------
   Trust store : /etc/pkg/trusted/<name>.pub   one line "ed25519-pub <64 hex> [comment]"
   Signing key : /etc/pkg/keys/<name>.sec      one line "ed25519-sec <64 hex seed>" (mode 0600)    made by 'pkg keygen <name>'
   Repository  : INDEX is signed (INDEX.sig, raw 64-byte signature, hex); its first line "# serial <n>" must never go backwards
                 (rollback protection), and every .rpk is pinned by its sha256 in the signed index.
   Local files : <file>.rpk.sig (detached, same format) is verified when present; /etc/pkg/policy "require-signatures" makes it mandatory.
   A remote without a valid signed index is refused unless it was added with --unsigned. */
#define KEYS_DIR "/etc/pkg/keys"
#define TRUST_DIR "/etc/pkg/trusted"
#define POLICY "/etc/pkg/policy"
static int hex_val(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int from_hex(const char* h, unsigned char* out, int n) { for (int i = 0; i < n; ++i) { int a = hex_val(h[2 * i]), b = h[2 * i] ? hex_val(h[2 * i + 1]) : -1; if (a < 0 || b < 0) return -1; out[i] = (unsigned char)(a * 16 + b); } return 0; }
static void bytes_hex(const unsigned char* d, int n, char* o) { static const char* x = "0123456789abcdef"; for (int i = 0; i < n; ++i) { o[2 * i] = x[d[i] >> 4]; o[2 * i + 1] = x[d[i] & 15]; } o[2 * n] = 0; }
static void fingerprint(const unsigned char* pub, char* out17) { unsigned char dg[32]; sha256_buf(pub, 32, dg); bytes_hex(dg, 8, out17); }
static int valid_keyname(const char* n) { size_t l = strlen(n); if (!l || l > 40) return 0; for (; *n; ++n) if (!(islower((unsigned char)*n) || isdigit((unsigned char)*n) || *n == '-' || *n == '_')) return 0; return 1; }
static int key_file_read(const char* path, const char* tag, unsigned char* out)
{
    FILE* f = fopen(path, "r"); if (!f) return -1; char ln[256]; ln[0] = 0; fgets(ln, sizeof(ln), f); fclose(f);
    char t[24], hx[100]; if (sscanf(ln, "%23s %99s", t, hx) != 2 || strcmp(t, tag) || strlen(hx) != 64) return -1;
    return from_hex(hx, out, 32);
}
static int policy_requires_signatures(void)
{
    FILE* f = fopen(POLICY, "r"); if (!f) return 0; char ln[128]; int req = 0;
    while (fgets(ln, sizeof(ln), f)) { trim_line(ln); size_t l = strlen(ln); while (l && ln[l - 1] == '\n') ln[--l] = 0; if (!strcmp(ln, "require-signatures")) req = 1; }
    fclose(f); return req;
}
/* verifies sig (raw 64 bytes) over data against every trusted key; returns 1 and the key fingerprint when valid */
static int trusted_verify(const unsigned char* data, unsigned long n, const unsigned char* sig, char* fp)
{
    char* names = (char*)malloc(8192); long ln0 = readdir_text(TRUST_DIR, names, 8191); if (ln0 < 0) { free(names); return 0; } names[ln0] = 0; int ok = 0;
    for (char* ln = names; *ln && !ok;)
    {
        char* e = ln; while (*e && *e != '\n') ++e; if (*e) *e++ = 0; char* nm = ln + 1; while (*nm == ' ') ++nm; while (*nm >= '0' && *nm <= '9') ++nm; while (*nm == ' ') ++nm; ln = e;
        size_t l = strlen(nm); if (l < 5 || strcmp(nm + l - 4, ".pub")) continue;
        char path[160]; snprintf(path, sizeof(path), TRUST_DIR "/%s", nm); unsigned char pub[32];
        if (key_file_read(path, "ed25519-pub", pub) == 0 && ed25519_verify(pub, data, n, sig) == 0) { ok = 1; if (fp) fingerprint(pub, fp); }
    }
    free(names); return ok;
}
static int sig_file_read(const char* path, unsigned char* sig)
{
    FILE* f = fopen(path, "r"); if (!f) return -1; char hx[300]; hx[0] = 0; fgets(hx, sizeof(hx), f); fclose(f);
    char* t = strtok(hx, " \r\n"); if (!t || strlen(t) != 128) return -1; return from_hex(t, sig, 64);
}
static int sig_file_write(const char* path, const unsigned char* sig)
{ char hx[130]; bytes_hex(sig, 64, hx); FILE* f = fopen(path, "w"); if (!f) return -1; fprintf(f, "%s\n", hx); fclose(f); return 0; }
static int sign_buffer(const char* keyspec, const unsigned char* data, unsigned long n, unsigned char* sig)
{
    char path[200]; if (strchr(keyspec, '/')) snprintf(path, sizeof(path), "%s", keyspec); else snprintf(path, sizeof(path), KEYS_DIR "/%s.sec", keyspec);
    unsigned char seed[32]; if (key_file_read(path, "ed25519-sec", seed)) { printf("pkg: cannot read signing key %s\n", path); return -1; }
    int rc = ed25519_sign(seed, data, n, sig); memset(seed, 0, sizeof(seed)); return rc;
}
/* is the package file trusted?  1 = signed / pinned by a signed index, 0 = unsigned, -1 = bad signature */
static int package_trust(const char* path, char* why, size_t cap)
{
    unsigned long n; unsigned char* raw = tar_slurp(path, &n); if (!raw) { snprintf(why, cap, "unreadable"); return -1; }
    char sp[300]; snprintf(sp, sizeof(sp), "%s.sig", path); unsigned char sig[64]; int r = 0;
    if (sig_file_read(sp, sig) == 0)
    {
        char fp[20]; if (trusted_verify(raw, n, sig, fp)) { snprintf(why, cap, "signed by key %s", fp); r = 1; }
        else { snprintf(why, cap, "signature %s is not valid for any trusted key", sp); r = -1; }
    }
    else
    {
        const char* b = strrchr(path, '/'); b = b ? b + 1 : path;
        unsigned char dg[32]; char hx[65]; sha256_buf(raw, n, dg); to_hex(dg, hx);
        FILE* f = fopen(RIDX, "r"); char ln[400];
        while (f && fgets(ln, sizeof(ln), f)) { char nm[64], v[32], fl[100], h[80]; int ri; if (sscanf(ln, "%63s %31s %99s %79s %d", nm, v, fl, h, &ri) == 5 && !strcmp(fl, b) && !strcmp(h, hx)) { r = 1; snprintf(why, cap, "listed in the signed repository index"); break; } }
        if (f) fclose(f);
        if (!r) snprintf(why, cap, "unsigned");
    }
    free(raw); return r;
}

static int remote_update(void)
{
    Remote rs[8]; int nr = remotes_load(rs, 8); if (!nr) return 0;
    FILE* out = fopen(RIDX, "w"); if (!out) { printf("pkg: cannot write " RIDX "\n"); return 1; }
    unsigned char* buf = (unsigned char*)malloc(NET_MAX + 1); int rc = 0, total = 0;
    for (int i = 0; i < nr; ++i)
    {
        long n = remote_get(&rs[i], "INDEX", buf, 262144);
        if (n < 0) { printf("pkg: %s: cannot fetch INDEX (%s)\n", rs[i].url, errno == ENOENT ? "not found" : errno == EIO ? "connection or TLS verification failed" : "error"); rc = 1; continue; }
        buf[n] = 0; int cnt = 0;
        {   /* signed index: signature by a trusted key, serial never going backwards */
            unsigned char sig[64]; unsigned char* sb = (unsigned char*)malloc(512); long sn = remote_get(&rs[i], "INDEX.sig", sb, 400); int sig_ok = 0; char fp[20]; fp[0] = 0;
            if (sn > 0) { sb[sn] = 0; char* t = strtok((char*)sb, " \r\n"); if (t && strlen(t) == 128 && from_hex(t, sig, 64) == 0) sig_ok = trusted_verify(buf, (unsigned long)n, sig, fp); }
            free(sb);
            if (!sig_ok)
            {
                if (!rs[i].unsigned_ok) { printf("pkg: %s: INDEX is %s - repository refused (trust its key with 'pkg trust add', or add the remote with --unsigned)\n", rs[i].url, sn > 0 ? "signed with an unknown or invalid key" : "not signed"); rc = 1; continue; }
                printf("pkg: warning: %s: unsigned index accepted (--unsigned)\n", rs[i].url);
            }
            else
            {
                unsigned long long serial = 0; const char* sp = strstr((const char*)buf, "# serial "); if (sp && (sp == (const char*)buf || sp[-1] == '\n')) serial = strtoull(sp + 9, 0, 10);
                unsigned char ud[32]; sha256_buf(rs[i].url, strlen(rs[i].url), ud); char tag[20]; bytes_hex(ud, 4, tag);
                char spath[120]; snprintf(spath, sizeof(spath), DB "/serial.%s", tag); unsigned long long last = 0; FILE* sf = fopen(spath, "r"); if (sf) { char l2[40]; if (fgets(l2, sizeof(l2), sf)) last = strtoull(l2, 0, 10); fclose(sf); }
                if (serial < last) { printf("pkg: %s: ROLLBACK refused (index serial %llu is older than the last seen %llu)\n", rs[i].url, serial, last); rc = 1; continue; }
                if (serial > last) { sf = fopen(spath, "w"); if (sf) { fprintf(sf, "%llu\n", serial); fclose(sf); } }
                printf("%s: index signed by key %s (serial %llu)\n", rs[i].url, fp, serial);
            }
        }
        for (char* ln = (char*)buf; *ln;)
        {
            char* e = ln; while (*e && *e != '\n') ++e; if (*e) *e++ = 0; char* cur = ln; ln = e; trim_line(cur);
            char name[64], ver[32], file[100], sha[80];
            if (!cur[0] || cur[0] == '#') continue;
            char* bar = strstr(cur, " | "); char desc[160]; desc[0] = 0; if (bar) { *bar = 0; strncpy(desc, bar + 3, sizeof(desc) - 1); desc[sizeof(desc) - 1] = 0; }
            if (sscanf(cur, "%63s %31s %99s %79s", name, ver, file, sha) != 4 || !valid_name(name) || !safe_file(file) || !is_sha(sha)) { printf("pkg: %s: ignoring bad INDEX line\n", rs[i].url); continue; }
            fprintf(out, "%s %s %s %s %d | %s\n", name, ver, file, sha, i, desc); ++cnt;
        }
        printf("%s: %d package(s)\n", rs[i].url, cnt); total += cnt;
    }
    fclose(out); free(buf); printf("remote index updated: %d package(s) from %d repositor%s\n", total, nr, nr == 1 ? "y" : "ies"); return rc;
}
/* best (highest version) entry of 'name' in the remote index */
static int remote_best(const char* name, char* ver, char* file, char* sha, int* ri)
{
    FILE* f = fopen(RIDX, "r"); if (!f) return 0; char ln[400]; int found = 0;
    while (fgets(ln, sizeof(ln), f))
    {
        char n[64], v[32], fl[100], h[80]; int r;
        if (sscanf(ln, "%63s %31s %99s %79s %d", n, v, fl, h, &r) != 5 || strcmp(n, name)) continue;
        if (!found || vercmp(v, ver) > 0) { strcpy(ver, v); strcpy(file, fl); strcpy(sha, h); *ri = r; found = 1; }
    }
    fclose(f); return found;
}
/* download + verify + store in the local repository; returns 1 with the path in out */
static int remote_fetch(const char* name, char* out, size_t cap)
{
    char ver[32], file[100], sha[80]; int ri; ver[0] = 0;
    if (!remote_best(name, ver, file, sha, &ri)) return 0;
    Remote rs[8]; int nr = remotes_load(rs, 8); if (ri < 0 || ri >= nr) { printf("pkg: remote list changed, run 'pkg update'\n"); return 0; }
    unsigned char* buf = (unsigned char*)malloc(NET_MAX); printf("downloading %s %s from %s ...\n", name, ver, rs[ri].url);
    long n = remote_get(&rs[ri], file, buf, NET_MAX);
    if (n < 0) { printf("pkg: download of %s failed (%s)\n", file, errno == ENOENT ? "not found on the server" : errno == EFBIG ? "package larger than 1 MB" : "connection or TLS verification failed"); free(buf); return 0; }
    unsigned char dg[32]; char hx[65]; sha256_buf(buf, (size_t)n, dg); to_hex(dg, hx);
    if (strcmp(hx, sha)) { printf("pkg: CHECKSUM MISMATCH for %s (index %s, got %s): package rejected\n", file, sha, hx); free(buf); return 0; }
    snprintf(out, cap, REPO "/%s", file); FILE* f = fopen(out, "w");
    if (!f || fwrite(buf, 1, (size_t)n, f) != (size_t)n) { printf("pkg: cannot store %s\n", out); if (f) fclose(f); free(buf); return 0; }
    fclose(f); free(buf); printf("verified sha256 %.16s... (%ld bytes)\n", hx, n); return 1;
}
/* local repository first, then the network */
static int repo_find_net(const char* name, char* out, size_t cap)
{
    if (repo_find(name, out, cap))
    {   /* a newer package may exist remotely */
        char ver[32], file[100], sha[80]; int ri; ver[0] = 0;
        if (remote_best(name, ver, file, sha, &ri))
        {
            char lp[256]; snprintf(lp, sizeof(lp), "%s", out); const char* b = strrchr(lp, '/'); b = b ? b + 1 : lp; size_t nl = strlen(name);
            char lv[48]; lv[0] = 0; if (!strncmp(b, name, nl) && b[nl] == '-') { strncpy(lv, b + nl + 1, sizeof(lv) - 1); lv[sizeof(lv) - 1] = 0; size_t ll = strlen(lv); if (ll > 4 && !strcmp(lv + ll - 4, ".rpk")) lv[ll - 4] = 0; }
            if (lv[0] && vercmp(ver, lv) > 0) { char np[256]; if (remote_fetch(name, np, sizeof(np))) { snprintf(out, cap, "%s", np); return 1; } }
        }
        return 1;
    }
    return remote_fetch(name, out, cap);
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
    else if (repo_find_net(arg, path, sizeof(path))) via_repo = 1;
    else { printf("pkg: package '%s' not found in " REPO " or on any remote (try 'pkg update')\n", arg); return 1; }
    unsigned long an; unsigned char* a = open_pkg(path, &an); if (!a) { printf("pkg: cannot read package %s\n", path); return 1; }
    char text[4096]; Info in;
    if (pkg_info_from(a, an, &in, text, sizeof(text)) || !valid_name(in.name) || !in.version[0]) { printf("pkg: %s: missing or invalid PKGINFO (need name and version)\n", path); free(a); return 1; }
    {
        char why[200]; int t = package_trust(path, why, sizeof(why));
        if (t < 0) { printf("pkg: %s: %s - installation refused\n", path, why); free(a); return 1; }
        if (t == 0 && policy_requires_signatures() && !force_flag) { printf("pkg: %s is unsigned and " POLICY " requires signatures (-f overrides)\n", path); free(a); return 1; }
        if (t == 0) printf("pkg: warning: %s is unsigned\n", path); else if (!is_dep) printf("pkg: %s: %s\n", path, why);
    }
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
        char rp[256]; if (repo_find_net(d.name, rp, sizeof(rp)))
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
           "  build [-z] <dir> <out>    build a package from <dir> (must hold PKGINFO)\n  update                    rebuild the local index (" DB "/INDEX) and fetch the indexes of all remotes\n"
           "  remote add <url> [-k] [--cacert <file>] [--unsigned] | remove <url> | list     network repositories (http:// or https://, checksummed, signed index required)\n  mkindex [-s <key>] <dir>  write <dir>/INDEX (+ INDEX.sig when signing) for the .rpk files in <dir>, to serve over HTTP(S)\n"
           "  keygen <name>             create an Ed25519 signing key (" KEYS_DIR "/<name>.sec, .pub)\n  trust add <file.pub|hex name> | list | remove <name>   keys trusted for signatures (" TRUST_DIR ")\n"
           "  sign <key> <file>         write <file>.sig      checksig <file>   verify <file> against <file>.sig / the signed index\n"
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
            if (installed(ln, &cur) && repo_find_net(ln, rp, sizeof(rp)))
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
        free(b);
        { FILE* rf = fopen(RIDX, "r"); char ln[400]; while (rf && fgets(ln, sizeof(ln), rf)) { char n[64], v[32], fl[100], h[80]; int r; if (sscanf(ln, "%63s %31s %99s %79s %d", n, v, fl, h, &r) != 5) continue; char* bar = strstr(ln, " | "); const char* ds = bar ? bar + 3 : ""; if (strcasestr(n, term) || strcasestr(ds, term)) { Info cur; printf("[%s] %-18s %-8s %s", installed(n, &cur) ? "installed" : "remote", n, v, ds); if (!bar || !strchr(ds, '\n')) printf("\n"); ++hits; } } if (rf) fclose(rf); }
        if (!hits) printf("no matches for '%s'\n", term); return hits ? 0 : 1;
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
    if (!strcmp(cmd, "remote"))
    {
        Remote rs[8]; int nr = remotes_load(rs, 8);
        const char* sub = ai < argc ? argv[ai] : "list";
        if (!strcmp(sub, "list")) { if (!nr) printf("no remotes (pkg remote add <url>)\n"); for (int i = 0; i < nr; ++i) printf("%d: %s%s%s%s%s\n", i, rs[i].url, rs[i].insecure ? "  [certificate NOT verified]" : "", rs[i].ca[0] ? "  cacert " : "", rs[i].ca, rs[i].unsigned_ok ? "  [unsigned index accepted]" : ""); return 0; }
        if (!strcmp(sub, "add"))
        {
            if (ai + 1 >= argc) { usage(); return 2; } Remote nw; memset(&nw, 0, sizeof(nw));
            if (!url_ok(argv[ai + 1]) || strlen(argv[ai + 1]) > 190) { printf("pkg: remote must be http://... or https://...\n"); return 1; }
            strncpy(nw.url, argv[ai + 1], sizeof(nw.url) - 1);
            for (int i = ai + 2; i < argc; ++i) { if (!strcmp(argv[i], "-k")) nw.insecure = 1; else if (!strcmp(argv[i], "--unsigned")) nw.unsigned_ok = 1; else if (!strcmp(argv[i], "--cacert") && i + 1 < argc) strncpy(nw.ca, argv[++i], sizeof(nw.ca) - 1); else { usage(); return 2; } }
            for (int i = 0; i < nr; ++i) if (!strcmp(rs[i].url, nw.url)) { rs[i] = nw; remotes_save(rs, nr); printf("remote updated: %s\n", nw.url); return 0; }
            if (nr >= 8) { printf("pkg: at most 8 remotes\n"); return 1; }
            rs[nr++] = nw; if (remotes_save(rs, nr)) { printf("pkg: cannot write " REMOTES " (root needed)\n"); return 1; } printf("remote added: %s (run 'pkg update')\n", nw.url); return 0;
        }
        if (!strcmp(sub, "remove"))
        {
            if (ai + 1 >= argc) { usage(); return 2; } int k = -1; for (int i = 0; i < nr; ++i) if (!strcmp(rs[i].url, argv[ai + 1])) k = i;
            if (k < 0) { printf("pkg: no such remote\n"); return 1; } for (int i = k; i + 1 < nr; ++i) rs[i] = rs[i + 1]; --nr;
            if (remotes_save(rs, nr)) { printf("pkg: cannot write " REMOTES "\n"); return 1; } unlink(RIDX); printf("remote removed (run 'pkg update')\n"); return 0;
        }
        usage(); return 2;
    }
    if (!strcmp(cmd, "keygen"))
    {
        if (ai >= argc || !valid_keyname(argv[ai])) { printf("usage: pkg keygen <name>   (name: a-z 0-9 - _)\n"); return 2; }
        if (!rng_ready() && !force_flag) { printf("pkg: the kernel has not collected enough entropy yet (see 'rng'); use the system a little longer or pass -f to override\n"); return 1; }
        mkdirs_abs(KEYS_DIR); mkdirs_abs(TRUST_DIR);
        char sp[160], pp[160]; snprintf(sp, sizeof(sp), KEYS_DIR "/%s.sec", argv[ai]); snprintf(pp, sizeof(pp), KEYS_DIR "/%s.pub", argv[ai]);
        FILE* t = fopen(sp, "r"); if (t) { fclose(t); printf("pkg: %s already exists (refusing to overwrite a signing key)\n", sp); return 1; }
        unsigned char seed[32], pub[32]; if (getrandom(seed, 32, 0) != 32 || ed25519_pubkey(seed, pub)) { printf("pkg: key generation failed\n"); return 1; }
        char hs[70], hp[70]; bytes_hex(seed, 32, hs); bytes_hex(pub, 32, hp);
        FILE* f = fopen(sp, "w"); if (!f) { printf("pkg: cannot write %s (root needed)\n", sp); return 1; } chmod(sp, 0600); fprintf(f, "ed25519-sec %s\n", hs); fclose(f);
        f = fopen(pp, "w"); if (!f) { printf("pkg: cannot write %s\n", pp); return 1; } fprintf(f, "ed25519-pub %s %s\n", hp, argv[ai]); fclose(f);
        memset(seed, 0, sizeof(seed)); memset(hs, 0, sizeof(hs)); char fp[20]; fingerprint(pub, fp);
        printf("key '%s' created\n  secret : %s (mode 0600 - keep it private, back it up)\n  public : %s\n  id     : %s\nGive the .pub file to users: pkg trust add %s.pub\n", argv[ai], sp, pp, fp, argv[ai]); return 0;
    }
    if (!strcmp(cmd, "trust"))
    {
        const char* sub = ai < argc ? argv[ai] : "list";
        if (!strcmp(sub, "list"))
        {
            char* b = (char*)malloc(8192); long n = readdir_text(TRUST_DIR, b, 8191); if (n < 0) n = 0; b[n] = 0; int cnt = 0;
            for (char* ln = b; *ln;) { char* e = ln; while (*e && *e != '\n') ++e; if (*e) *e++ = 0; char* nm = ln + 1; while (*nm == ' ') ++nm; while (*nm >= '0' && *nm <= '9') ++nm; while (*nm == ' ') ++nm; ln = e;
                size_t l = strlen(nm); if (l < 5 || strcmp(nm + l - 4, ".pub")) continue; char path[160]; snprintf(path, sizeof(path), TRUST_DIR "/%s", nm); unsigned char pub[32];
                if (!key_file_read(path, "ed25519-pub", pub)) { char fp[20]; fingerprint(pub, fp); printf("%s  %.*s\n", fp, (int)(l - 4), nm); ++cnt; } }
            free(b); if (!cnt) printf("no trusted keys (pkg trust add <file.pub|64-hex-key> [name])\n"); return 0;
        }
        if (!strcmp(sub, "add"))
        {
            if (ai + 1 >= argc) { usage(); return 2; } unsigned char pub[32]; const char* src = argv[ai + 1]; char name[48]; name[0] = 0;
            if (strlen(src) == 64 && from_hex(src, pub, 32) == 0) { if (ai + 2 >= argc) { printf("pkg: a name is required when adding a raw hex key\n"); return 2; } }
            else
            {
                if (key_file_read(src, "ed25519-pub", pub)) { printf("pkg: %s is not an ed25519 public key file\n", src); return 1; }
                const char* b = strrchr(src, '/'); b = b ? b + 1 : src; size_t l = strlen(b); if (l > 4 && !strcmp(b + l - 4, ".pub")) l -= 4; if (l < sizeof(name)) { memcpy(name, b, l); name[l] = 0; }
            }
            if (ai + 2 < argc) snprintf(name, sizeof(name), "%s", argv[ai + 2]);
            if (!valid_keyname(name)) { printf("pkg: bad key name\n"); return 1; }
            mkdirs_abs(TRUST_DIR); char path[160]; snprintf(path, sizeof(path), TRUST_DIR "/%s.pub", name); char hp[70]; bytes_hex(pub, 32, hp);
            FILE* f = fopen(path, "w"); if (!f) { printf("pkg: cannot write %s (root needed)\n", path); return 1; } fprintf(f, "ed25519-pub %s %s\n", hp, name); fclose(f);
            char fp[20]; fingerprint(pub, fp); printf("trusted key '%s' (id %s)\n", name, fp); return 0;
        }
        if (!strcmp(sub, "remove"))
        {
            if (ai + 1 >= argc || !valid_keyname(argv[ai + 1])) { usage(); return 2; } char path[160]; snprintf(path, sizeof(path), TRUST_DIR "/%s.pub", argv[ai + 1]);
            if (unlink(path)) { printf("pkg: no such trusted key\n"); return 1; } printf("key removed\n"); return 0;
        }
        usage(); return 2;
    }
    if (!strcmp(cmd, "sign"))
    {
        if (ai + 1 >= argc) { printf("usage: pkg sign <keyname|keyfile> <file>\n"); return 2; }
        unsigned long n; unsigned char* raw = tar_slurp(argv[ai + 1], &n); if (!raw) { printf("pkg: cannot read %s\n", argv[ai + 1]); return 1; }
        unsigned char sig[64]; if (sign_buffer(argv[ai], raw, n, sig)) { free(raw); return 1; } free(raw);
        char sp[300]; snprintf(sp, sizeof(sp), "%s.sig", argv[ai + 1]); if (sig_file_write(sp, sig)) { printf("pkg: cannot write %s\n", sp); return 1; } printf("wrote %s\n", sp); return 0;
    }
    if (!strcmp(cmd, "checksig"))
    {
        if (ai >= argc) { usage(); return 2; } char why[200]; int t = package_trust(argv[ai], why, sizeof(why)); printf("%s: %s\n", argv[ai], why); return t == 1 ? 0 : 1;
    }
    if (!strcmp(cmd, "mkindex"))
    {
        const char* skey = 0; if (ai + 1 < argc && !strcmp(argv[ai], "-s")) { skey = argv[ai + 1]; ai += 2; }
        if (ai >= argc) { usage(); return 2; } const char* dir = argv[ai];
        char* b = (char*)malloc(65536); long n = readdir_text(dir, b, 65535); if (n < 0) { printf("pkg: cannot read %s\n", dir); return 1; } b[n] = 0;
        char ip[300]; snprintf(ip, sizeof(ip), "%s/INDEX", dir); FILE* o = fopen(ip, "w"); if (!o) { printf("pkg: cannot write %s\n", ip); return 1; } int cnt = 0;
        fprintf(o, "# serial %lld\n", (long long)time(0));
        for (char* ln = b; *ln;)
        {
            char* e = ln; while (*e && *e != '\n') ++e; if (*e) *e++ = 0; char* nm = ln + 1; while (*nm == ' ') ++nm; while (*nm >= '0' && *nm <= '9') ++nm; while (*nm == ' ') ++nm; ln = e;
            if (!safe_file(nm)) continue;
            char path[400]; snprintf(path, sizeof(path), "%s/%s", dir, nm); unsigned long an; unsigned char* raw = tar_slurp(path, &an);
            unsigned long pn; unsigned char* a = open_pkg(path, &pn); Info in; char tx[4096];
            if (raw && a && !pkg_info_from(a, pn, &in, tx, sizeof(tx)) && valid_name(in.name) && in.version[0])
            { unsigned char dg[32]; char hx[65]; sha256_buf(raw, an, dg); to_hex(dg, hx); fprintf(o, "%s %s %s %s | %s\n", in.name, in.version, nm, hx, in.desc); ++cnt; }
            else printf("pkg: skipping unreadable %s\n", nm);
            free(a); free(raw);
        }
        fclose(o); free(b); printf("wrote %s: %d package(s)\n", ip, cnt);
        if (skey)
        {
            unsigned long xn; unsigned char* xr = tar_slurp(ip, &xn); unsigned char sig[64]; char sp[320];
            if (!xr || sign_buffer(skey, xr, xn, sig)) { free(xr); return 1; } free(xr);
            snprintf(sp, sizeof(sp), "%s.sig", ip); if (sig_file_write(sp, sig)) { printf("pkg: cannot write %s\n", sp); return 1; } printf("signed: %s\n", sp);
        }
        else printf("pkg: note: the index is not signed (use: pkg mkindex -s <key> <dir>); clients refuse unsigned repositories by default\n");
        return 0;
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
        fclose(f); free(b); printf("index updated: %d packages in " REPO "\n", nlines); return remote_update();
    }
    printf("pkg: unknown command '%s'\n", cmd); usage(); return 2;
}
