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

#ifndef VIRTIO_SND_H
#define VIRTIO_SND_H

#include "types.h"

/* virtio-sound (device id 25): PCM output through the first output stream, blocking playback.
   QEMU: -audiodev <backend>,id=a -device virtio-sound-device,audiodev=a */
struct SndInfo
{
    uint32_t jacks, streams, chmaps;
    int out_stream;              /* index of the output stream in use, -1 if none */
    uint32_t formats_lo;         /* supported sample formats (bit n = VIRTIO_SND_PCM_FMT n), low 32 bits */
    uint32_t rates;              /* supported rates (bit n = VIRTIO_SND_PCM_RATE n) */
    uint8_t channels_min, channels_max;
};

enum
{
    SND_OK = 0,
    SND_E_NODEV = -1,
    SND_E_ARGS = -2,
    SND_E_PARAMS = -3,
    SND_E_PREPARE = -4,
    SND_E_START = -5,
    SND_E_IO = -6,
    SND_E_WAV = -7
};

int snd_init();                                  /* 1 = an output stream is usable */
int snd_present();
const SndInfo* snd_get_info();
/* PCM16 little-endian, interleaved. bytes is rounded down to whole frames. */
int snd_play_pcm16(const void* data, uint32_t bytes, uint32_t rate_hz, uint32_t channels);
/* Sine tone: freq_hz 20..20000, duration in ms (1..60000), volume 1..100 percent. */
int snd_play_tone(uint32_t freq_hz, uint32_t ms, uint32_t volume_pct, uint32_t rate_hz, uint32_t channels);
/* RIFF/WAVE, PCM 16-bit mono/stereo at a supported rate; *frames gets the number of frames played. */
int snd_play_wav(const uint8_t* wav, uint32_t size, uint32_t* frames, uint32_t* rate_hz, uint32_t* channels);
uint64_t snd_bytes_played();
uint32_t snd_error_count();
const char* snd_error_text(int rc);

#endif
