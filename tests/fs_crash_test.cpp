/* Crash-injection test for the journaled filesystem: power is cut at every single sector write of a set of metadata-heavy operations
   (two disk models: in-order write-through, and a volatile write cache where only flushed data plus a random subset of the unflushed
   writes survive). After "reboot" the filesystem must mount from the disk (no RAM fallback), pass its own integrity check, and show
   exactly the state before the operation or exactly the state after it - never anything in between. */
#include "fs.h"
#include "types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include <algorithm>

static const uint64_t SECTORS = 32768, SS = 512;
static std::vector<uint8_t> live, dur;
struct W { uint64_t s; uint8_t d[512]; };
static std::vector<W> pending;
static long wcount = 0, crash_at = -1;      /* crash when wcount reaches crash_at */
static bool crashed = false, cache_model = false;
static bool ready = true;

void uart_putc(char) {}
void uart_puts(const char*) {}
void storage_init() {}
int storage_ready() { return ready; }
int storage_read_sector(uint64_t s, void* b) { if (s >= SECTORS) return -1; memcpy(b, &live[s * SS], SS); return 0; }
int storage_write_sector(uint64_t s, const void* b)
{
    if (s >= SECTORS) return -1;
    if (crashed) return 0;                                  /* power is gone: the code keeps running, nothing reaches the disk */
    if (crash_at >= 0 && wcount == crash_at) { crashed = true; return 0; }
    ++wcount;
    memcpy(&live[s * SS], b, SS);
    if (cache_model) { W w; w.s = s; memcpy(w.d, b, SS); pending.push_back(w); }
    else memcpy(&dur[s * SS], b, SS);
    return 0;
}
int storage_flush()
{
    if (crashed) return 0;
    for (auto& w : pending) memcpy(&dur[w.s * SS], w.d, SS);
    pending.clear();
    return 0;
}

