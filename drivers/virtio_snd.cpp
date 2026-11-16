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

#include "virtio_snd.h"
#include "virtio_dev.h"
#include "timer.h"

#define SND_DEVICE_ID 25U
#define SND_VQ_CONTROL 0U
#define SND_VQ_TX 2U
#define SND_R_PCM_INFO 0x0100U
#define SND_R_PCM_SET_PARAMS 0x0101U
#define SND_R_PCM_PREPARE 0x0102U
#define SND_R_PCM_RELEASE 0x0103U
#define SND_R_PCM_START 0x0104U
#define SND_R_PCM_STOP 0x0105U
#define SND_S_OK 0x8000U
#define SND_D_OUTPUT 0U
#define SND_FMT_S16 5U
#define SND_PCM_INFO_SIZE 32U
#define SND_MAX_INFO 8U
#define SND_PERIOD_BYTES 4096U
#define SND_SLOTS 4U
#define SND_CTRL_TIMEOUT_MS 2000U

struct SndQuery { uint32_t code, start_id, count, size; };
struct SndPcmHdr { uint32_t code, stream_id; };
struct SndSetParams { uint32_t code, stream_id, buffer_bytes, period_bytes, features; uint8_t channels, format, rate, pad; };
struct SndStatus { uint32_t status, latency; };
struct SndSlot { uint32_t stream_id; uint8_t data[SND_PERIOD_BYTES]; SndStatus status; };

static_assert(sizeof(SndQuery) == 16, "virtio_snd_query_info");
static_assert(sizeof(SndSetParams) == 24, "virtio_snd_pcm_set_params");

static VirtioDev dev;
static VQ ctlq, txq;
static VQMem ctlq_mem VQ_MEM_ATTR;
static VQMem txq_mem VQ_MEM_ATTR;
static union { SndQuery q; SndPcmHdr h; SndSetParams p; } req __attribute__((aligned(16), section(".bss.virtio_req")));
static uint8_t resp[4U + SND_MAX_INFO * SND_PCM_INFO_SIZE] __attribute__((aligned(16), section(".bss.virtio_req")));
static SndSlot slots[SND_SLOTS] __attribute__((aligned(16), section(".bss.virtio_req")));

static SndInfo info;
static int present = 0;
static uint64_t bytes_played = 0;
static uint32_t errors = 0;

static const uint32_t rate_table[14] = { 5512, 8000, 11025, 16000, 22050, 32000, 44100, 48000, 64000, 88200, 96000, 176400, 192000, 384000 };

static const short sine256[256] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285, 32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571,
    30273, 29956, 29621, 29268, 28898, 28510, 28105, 27683, 27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
    23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868, 18204, 17530, 16846, 16151, 15446, 14732, 14010, 13279,
    12539, 11793, 11039, 10278, 9512, 8739, 7962, 7179, 6393, 5602, 4808, 4011, 3212, 2410, 1608, 804,
    0, -804, -1608, -2410, -3212, -4011, -4808, -5602, -6393, -7179, -7962, -8739, -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790, -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971, -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683, -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868, -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278, -9512, -8739, -7962, -7179, -6393, -5602, -4808, -4011, -3212, -2410, -1608, -804,
};

static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t get64(const uint8_t* p) { return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32); }

/* control request: device-readable request, device-writable response; 1 when the device answered SND_S_OK */
static int control(const void* r, uint32_t rlen, void* out, uint32_t outlen)
{
    uint8_t* o = (uint8_t*)out;
    for (uint32_t i = 0; i < 4U; ++i) o[i] = 0;
    VSeg segs[2] = { { r, rlen, 0 }, { out, outlen, 1 } };
    uint32_t got = 0;
    if (vq_request(&ctlq, segs, 2, SND_CTRL_TIMEOUT_MS, &got) != 0) return 0;
    return get32(o) == SND_S_OK;
}

static int pcm_cmd(uint32_t code)
{
    req.h.code = code;
    req.h.stream_id = (uint32_t)info.out_stream;
    return control(&req, sizeof(SndPcmHdr), resp, 4U);
}

