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
