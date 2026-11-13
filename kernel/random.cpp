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

#include "random.h"
#include "sha256.h"
#include "fs.h"

#ifdef HOST_TEST
static inline uint64_t cycle_counter() { static uint64_t c = 1; return c += 7919; }
static uint64_t now_ms() { return 0; }
#else
#include "timer.h"
static inline uint64_t cycle_counter() { uint64_t v; asm volatile("mrs %0, cntvct_el0" : "=r"(v)); return v; }
static uint64_t now_ms() { return timer_millis(); }
#endif

static uint8_t key[32];                 /* DRBG state */
static uint64_t out_counter = 0;
static uint64_t pend[8];                /* IRQ-written accumulator, folded into key on the next draw */
static volatile uint32_t pend_idx = 0;
static volatile uint32_t events = 0;
static uint32_t entropy_bits = 0;
static int hw_rng = 0, seed_loaded = 0;
static uint64_t bytes_out = 0;
static uint32_t folded_events = 0;

static void mix_in(const void* data, uint32_t n)
{
    uint8_t buf[32 + 8 + 256]; uint32_t k = 0;
    for (; k < 32; ++k) buf[k] = key[k];
    uint64_t c = cycle_counter(); for (int i = 0; i < 8; ++i) buf[k++] = (uint8_t)(c >> (8 * i));
    const uint8_t* d = (const uint8_t*)data;
    for (uint32_t i = 0; i < n && k < sizeof(buf); ++i) buf[k++] = d[i];
    sha256(buf, k, key);
}

static void fold_pending()
{
    uint64_t snap[8];
    for (int i = 0; i < 8; ++i) { snap[i] = pend[i]; pend[i] = 0; }
    uint32_t ev = events;
    uint32_t add = ev - folded_events; folded_events = ev;
    mix_in(snap, sizeof(snap));
    /* Interrupt timing jitter is credited conservatively: one bit per event, at most 128 per fold. */
    if (add > 128U) add = 128U;
    if (entropy_bits < 256U) entropy_bits = entropy_bits + add > 256U ? 256U : entropy_bits + add;
}

void random_add_event()
{
    uint64_t v = cycle_counter();
    uint32_t i = pend_idx; pend_idx = (i + 1U) & 7U;
    pend[i & 7U] = ((pend[i & 7U] << 13) | (pend[i & 7U] >> 51)) ^ v ^ (v << 32);
    ++events;
}

void random_add_bytes(const void* data, uint32_t n, uint32_t bits)
{
    mix_in(data, n);
    if (entropy_bits < 256U) entropy_bits = entropy_bits + bits > 256U ? 256U : entropy_bits + bits;
}

void random_init()
{
    uint8_t seed[64]; uint32_t n = 0;
    uint64_t a = cycle_counter(), b = now_ms();
    for (int i = 0; i < 8; ++i) { seed[n++] = (uint8_t)(a >> (8 * i)); seed[n++] = (uint8_t)(b >> (8 * i)); }
    uint64_t addr = (uint64_t)&seed;
    for (int i = 0; i < 8; ++i) seed[n++] = (uint8_t)(addr >> (8 * i));
    mix_in(seed, n);
    if (virtio_rng_init())
    {
        uint8_t hw[64]; uint32_t got = virtio_rng_read(hw, sizeof(hw));
        if (got >= 32) { hw_rng = 1; mix_in(hw, got); entropy_bits = 256U; }
    }
}

void random_get(uint8_t* out, uint32_t n)
{
    fold_pending();
    if (hw_rng)                                     /* top up from the host on every draw: cheap and keeps the pool fresh */
    {
        uint8_t hw[32]; uint32_t got = virtio_rng_read(hw, sizeof(hw));
        if (got) mix_in(hw, got);
    }
    uint32_t done = 0;
    while (done < n)
    {
        uint8_t blk[48]; uint8_t h[32];
        for (int i = 0; i < 32; ++i) blk[i] = key[i];
        for (int i = 0; i < 8; ++i) blk[32 + i] = (uint8_t)(out_counter >> (8 * i));
        blk[40] = 'o'; blk[41] = 'u'; blk[42] = 't';
        ++out_counter;
        sha256(blk, 43, h);
        for (uint32_t i = 0; i < 32 && done < n; ++i) out[done++] = h[i];
    }
    uint8_t rat[33]; for (int i = 0; i < 32; ++i) rat[i] = key[i]; rat[32] = 'r';       /* ratchet: earlier output cannot be recomputed from later state */
    sha256(rat, 33, key);
    bytes_out += n;
}

int random_ready() { return entropy_bits >= 128U; }

void random_status(RandomStatus* o)
{
    o->entropy_bits = entropy_bits; o->events = events; o->ready = random_ready(); o->hw_rng = hw_rng;
    o->seed_loaded = seed_loaded; o->bytes_out = bytes_out;
}

#define SEED_PATH "/var/lib/random.seed"
void random_seed_load()
{
    int root = fs_get_root();
    uint8_t buf[80]; uint64_t got = 0;
    if (fs_resolve("/var", root) < 0) (void)fs_mkdir("/var", root);
    if (fs_resolve("/var/lib", root) < 0) (void)fs_mkdir("/var/lib", root);
    if (fs_read_file(SEED_PATH, root, buf, sizeof(buf), &got) == 0 && got == 64)
    {
        random_add_bytes(buf, 64, 128U);
        seed_loaded = 1;
    }
    (void)random_seed_save();                         /* never reuse a seed: the file is replaced right away */
}

int random_seed_save()
{
    uint8_t buf[64];
    random_get(buf, sizeof(buf));
    int root = fs_get_root();
    uint32_t u = fs_current_uid(), g = fs_current_gid();
    fs_set_credentials(0U, 0U);                       /* the seed belongs to root whoever triggers the save */
    int rc = fs_write_data(SEED_PATH, root, buf, sizeof(buf));
    if (rc == 0) { int id = fs_resolve(SEED_PATH, root); if (id >= 0) { (void)fs_set_mode(id, 0600U); (void)fs_set_owner(id, 0U, 0U); } }
    fs_set_credentials(u, g);
    return rc;
}
