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

#ifndef RIOOS_VIRTIO_INPUT_H
#define RIOOS_VIRTIO_INPUT_H

#include "types.h"

enum RioInputEventKind
{
    RIO_INPUT_NONE = 0,
    RIO_INPUT_KEY = 1,
    RIO_INPUT_POINTER = 2,
    RIO_INPUT_SCROLL = 3
};

struct RioInputEvent
{
    uint32_t kind;
    char key;
    int32_t x;       /* Absolute coordinate normalized to 0..32767. */
    int32_t y;
    uint32_t pressed;
    int32_t scroll;   /* Positive = wheel up, negative = wheel down. */
};

void virtio_input_init();
int virtio_input_ready();
int virtio_input_keyboard_ready();
int virtio_input_pointer_ready();
/* Non-blocking. Returns keyboard, pointer, or vertical wheel-scroll events. */
int virtio_input_poll(struct RioInputEvent* event);
/* Non-blocking translated key input, for interactive kernel/user programs. */
int virtio_input_try_get_key(char* key);

#endif