int snd_init()
{
    present = 0;
    info.jacks = info.streams = info.chmaps = 0; info.out_stream = -1;
    info.formats_lo = info.rates = 0; info.channels_min = info.channels_max = 0;
    if (!virtio_dev_open(&dev, SND_DEVICE_ID, 0, 0)) return 0;
    info.jacks = virtio_dev_cfg32(&dev, 0);
    info.streams = virtio_dev_cfg32(&dev, 4);
    info.chmaps = virtio_dev_cfg32(&dev, 8);
    if (info.streams == 0 || !virtio_dev_queue(&dev, SND_VQ_CONTROL, &ctlq, &ctlq_mem, 8) ||
        !virtio_dev_queue(&dev, SND_VQ_TX, &txq, &txq_mem, 16) || !virtio_dev_start(&dev))
    {
        virtio_dev_reset(&dev);
        return 0;
    }
    uint32_t count = info.streams < SND_MAX_INFO ? info.streams : SND_MAX_INFO;
    req.q.code = SND_R_PCM_INFO; req.q.start_id = 0; req.q.count = count; req.q.size = SND_PCM_INFO_SIZE;
    if (!control(&req, sizeof(SndQuery), resp, 4U + count * SND_PCM_INFO_SIZE)) { virtio_dev_reset(&dev); return 0; }
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint8_t* e = resp + 4U + i * SND_PCM_INFO_SIZE;
        if (e[24] != SND_D_OUTPUT) continue;
        info.out_stream = (int)i;
        info.formats_lo = (uint32_t)get64(e + 8);
        info.rates = (uint32_t)get64(e + 16);
        info.channels_min = e[25]; info.channels_max = e[26];
        break;
    }
    if (info.out_stream < 0 || (info.formats_lo & (1U << SND_FMT_S16)) == 0U) { virtio_dev_reset(&dev); return 0; }
    present = 1;
    return 1;
}

int snd_present() { return present; }
const SndInfo* snd_get_info() { return &info; }
uint64_t snd_bytes_played() { return bytes_played; }
uint32_t snd_error_count() { return errors; }

const char* snd_error_text(int rc)
{
    switch (rc)
    {
        case SND_OK: return "ok";
        case SND_E_NODEV: return "no virtio-sound output stream (start QEMU with -audiodev none,id=a -device virtio-sound-device,audiodev=a)";
        case SND_E_ARGS: return "unsupported or invalid rate/channels/length";
        case SND_E_PARAMS: return "device rejected the stream parameters";
        case SND_E_PREPARE: return "stream prepare failed";
        case SND_E_START: return "stream start failed";
        case SND_E_IO: return "transfer error or timeout";
        case SND_E_WAV: return "not a 16-bit PCM WAV file";
        default: return "error";
    }
}

static int rate_index(uint32_t hz)
{
    for (int i = 0; i < 14; ++i)
        if (rate_table[i] == hz && (info.rates & (1U << i)) != 0U) return i;
    return -1;
}

typedef void (*SndFill)(void* ctx, uint8_t* dst, uint32_t bytes);

