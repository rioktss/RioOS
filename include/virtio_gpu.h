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

#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include "types.h"

/* virtio-gpu (device id 16), 2D mode only: one XRGB8888 resource backed by caller memory, scanned out on
   scanout 0. The framebuffer layer (fb.cpp) uses it instead of ramfb when the device is present. */
int virtio_gpu_init(void* pixels, uint32_t width, uint32_t height);   /* 1 = scanout active */
int virtio_gpu_ready();
/* Copy pixel rows [y0, y1) to the host and flush them to the display. */
void virtio_gpu_flush_rows(int y0, int y1);
/* Display geometry the host reported for scanout 0 (0 when unknown). */
void virtio_gpu_display(uint32_t* width, uint32_t* height, int* enabled);
uint32_t virtio_gpu_flush_count();

#endif