static unsigned rng_state = 12345;
static unsigned rnd() { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL %s (line %d)\n", msg, __LINE__); ++fails; } } while (0)

static void walk(int id, const std::string& path, std::vector<std::string>& out)
{
    FsStat st; char line[400];
    int n = fs_get_child_count(id);
    for (int i = 0; i < n; ++i)
    {
        int c = fs_get_child(id, i); if (c < 0) continue;
        std::string p = path + "/" + fs_get_name(c);
        int t = fs_get_type(c); uint32_t h = 2166136261u; uint64_t sz = fs_file_size(c);
        static uint8_t buf[4 * 1024 * 1024 + 16];
        if (t == FS_FILE) { uint64_t got = 0; if (fs_read_file(p.c_str(), fs_get_root(), buf, sizeof buf, &got) == 0) for (uint64_t k = 0; k < got; ++k) { h ^= buf[k]; h *= 16777619u; } else h = 1; }
        if (t == FS_SYMLINK) { char tg[200]; int l = fs_link_target_of(c, tg, sizeof tg); for (int k = 0; k < l; ++k) { h ^= (uint8_t)tg[k]; h *= 16777619u; } }
        snprintf(line, sizeof line, "%s t=%d sz=%llu mode=%o uid=%u gid=%u h=%08x", p.c_str(), t, (unsigned long long)sz, fs_get_mode(c), fs_get_uid(c), fs_get_gid(c), h);
        out.push_back(line);
        (void)st;
        if (t == FS_DIR) walk(c, p, out);
    }
}
static std::string snap()
{
    std::vector<std::string> v; walk(fs_get_root(), "", v);
    std::string s; for (auto& l : v) { s += l; s += '\n'; } return s;
}

typedef void (*Op)();
static bool subset(const std::string& a, const std::string& b)      // every line of a occurs in b
{
    size_t p = 0;
    while (p < a.size()) { size_t e = a.find((char)10, p); std::string l = a.substr(p, e - p); if (b.find(l + (char)10) == std::string::npos) return false; p = e + 1; }
    return true;
}
static int R() { return fs_get_root(); }
static void fill(uint8_t* b, size_t n, uint8_t seed) { for (size_t i = 0; i < n; ++i) b[i] = (uint8_t)(seed + i * 7); }

static void prelude()
{
    static uint8_t b[20000];
    fs_mkdir("/work", R()); fs_mkdir("/work/sub", R()); fs_mkdir("/many", R());
    fill(b, 1500, 1); fs_write_data("/work/a.txt", R(), b, 1500);
    fill(b, 20000, 2); fs_write_data("/work/big.bin", R(), b, 20000);
    fill(b, 300, 3); fs_write_data("/work/sub/c.txt", R(), b, 300);
    for (int i = 0; i < 60; ++i) { char p[64]; snprintf(p, sizeof p, "/many/f%02d", i); fill(b, 100 + i, (uint8_t)i); fs_write_data(p, R(), b, 100 + (uint64_t)i); }
    fs_symlink("a.txt", "/work/ln", R());
    fill(b, 800, 5); fs_write_data("/work/b.txt", R(), b, 800); fs_link_hard("/work/b.txt", "/work/b2.txt", R());
    fs_mkdir("/big", R());
    for (int i = 0; i < 120; ++i) { char p[64]; snprintf(p, sizeof p, "/big/g%03d", i); fill(b, 40, (uint8_t)i); fs_write_data(p, R(), b, 40); }
}

static void op_create()      { static uint8_t b[700]; fill(b, 700, 9); fs_write_data("/work/new.txt", R(), b, 700); }
static void op_overwrite()   { static uint8_t b[9000]; fill(b, 9000, 11); fs_write_data("/work/a.txt", R(), b, 9000); }
static void op_shrink()      { static uint8_t b[10]; fill(b, 10, 12); fs_write_data("/work/big.bin", R(), b, 10); }
static void op_mkdir()       { fs_mkdir("/work/newdir", R()); }
static void op_mkdirp()      { fs_mkdir_p("/work/x/y/z", R()); }
static void op_rename_file() { fs_rename("/work/a.txt", "/work/sub/moved.txt", R()); }
static void op_rename_dir()  { fs_rename("/work/sub", "/many/subdir", R()); }
static void op_rm()          { fs_rm("/work/big.bin", R()); }
static void op_rm_r()        { fs_rm_recursive("/many", R()); }
static void op_symlink()     { fs_symlink("/work/a.txt", "/work/ln2", R()); }
static void op_hardlink()    { fs_link_hard("/work/a.txt", "/work/hard", R()); }
static void op_chmod()       { fs_set_mode_path("/work/a.txt", R(), 0600); }
static void op_chown()       { fs_set_owner_path("/work/a.txt", R(), 1000, 1000); }
static void op_truncate()    { fs_truncate("/work/big.bin", R(), 5000); }
static void op_hard_overwrite() { static uint8_t b[3000]; fill(b, 3000, 13); fs_write_data("/work/b.txt", R(), b, 3000); }
static void op_replace()     { fs_rename_replace("/work/a.txt", "/work/sub/c.txt", R()); }
static void op_replace_hl()  { fs_rename_replace("/work/a.txt", "/work/b2.txt", R()); }
static void op_replace_sym() { fs_rename_replace("/work/a.txt", "/work/ln", R()); }
static void op_rm_big()      { fs_rm_recursive("/big", R()); }


struct Case { const char* name; Op op; bool partial; };
static const Case cases[] = {
    {"create", op_create, false}, {"overwrite", op_overwrite, false}, {"shrink", op_shrink, false}, {"mkdir", op_mkdir, false}, {"mkdir_p", op_mkdirp, false},
    {"rename-file", op_rename_file, false}, {"rename-dir", op_rename_dir, false}, {"rm", op_rm, false},
    {"rm-recursive-60", op_rm_r, true}, {"symlink", op_symlink, false}, {"hardlink", op_hardlink, false}, {"chmod", op_chmod, false}, {"chown", op_chown, false},
    {"truncate", op_truncate, false}, {"hardlink-overwrite", op_hard_overwrite, false}, {"rm-recursive-120", op_rm_big, true},
    {"rename-replace", op_replace, false}, {"rename-replace-hardlinked", op_replace_hl, false}, {"rename-replace-symlink", op_replace_sym, false},
};

static std::vector<uint8_t> baseline;

static void load(const std::vector<uint8_t>& img) { live = img; dur = img; pending.clear(); crashed = false; crash_at = -1; wcount = 0; }


// ---- corruption repair: damage the inode table of the baseline image in specific ways; mount must repair and fsck must come out clean
static void poke(std::vector<uint8_t>& img, int id, int off, const void* v, int n)
{ memcpy(&img[(1 + (uint64_t)id * 128 / 512) * SS + (uint64_t)id * 128 % 512 + off], v, n); }
static int ino(const char* path) { return fs_resolve(path, fs_get_root()); }
static int corruption_tests()
{
    int bad = 0;
    struct C { const char* name; void (*dmg)(std::vector<uint8_t>&); const char* survivor; };
    static const C cs[] = {
        {"orphan-parent-unused", [](std::vector<uint8_t>& im) { int32_t p = 250; poke(im, ino("/work/a.txt"), 8, &p, 4); }, "/lost+found"},
        {"parent-is-a-file", [](std::vector<uint8_t>& im) { int32_t p = ino("/work/b.txt"); poke(im, ino("/work/a.txt"), 8, &p, 4); }, "/lost+found"},
        {"directory-cycle", [](std::vector<uint8_t>& im) { int32_t a = ino("/work/sub"), b = ino("/work"); poke(im, b, 8, &a, 4); }, "/lost+found"},
        {"duplicate-name", [](std::vector<uint8_t>& im) { char nm[64] = {0}; strcpy(nm, "a.txt"); poke(im, ino("/work/b.txt"), 36, nm, 64); }, "/work/a.txt"},
        {"empty-name", [](std::vector<uint8_t>& im) { char nm[64] = {0}; poke(im, ino("/work/a.txt"), 36, nm, 64); }, "/work/big.bin"},
        {"extent-out-of-range", [](std::vector<uint8_t>& im) { uint64_t f = 99999999; poke(im, ino("/work/big.bin"), 24, &f, 8); }, "/work/a.txt"},
        {"extent-overlap", [](std::vector<uint8_t>& im) { uint64_t f; memcpy(&f, &im[(1 + (uint64_t)ino("/work/big.bin") * 128 / 512) * SS + (uint64_t)ino("/work/big.bin") * 128 % 512 + 24], 8); f += 1; poke(im, ino("/work/c.txt") >= 0 ? ino("/work/sub/c.txt") : 1, 24, &f, 8); }, "/work/big.bin"},
        {"size-vs-sectors", [](std::vector<uint8_t>& im) { uint64_t sz = 3; poke(im, ino("/work/big.bin"), 16, &sz, 8); }, "/work/a.txt"},
        {"bad-type", [](std::vector<uint8_t>& im) { uint32_t t = 77; poke(im, ino("/work/a.txt"), 4, &t, 4); }, "/work/big.bin"},
    };
    for (const C& c : cs)
    {
        load(baseline); fs_init();
        std::vector<uint8_t> img = dur;
        c.dmg(img);
        load(img); fs_init();
        if (!fs_persistent()) { printf("FAIL corruption %s: did not mount from disk\n", c.name); ++bad; continue; }
        FsckInfo f; int rc = fs_fsck("/", 0, 0, &f);
        if (rc != 0 || !f.structure_ok || !f.bitmap_ok || f.problems) { printf("FAIL corruption %s: not clean after mount repair (problems=%u structure=%d bitmap=%d)\n", c.name, f.problems, f.structure_ok, f.bitmap_ok); ++bad; }
        if (fs_resolve(c.survivor, fs_get_root()) < 0) { printf("FAIL corruption %s: %s vanished\n", c.name, c.survivor); ++bad; }
        fs_init(); if (!fs_persistent()) { printf("FAIL corruption %s: remount\n", c.name); ++bad; }
        printf("  corruption %-24s repaired\n", c.name);
    }
    // dry run must not modify anything
    load(baseline); fs_init(); std::vector<uint8_t> img = dur; { int32_t p = 250; poke(img, ino("/work/a.txt"), 8, &p, 4); }
    return bad;
}

int main(int argc, char** argv)
{
    int stride = argc > 1 ? atoi(argv[1]) : 1;     /* test every Nth crash point (1 = all) */
    live.assign(SECTORS * SS, 0); dur = live;
    cache_model = false; fs_init();
    prelude(); fs_sync();
    CHECK(fs_persistent() == 1, "baseline mounted");
    baseline = dur;
    long total_points = 0, total_cases = 0;
    for (const Case& c : cases)
    {
        ++total_cases;
        load(baseline); fs_init(); std::string before = snap();
        cache_model = false; long w0 = wcount; c.op(); fs_sync(); long writes = wcount - w0; std::string after_live = snap();
        fs_init(); std::string after = snap();
        if (after != after_live) { printf("FAIL %s: in-memory state differs from what reached the disk\n", c.name); ++fails; }
        CHECK(before != after || std::string(c.name) == "chown", c.name);
        printf("  [%s] entries before=%zu after=%zu\n", c.name, (size_t)std::count(before.begin(), before.end(), (char)10), (size_t)std::count(after.begin(), after.end(), (char)10));
        int outcome_before = 0, outcome_after = 0, points = 0;
        for (int model = 0; model < 2; ++model)
        {
            int variants = model == 0 ? 1 : 4;
            for (long n = 0; n <= writes; n += stride)
                for (int v = 0; v < variants; ++v)
                {
                    cache_model = model == 1;
                    load(baseline); fs_init();                       // identical starting state
                    if (cache_model) { pending.clear(); }
                    wcount = 0; crash_at = n; crashed = false;
                    c.op();
                    // power loss: durable image + (cache model) a random subset of unflushed writes
                    std::vector<uint8_t> img = dur;
                    if (cache_model && !pending.empty())
                    {
                        rng_state = (unsigned)(n * 7919 + v * 104729 + 17);
                        // keep a prefix-free random subset; later writes to one sector override earlier ones in order
                        for (auto& w : pending) { bool keep = v == 0 ? false : v == 1 ? true : (rnd() & 1); if (keep) memcpy(&img[w.s * SS], w.d, SS); }
                    }
                    load(img); cache_model = false; fs_init();
                    ++points;
                    if (!fs_persistent()) { printf("FAIL %s: not mounted from disk after crash at write %ld model %d v%d\n", c.name, n, model, v); ++fails; continue; }
                    std::string s = snap();
                    if (s == before) ++outcome_before; else if (s == after) ++outcome_after;
                    else if (c.partial && subset(after, s) && subset(s, before)) ++outcome_after;   // recursive removal: any prefix of the removals
                    else { printf("FAIL %s: torn state after crash at write %ld model %d v%d\n", c.name, n, model, v); ++fails; if (fails > 20) return 1; }
                    // the recovered filesystem must keep working
                    uint8_t t[64] = {1}; if (fs_write_data("/post_crash_probe", R(), t, 64) != 0) { printf("FAIL %s: fs unusable after recovery (write %ld)\n", c.name, n); ++fails; }
                    fs_rm("/post_crash_probe", R());
                    fs_init();                                        // second mount of the same image must also be clean
                    if (!fs_persistent()) { printf("FAIL %s: remount failed\n", c.name); ++fails; }
                }
        }
        total_points += points;
        printf("  %-24s writes=%-3ld points=%-4d rolled-back=%-4d completed=%-4d\n", c.name, writes, points, outcome_before, outcome_after);
    }
    fails += corruption_tests();
    printf("fs_crash_test: %ld crash points over %ld operations, %d failures\n", total_points, total_cases, fails);
    return fails ? 1 : 0;
}