/* One blocking playback: params -> prepare -> start -> keep SND_SLOTS periods in flight -> drain -> stop -> release. */
static int play_stream(SndFill fill, void* ctx, uint32_t total, uint32_t rate_hz, uint32_t channels)
{
    if (!present) return SND_E_NODEV;
    int ri = rate_index(rate_hz);
    uint32_t frame = channels * 2U;
    if (ri < 0 || channels < info.channels_min || channels > info.channels_max || channels > 8U) return SND_E_ARGS;
    total -= total % frame;
    if (total == 0) return SND_E_ARGS;
    if (txq.broken || ctlq.broken) return SND_E_IO;

    uint32_t period = SND_PERIOD_BYTES - (SND_PERIOD_BYTES % frame);
    req.p.code = SND_R_PCM_SET_PARAMS; req.p.stream_id = (uint32_t)info.out_stream;
    req.p.buffer_bytes = period * SND_SLOTS; req.p.period_bytes = period; req.p.features = 0;
    req.p.channels = (uint8_t)channels; req.p.format = (uint8_t)SND_FMT_S16; req.p.rate = (uint8_t)ri; req.p.pad = 0;
    if (!control(&req, sizeof(SndSetParams), resp, 4U)) return SND_E_PARAMS;
    if (!pcm_cmd(SND_R_PCM_PREPARE)) return SND_E_PREPARE;
    if (!pcm_cmd(SND_R_PCM_START)) { (void)pcm_cmd(SND_R_PCM_RELEASE); return SND_E_START; }

    uint32_t period_ms = (uint32_t)(((uint64_t)period * 1000ULL) / ((uint64_t)rate_hz * frame)) + 1U;
    uint32_t wait_ms = 2000U + period_ms * 8U;
    uint32_t sent = 0, inflight = 0, done_bytes = 0;
    uint32_t slot_len[SND_SLOTS];
    int rc = SND_OK;
    int busy[SND_SLOTS];
    for (uint32_t i = 0; i < SND_SLOTS; ++i) busy[i] = 0;

    for (;;)
    {
        /* refill every idle slot while data remains */
        for (uint32_t s = 0; s < SND_SLOTS && sent < total && rc == SND_OK; ++s)
        {
            if (busy[s]) continue;
            uint32_t n = total - sent;
            if (n > period) n = period;
            fill(ctx, slots[s].data, n);
            slots[s].stream_id = (uint32_t)info.out_stream;
            slots[s].status.status = 0; slots[s].status.latency = 0;
            VSeg segs[3] = { { &slots[s].stream_id, 4U, 0 }, { slots[s].data, n, 0 }, { &slots[s].status, sizeof(SndStatus), 1 } };
            if (vq_post(&txq, (uint16_t)(s * 3U), segs, 3) != 0) { rc = SND_E_IO; break; }
            busy[s] = 1; slot_len[s] = n; sent += n; ++inflight;
            vq_kick(&txq);
        }
        if (inflight == 0) break;
        uint16_t head = 0; uint32_t len = 0;
        if (!vq_wait(&txq, &head, &len, wait_ms)) { txq.broken = 1; rc = SND_E_IO; break; }
        uint32_t s = head / 3U;
        if (s >= SND_SLOTS || !busy[s]) { rc = SND_E_IO; break; }
        busy[s] = 0; --inflight;
        if (slots[s].status.status != SND_S_OK) { ++errors; rc = SND_E_IO; }
        else done_bytes += slot_len[s];
    }
    bytes_played += done_bytes;
    if (rc == SND_OK) timer_delay_ms(period_ms < 120U ? 120U : period_ms);   /* let the host backend drain what it still buffers */
    if (!txq.broken)
    {
        (void)pcm_cmd(SND_R_PCM_STOP);
        (void)pcm_cmd(SND_R_PCM_RELEASE);
        /* STOP/RELEASE hand back any period the device still held; consume those completions so the ring stays in step */
        while (inflight > 0)
        {
            uint16_t head = 0; uint32_t len = 0;
            if (!vq_wait(&txq, &head, &len, 500U)) { txq.broken = 1; break; }
            --inflight;
        }
    }
    if (rc != SND_OK) ++errors;
    return rc;
}

struct CopyCtx { const uint8_t* src; uint32_t off; };
static void fill_copy(void* c, uint8_t* dst, uint32_t bytes)
{
    CopyCtx* x = (CopyCtx*)c;
    for (uint32_t i = 0; i < bytes; ++i) dst[i] = x->src[x->off + i];
    x->off += bytes;
}

int snd_play_pcm16(const void* data, uint32_t bytes, uint32_t rate_hz, uint32_t channels)
{
    if (!data || bytes == 0 || channels == 0) return SND_E_ARGS;
    CopyCtx c = { (const uint8_t*)data, 0 };
    return play_stream(fill_copy, &c, bytes, rate_hz, channels);
}

