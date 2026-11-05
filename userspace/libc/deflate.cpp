/* RioOS deflate/inflate/gzip/CRC32 (RFC 1951, RFC 1952). Compiles for the user libc and on the host (tests/deflate_host_test.cpp).
   Compressor: LZ77 with hash chains + lazy matching, per-block choice of stored / fixed-Huffman / dynamic-Huffman. */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long size_t_;

extern "C" {
void* malloc(unsigned long);
void* realloc(void*, unsigned long);
void free(void*);
void* memcpy(void*, const void*, unsigned long);
void* memset(void*, int, unsigned long);

/* ---------------- CRC32 ---------------- */
static u32 crc_table[256];
static int crc_ready = 0;
u32 rio_crc32(u32 crc, const u8* p, unsigned long n)
{
    if (!crc_ready)
    {
        for (u32 i = 0; i < 256; ++i) { u32 c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_table[i] = c; }
        crc_ready = 1;
    }
    crc = ~crc;
    for (unsigned long i = 0; i < n; ++i) crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---------------- output buffer ---------------- */
struct OutBuf { u8* p; unsigned long len, cap; int err; };
static void ob_put(OutBuf* o, u8 b)
{
    if (o->len == o->cap)
    {
        unsigned long nc = o->cap ? o->cap * 2 : 4096;
        u8* np = (u8*)realloc(o->p, nc);
        if (!np) { o->err = 1; return; }
        o->p = np; o->cap = nc;
    }
    o->p[o->len++] = b;
}

/* ---------------- inflate ---------------- */
struct InState { const u8* in; unsigned long len, pos; u32 bitbuf; int bitcnt; OutBuf* out; int err; };
static int in_bits(InState* s, int need)
{
    while (s->bitcnt < need)
    {
        if (s->pos >= s->len) { s->err = 1; return 0; }
        s->bitbuf |= (u32)s->in[s->pos++] << s->bitcnt; s->bitcnt += 8;
    }
    int v = (int)(s->bitbuf & ((1u << need) - 1));
    s->bitbuf >>= need; s->bitcnt -= need;
    return v;
}
struct Huff { u16 count[16]; u16 symbol[320]; };
static int huff_build(Huff* h, const u8* lengths, int n)
{
    for (int i = 0; i < 16; ++i) h->count[i] = 0;
    for (int i = 0; i < n; ++i) h->count[lengths[i]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; ++len) { left <<= 1; left -= h->count[len]; if (left < 0) return left; }
    u16 offs[16]; offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = (u16)(offs[len] + h->count[len]);
    for (int i = 0; i < n; ++i) if (lengths[i]) h->symbol[offs[lengths[i]]++] = (u16)i;
    return left;
}
static int huff_decode(InState* s, const Huff* h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len)
    {
        code |= in_bits(s, 1); if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}
static const u16 len_base[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const u8 len_extra[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const u16 dist_base[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const u8 dist_extra[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static int inflate_codes(InState* s, const Huff* lit, const Huff* dist)
{
    for (;;)
    {
        int sym = huff_decode(s, lit); if (sym < 0) return -1;
        if (sym < 256) { ob_put(s->out, (u8)sym); if (s->out->err) return -1; }
        else if (sym == 256) return 0;
        else
        {
            sym -= 257; if (sym >= 29) return -1;
            int len = len_base[sym] + in_bits(s, len_extra[sym]); if (s->err) return -1;
            int ds = huff_decode(s, dist); if (ds < 0 || ds >= 30) return -1;
            unsigned long d = (unsigned long)dist_base[ds] + (unsigned long)in_bits(s, dist_extra[ds]); if (s->err) return -1;
            if (d > s->out->len) return -1;
            for (int i = 0; i < len; ++i) { ob_put(s->out, s->out->p[s->out->len - d]); if (s->out->err) return -1; }
        }
    }
}

int rio_inflate(const u8* in, unsigned long inlen, u8** out, unsigned long* outlen, unsigned long* consumed)
{
    InState st; st.in = in; st.len = inlen; st.pos = 0; st.bitbuf = 0; st.bitcnt = 0; st.err = 0;
    OutBuf ob = { 0, 0, 0, 0 }; st.out = &ob;
    static Huff lit, dist;
    int last;
    do
    {
        last = in_bits(&st, 1); int type = in_bits(&st, 2); if (st.err) goto fail;
        if (type == 0)
        {
            st.bitbuf = 0; st.bitcnt = 0;
            if (st.pos + 4 > st.len) goto fail;
            unsigned len = st.in[st.pos] | (st.in[st.pos + 1] << 8), nlen = st.in[st.pos + 2] | (st.in[st.pos + 3] << 8);
            st.pos += 4;
            if ((len ^ 0xFFFFu) != nlen || st.pos + len > st.len) goto fail;
            for (unsigned i = 0; i < len; ++i) ob_put(&ob, st.in[st.pos + i]);
            if (ob.err) goto fail;
            st.pos += len;
        }
        else if (type == 1)
        {
            u8 l[288]; int i = 0;
            for (; i < 144; ++i) l[i] = 8; for (; i < 256; ++i) l[i] = 9; for (; i < 280; ++i) l[i] = 7; for (; i < 288; ++i) l[i] = 8;
            huff_build(&lit, l, 288);
            u8 dl[30]; for (i = 0; i < 30; ++i) dl[i] = 5; huff_build(&dist, dl, 30);
            if (inflate_codes(&st, &lit, &dist) != 0) goto fail;
        }
        else if (type == 2)
        {
            int nlen = in_bits(&st, 5) + 257, ndist = in_bits(&st, 5) + 1, ncode = in_bits(&st, 4) + 4;
            if (st.err || nlen > 286 || ndist > 30) goto fail;
            static const u8 order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            u8 lengths[320]; for (int i = 0; i < 19; ++i) lengths[i] = 0;
            for (int i = 0; i < ncode; ++i) lengths[order[i]] = (u8)in_bits(&st, 3);
            if (st.err) goto fail;
            Huff cl; if (huff_build(&cl, lengths, 19) != 0) goto fail;
            int idx = 0;
            while (idx < nlen + ndist)
            {
                int sym = huff_decode(&st, &cl); if (sym < 0) goto fail;
                if (sym < 16) lengths[idx++] = (u8)sym;
                else
                {
                    int rep, val = 0;
                    if (sym == 16) { if (!idx) goto fail; val = lengths[idx - 1]; rep = 3 + in_bits(&st, 2); }
                    else if (sym == 17) rep = 3 + in_bits(&st, 3); else rep = 11 + in_bits(&st, 7);
                    if (st.err || idx + rep > nlen + ndist) goto fail;
                    while (rep--) lengths[idx++] = (u8)val;
                }
            }
            if (lengths[256] == 0) goto fail;
            int e = huff_build(&lit, lengths, nlen); if (e < 0 || (e > 0 && nlen - lit.count[0] != 1)) goto fail;
            e = huff_build(&dist, lengths + nlen, ndist); if (e < 0 || (e > 0 && ndist - dist.count[0] != 1)) goto fail;
            if (inflate_codes(&st, &lit, &dist) != 0) goto fail;
        }
        else goto fail;
    } while (!last);
    *out = ob.p; *outlen = ob.len; if (consumed) *consumed = st.pos;
    return 0;
fail:
    free(ob.p); *out = 0; *outlen = 0;
    return -1;
}

/* ---------------- deflate ---------------- */
#define WSIZE 32768
#define HBITS 15
#define HSIZE (1 << HBITS)
#define MAXTOK 32768

struct Tok { u16 len; u16 dist; };    /* len==0: literal in dist */
struct BitW { OutBuf* o; u32 buf; int cnt; };
static void bw_put(BitW* w, u32 v, int n) { w->buf |= v << w->cnt; w->cnt += n; while (w->cnt >= 8) { ob_put(w->o, (u8)w->buf); w->buf >>= 8; w->cnt -= 8; } }
static void bw_flush(BitW* w) { if (w->cnt > 0) { ob_put(w->o, (u8)w->buf); w->buf = 0; w->cnt = 0; } }
static u32 rev_bits(u32 code, int n) { u32 r = 0; for (int i = 0; i < n; ++i) { r = (r << 1) | (code & 1); code >>= 1; } return r; }

static int len_code(int len) { int c = 28; while (len < len_base[c]) --c; return c; }
static int dist_code(int d) { int c = 29; while (d < dist_base[c]) --c; return c; }

/* Build length-limited Huffman code lengths (max 15 / 7) from frequencies. */
static void build_lengths(const u32* freq, int n, int maxbits, u8* lens)
{
    for (int i = 0; i < n; ++i) lens[i] = 0;
    int nodes_cap = 2 * n + 1;
    u32* w = (u32*)malloc(sizeof(u32) * nodes_cap); int* parent = (int*)malloc(sizeof(int) * nodes_cap); int* heap = (int*)malloc(sizeof(int) * nodes_cap);
    int used = 0, hn = 0;
    for (int i = 0; i < n; ++i) if (freq[i]) { w[i] = freq[i]; parent[i] = -1; heap[hn++] = i; ++used; }
    if (used == 0) { free(w); free(parent); free(heap); return; }
    if (used == 1) { lens[heap[0]] = 1; free(w); free(parent); free(heap); return; }
    int next = n;
    /* simple O(n^2) selection is fine: n <= 286 */
    while (hn > 1)
    {
        int a = 0, b = 1; if (w[heap[b]] < w[heap[a]]) { int t = a; a = b; b = t; }
        for (int i = 2; i < hn; ++i)
        {
            if (w[heap[i]] < w[heap[a]]) { b = a; a = i; }
            else if (w[heap[i]] < w[heap[b]]) b = i;
        }
        int na = heap[a], nb = heap[b];
        w[next] = w[na] + w[nb]; parent[next] = -1; parent[na] = next; parent[nb] = next;
        int hi = a > b ? a : b, lo = a > b ? b : a;
        heap[hi] = heap[hn - 1]; --hn;
        heap[lo] = next;
        ++next;
    }
    int count[32]; for (int i = 0; i < 32; ++i) count[i] = 0;
    int overflow = 0;
    for (int i = 0; i < n; ++i)
        if (freq[i])
        {
            int d = 0, p = i; while (parent[p] != -1) { p = parent[p]; ++d; }
            if (d > maxbits) { d = maxbits; ++overflow; }
            lens[i] = (u8)d; count[d]++;
        }
    /* repair the Kraft inequality after clamping (as zlib does) */
    while (overflow > 0)
    {
        int bits = maxbits - 1; while (count[bits] == 0) --bits;
        count[bits]--; count[bits + 1] += 2; count[maxbits]--; overflow -= 2;
    }
    /* reassign lengths: most frequent symbols get the shortest codes */
    int idx[320]; int m = 0;
    for (int i = 0; i < n; ++i) if (freq[i]) idx[m++] = i;
    for (int i = 1; i < m; ++i) { int v = idx[i], j = i - 1; while (j >= 0 && (freq[idx[j]] < freq[v] || (freq[idx[j]] == freq[v] && idx[j] > v))) { idx[j + 1] = idx[j]; --j; } idx[j + 1] = v; }
    int k = 0;
    for (int bits = 1; bits <= maxbits; ++bits) for (int c = 0; c < count[bits]; ++c) lens[idx[k++]] = (u8)bits;
    free(w); free(parent); free(heap);
}
static void canon_codes(const u8* lens, int n, u16* codes)
{
    int bl_count[16]; for (int i = 0; i < 16; ++i) bl_count[i] = 0;
    for (int i = 0; i < n; ++i) bl_count[lens[i]]++;
    bl_count[0] = 0;
    int next[16]; int code = 0;
    for (int b = 1; b < 16; ++b) { code = (code + bl_count[b - 1]) << 1; next[b] = code; }
    for (int i = 0; i < n; ++i) if (lens[i]) codes[i] = (u16)rev_bits((u32)next[lens[i]]++, lens[i]);
}

static void put_block(BitW* w, const u8* data, const Tok* toks, int nt, unsigned long start, unsigned long end, int final)
{
    u32 lf[286], df[30];
    for (int i = 0; i < 286; ++i) lf[i] = 0; for (int i = 0; i < 30; ++i) df[i] = 0;
    for (int i = 0; i < nt; ++i)
    {
        if (toks[i].len == 0) lf[toks[i].dist]++;
        else { lf[257 + len_code(toks[i].len)]++; df[dist_code(toks[i].dist)]++; }
    }
    lf[256] = 1;
    u8 ll[286], dl[30];
    build_lengths(lf, 286, 15, ll); build_lengths(df, 30, 15, dl);
    int dused = 0; for (int i = 0; i < 30; ++i) if (dl[i]) ++dused;
    if (dused == 0) dl[0] = 1;
    int hlit = 286; while (hlit > 257 && ll[hlit - 1] == 0) --hlit;
    int hdist = 30; while (hdist > 1 && dl[hdist - 1] == 0) --hdist;
    /* run-length encode the code lengths */
    u8 all[320]; int na = 0;
    for (int i = 0; i < hlit; ++i) all[na++] = ll[i]; for (int i = 0; i < hdist; ++i) all[na++] = dl[i];
    u8 rle_sym[320], rle_ext[320]; int nr = 0;
    for (int i = 0; i < na;)
    {
        int v = all[i], j = i; while (j < na && all[j] == v) ++j;
        int run = j - i;
        if (v == 0)
        {
            while (run >= 11) { int r = run > 138 ? 138 : run; rle_sym[nr] = 18; rle_ext[nr++] = (u8)(r - 11); run -= r; }
            if (run >= 3) { rle_sym[nr] = 17; rle_ext[nr++] = (u8)(run - 3); run = 0; }
            while (run > 0) { rle_sym[nr] = 0; rle_ext[nr++] = 0; --run; }
        }
        else
        {
            rle_sym[nr] = (u8)v; rle_ext[nr++] = 0; --run;
            while (run >= 3) { int r = run > 6 ? 6 : run; rle_sym[nr] = 16; rle_ext[nr++] = (u8)(r - 3); run -= r; }
            while (run > 0) { rle_sym[nr] = (u8)v; rle_ext[nr++] = 0; --run; }
        }
        i = j;
    }
    u32 cf[19]; for (int i = 0; i < 19; ++i) cf[i] = 0; for (int i = 0; i < nr; ++i) cf[rle_sym[i]]++;
    u8 cl[19]; build_lengths(cf, 19, 7, cl);
    static const u8 order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    int hclen = 19; while (hclen > 4 && cl[order[hclen - 1]] == 0) --hclen;
    /* sizes in bits */
    unsigned long dyn = 3 + 14 + 3UL * hclen, fixed = 3;
    for (int i = 0; i < nr; ++i) { dyn += cl[rle_sym[i]]; if (rle_sym[i] == 16) dyn += 2; else if (rle_sym[i] == 17) dyn += 3; else if (rle_sym[i] == 18) dyn += 7; }
    for (int i = 0; i < 286; ++i)
    {
        if (!lf[i]) continue;
        int ex = i >= 257 ? len_extra[i - 257] : 0;
        dyn += (unsigned long)lf[i] * (ll[i] + ex);
        int fl = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        fixed += (unsigned long)lf[i] * (fl + ex);
    }
    for (int i = 0; i < 30; ++i) { if (!df[i]) continue; dyn += (unsigned long)df[i] * (dl[i] + dist_extra[i]); fixed += (unsigned long)df[i] * (5 + dist_extra[i]); }
    unsigned long stored = 8UL * (end - start) + 40;
    if (stored < dyn && stored < fixed)
    {
        unsigned long pos = start;
        while (pos < end || start == end)
        {
            unsigned long n = end - pos; if (n > 65535) n = 65535;
            int last = final && (pos + n == end);
            bw_put(w, (u32)last, 1); bw_put(w, 0, 2); bw_flush(w);
            ob_put(w->o, (u8)(n & 255)); ob_put(w->o, (u8)(n >> 8)); ob_put(w->o, (u8)(~n & 255)); ob_put(w->o, (u8)((~n >> 8) & 255));
            for (unsigned long i = 0; i < n; ++i) ob_put(w->o, data[pos + i]);
            pos += n; if (pos >= end) break;
        }
        return;
    }
    u8 flens[288], fdl[30]; u16 lc[288], dc[30];
    const u8* L; const u8* D; int use_dyn = dyn <= fixed;
    if (use_dyn) { L = ll; D = dl; canon_codes(ll, 286, lc); canon_codes(dl, 30, dc); }
    else
    {
        int i = 0; for (; i < 144; ++i) flens[i] = 8; for (; i < 256; ++i) flens[i] = 9; for (; i < 280; ++i) flens[i] = 7; for (; i < 288; ++i) flens[i] = 8;
        for (i = 0; i < 30; ++i) fdl[i] = 5;
        canon_codes(flens, 288, lc); canon_codes(fdl, 30, dc); L = flens; D = fdl;
    }
    bw_put(w, (u32)final, 1); bw_put(w, use_dyn ? 2 : 1, 2);
    if (use_dyn)
    {
        bw_put(w, (u32)(hlit - 257), 5); bw_put(w, (u32)(hdist - 1), 5); bw_put(w, (u32)(hclen - 4), 4);
        for (int i = 0; i < hclen; ++i) bw_put(w, cl[order[i]], 3);
        u16 cc[19]; canon_codes(cl, 19, cc);
        for (int i = 0; i < nr; ++i)
        {
            bw_put(w, cc[rle_sym[i]], cl[rle_sym[i]]);
            if (rle_sym[i] == 16) bw_put(w, rle_ext[i], 2); else if (rle_sym[i] == 17) bw_put(w, rle_ext[i], 3); else if (rle_sym[i] == 18) bw_put(w, rle_ext[i], 7);
        }
    }
    for (int i = 0; i < nt; ++i)
    {
        if (toks[i].len == 0) bw_put(w, lc[toks[i].dist], L[toks[i].dist]);
        else
        {
            int c = len_code(toks[i].len); bw_put(w, lc[257 + c], L[257 + c]);
            if (len_extra[c]) bw_put(w, (u32)(toks[i].len - len_base[c]), len_extra[c]);
            int d = dist_code(toks[i].dist); bw_put(w, dc[d], D[d]);
            if (dist_extra[d]) bw_put(w, (u32)(toks[i].dist - dist_base[d]), dist_extra[d]);
        }
    }
    bw_put(w, lc[256], L[256]);
}

int rio_deflate(const u8* in, unsigned long inlen, u8** out, unsigned long* outlen, int level)
{
    if (level < 1) level = 1; if (level > 9) level = 9;
    static const int chain_tab[10] = { 0, 4, 8, 16, 32, 64, 128, 256, 512, 1024 };
    int max_chain = chain_tab[level]; int lazy = level >= 4;
    OutBuf ob = { 0, 0, 0, 0 }; BitW w = { &ob, 0, 0 };
    int* head = (int*)malloc(sizeof(int) * HSIZE); int* prev = (int*)malloc(sizeof(int) * WSIZE); Tok* toks = (Tok*)malloc(sizeof(Tok) * MAXTOK);
    if (!head || !prev || !toks) { free(head); free(prev); free(toks); return -1; }
    for (int i = 0; i < HSIZE; ++i) head[i] = -1;
    unsigned long pos = 0, bstart = 0; int nt = 0;
    if (inlen == 0) { bw_put(&w, 1, 1); bw_put(&w, 1, 2); bw_put(&w, 0, 7); }
    #define HASH(p) ((((u32)in[p] << 10) ^ ((u32)in[(p) + 1] << 5) ^ (u32)in[(p) + 2]) & (HSIZE - 1))
    #define INSERT(p) do { if ((p) + 2 < inlen) { u32 h_ = HASH(p); prev[(p) & (WSIZE - 1)] = head[h_]; head[h_] = (int)(p); } } while (0)
    auto find = [&](unsigned long p, int* bl, int* bd)
    {
        *bl = 0; *bd = 0;
        if (p + 2 >= inlen) return;
        u32 h = HASH(p); int cand = head[h]; int chain = max_chain;
        unsigned long maxlen = inlen - p; if (maxlen > 258) maxlen = 258;
        while (cand >= 0 && chain-- > 0)
        {
            unsigned long d = p - (unsigned long)cand;
            if (d > WSIZE || d == 0) break;
            if (in[cand + *bl] == in[p + *bl] || *bl == 0)
            {
                unsigned long l = 0; while (l < maxlen && in[cand + l] == in[p + l]) ++l;
                if ((int)l > *bl && l >= 3) { *bl = (int)l; *bd = (int)d; if (l == maxlen) break; }
            }
            int nx = prev[(unsigned long)cand & (WSIZE - 1)];
            if (nx >= cand) break;
            cand = nx;
        }
    };
    unsigned long ins = 0;
    #define INSERT_TO(limit) do { while (ins < (limit)) { INSERT(ins); ++ins; } } while (0)
    while (pos < inlen)
    {
        int bl, bd; INSERT_TO(pos); find(pos, &bl, &bd);
        if (bl >= 3 && lazy && pos + 1 < inlen)
        {
            INSERT_TO(pos + 1);
            int l2, d2; find(pos + 1, &l2, &d2);
            if (l2 > bl) { toks[nt].len = 0; toks[nt].dist = in[pos]; ++nt; ++pos; goto check_block; }
        }
        if (bl >= 3) { toks[nt].len = (u16)bl; toks[nt].dist = (u16)bd; ++nt; pos += (unsigned long)bl; }
        else { toks[nt].len = 0; toks[nt].dist = in[pos]; ++nt; ++pos; }
    check_block:
        if (nt >= MAXTOK - 2) { put_block(&w, in, toks, nt, bstart, pos, 0); nt = 0; bstart = pos; }
    }
    if (inlen) put_block(&w, in, toks, nt, bstart, pos, 1);
    bw_flush(&w);
    free(head); free(prev); free(toks);
    if (ob.err) { free(ob.p); return -1; }
    *out = ob.p; *outlen = ob.len;
    return 0;
    #undef HASH
    #undef INSERT
}

/* ---------------- gzip container ---------------- */
int rio_gzip_compress(const u8* in, unsigned long inlen, const char* name, u32 mtime, u8** out, unsigned long* outlen, int level)
{
    u8* body; unsigned long bl;
    if (rio_deflate(in, inlen, &body, &bl, level) != 0) return -1;
    int nl = 0; if (name) while (name[nl]) ++nl;
    unsigned long total = 10 + (name ? (unsigned long)nl + 1 : 0) + bl + 8;
    u8* o = (u8*)malloc(total); if (!o) { free(body); return -1; }
    unsigned long p = 0;
    o[p++] = 0x1f; o[p++] = 0x8b; o[p++] = 8; o[p++] = name ? 8 : 0;
    o[p++] = (u8)mtime; o[p++] = (u8)(mtime >> 8); o[p++] = (u8)(mtime >> 16); o[p++] = (u8)(mtime >> 24);
    o[p++] = level >= 9 ? 2 : (level == 1 ? 4 : 0); o[p++] = 3;
    if (name) { for (int i = 0; i < nl; ++i) o[p++] = (u8)name[i]; o[p++] = 0; }
    memcpy(o + p, body, bl); p += bl; free(body);
    u32 crc = rio_crc32(0, in, inlen);
    for (int i = 0; i < 4; ++i) o[p++] = (u8)(crc >> (8 * i));
    for (int i = 0; i < 4; ++i) o[p++] = (u8)((u32)inlen >> (8 * i));
    *out = o; *outlen = p;
    return 0;
}

/* returns 0 ok, -1 not gzip, -2 corrupt data, -3 crc/size mismatch. Concatenated members are decoded in sequence. */
int rio_gzip_decompress(const u8* in, unsigned long inlen, u8** out, unsigned long* outlen, char* name_out, int name_cap)
{
    OutBuf all = { 0, 0, 0, 0 }; unsigned long pos = 0; int first = 1;
    while (pos < inlen)
    {
        if (inlen - pos < 18 || in[pos] != 0x1f || in[pos + 1] != 0x8b || in[pos + 2] != 8) { if (first) { free(all.p); return -1; } break; }
        u8 flg = in[pos + 3]; unsigned long p = pos + 10;
        if (flg & 4) { if (p + 2 > inlen) { free(all.p); return -2; } p += 2 + (in[p] | (in[p + 1] << 8)); }
        if (flg & 8) { int k = 0; while (p < inlen && in[p]) { if (first && name_out && k < name_cap - 1) name_out[k++] = (char)in[p]; ++p; } if (first && name_out && name_cap) name_out[k] = 0; ++p; }
        else if (first && name_out && name_cap) name_out[0] = 0;
        if (flg & 16) { while (p < inlen && in[p]) ++p; ++p; }
        if (flg & 2) p += 2;
        if (p >= inlen) { free(all.p); return -2; }
        u8* buf; unsigned long bl, used;
        if (rio_inflate(in + p, inlen - p, &buf, &bl, &used) != 0) { free(all.p); return -2; }
        p += used;
        if (p + 8 > inlen) { free(buf); free(all.p); return -2; }
        u32 crc = in[p] | (in[p + 1] << 8) | (in[p + 2] << 16) | ((u32)in[p + 3] << 24);
        u32 isz = in[p + 4] | (in[p + 5] << 8) | (in[p + 6] << 16) | ((u32)in[p + 7] << 24);
        if (crc != rio_crc32(0, buf, bl) || isz != (u32)bl) { free(buf); free(all.p); return -3; }
        for (unsigned long i = 0; i < bl; ++i) ob_put(&all, buf[i]);
        free(buf);
        if (all.err) { free(all.p); return -2; }
        pos = p + 8; first = 0;
    }
    if (!all.p) { all.p = (u8*)malloc(1); }
    *out = all.p; *outlen = all.len;
    return 0;
}

}  // extern "C"