struct ToneCtx { uint32_t phase, step, vol, channels, frames_done, frames_total, ramp; };
static void fill_tone(void* c, uint8_t* dst, uint32_t bytes)
{
    ToneCtx* t = (ToneCtx*)c;
    uint32_t frames = bytes / (t->channels * 2U);
    for (uint32_t f = 0; f < frames; ++f)
    {
        int32_t v = sine256[t->phase >> 24];
        v = (int32_t)(((int64_t)v * (int64_t)t->vol) / 100);
        uint32_t pos = t->frames_done + f, left = t->frames_total - 1U - pos;
        uint32_t edge = pos < left ? pos : left;
        if (edge < t->ramp) v = (int32_t)(((int64_t)v * (int64_t)edge) / (int64_t)t->ramp);      /* short fade in/out avoids clicks */
        t->phase += t->step;
        for (uint32_t ch = 0; ch < t->channels; ++ch)
        {
            uint32_t o = (f * t->channels + ch) * 2U;
            dst[o] = (uint8_t)((uint32_t)v & 0xFFU);
            dst[o + 1U] = (uint8_t)(((uint32_t)v >> 8) & 0xFFU);
        }
    }
    t->frames_done += frames;
}

int snd_play_tone(uint32_t freq_hz, uint32_t ms, uint32_t volume_pct, uint32_t rate_hz, uint32_t channels)
{
    if (!present) return SND_E_NODEV;
    if (freq_hz < 20U || freq_hz > 20000U || ms == 0U || ms > 60000U || volume_pct == 0U || volume_pct > 100U ||
        rate_hz == 0U || freq_hz * 2U >= rate_hz || channels == 0U)
        return SND_E_ARGS;
    ToneCtx t;
    t.phase = 0;
    t.step = (uint32_t)(((uint64_t)freq_hz << 32) / (uint64_t)rate_hz);
    t.vol = volume_pct; t.channels = channels; t.frames_done = 0;
    t.frames_total = (uint32_t)(((uint64_t)rate_hz * (uint64_t)ms) / 1000ULL);
    if (t.frames_total == 0) return SND_E_ARGS;
    t.ramp = rate_hz / 125U;                          /* 8 ms */
    if (t.ramp * 2U > t.frames_total) t.ramp = t.frames_total / 2U;
    if (t.ramp == 0) t.ramp = 1;
    return play_stream(fill_tone, &t, t.frames_total * channels * 2U, rate_hz, channels);
}

static uint32_t le16(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

int snd_play_wav(const uint8_t* wav, uint32_t size, uint32_t* frames, uint32_t* rate_hz, uint32_t* channels)
{
    if (!wav || size < 44U) return SND_E_WAV;
    if (wav[0] != 'R' || wav[1] != 'I' || wav[2] != 'F' || wav[3] != 'F' || wav[8] != 'W' || wav[9] != 'A' || wav[10] != 'V' || wav[11] != 'E')
        return SND_E_WAV;
    uint32_t pos = 12, fmt_ok = 0, ch = 0, rate = 0, bits = 0;
    const uint8_t* pcm = 0; uint32_t pcm_len = 0;
    while (pos + 8U <= size)
    {
        uint32_t clen = get32(wav + pos + 4);
        const uint8_t* body = wav + pos + 8;
        uint32_t avail = size - (pos + 8U);
        if (wav[pos] == 'f' && wav[pos + 1] == 'm' && wav[pos + 2] == 't' && wav[pos + 3] == ' ')
        {
            if (clen < 16U || avail < 16U) return SND_E_WAV;
            uint32_t tag = le16(body);
            ch = le16(body + 2); rate = get32(body + 4); bits = le16(body + 14);
            fmt_ok = (tag == 1U || tag == 0xFFFEU) ? 1U : 0U;          /* PCM or WAVE_FORMAT_EXTENSIBLE (assumed PCM) */
        }
        else if (wav[pos] == 'd' && wav[pos + 1] == 'a' && wav[pos + 2] == 't' && wav[pos + 3] == 'a')
        {
            pcm = body; pcm_len = clen < avail ? clen : avail;           /* tolerate a header that claims too much */
            break;
        }
        if (clen > avail) break;
        pos += 8U + clen + (clen & 1U);
    }
    if (!fmt_ok || !pcm || bits != 16U || ch == 0U || ch > 8U || rate == 0U || pcm_len < ch * 2U) return SND_E_WAV;
    if (frames) *frames = pcm_len / (ch * 2U);
    if (rate_hz) *rate_hz = rate;
    if (channels) *channels = ch;
    return snd_play_pcm16(pcm, pcm_len, rate, ch);
}
