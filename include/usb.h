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

#ifndef USB_H
#define USB_H

#include "types.h"

/* xHCI (USB 3) host controller driver, polling: keyboards, pointing devices (mice, tablets, any HID report layout),
   hubs (multi-tier), hot-plug / hot-unplug, error recovery. */
struct RioInputEvent;
void usb_init();                    /* find xHCI on PCI, reset, enumerate root-hub ports (and whatever hangs off hubs) */
int usb_present();                  /* controller initialised */
int usb_keyboard_count();
int usb_mouse_count();              /* relative pointers */
int usb_tablet_count();             /* absolute pointers (tablets, touch screens) */
int usb_hub_count();
void usb_poll();                    /* process events, hot-plug, hub status, key repeat; cheap, called from the keyboard idle path */
void usb_rescan();                  /* force a look at every root port and hub port */
int usb_try_get_key(char* out);     /* next key typed on a USB keyboard (1 if any); arrows/PgUp/PgDn use the virtio-input codes 0x80..0x85 */
int usb_poll_event(struct RioInputEvent* event);   /* next pointer/scroll event of a USB pointing device (coordinates 0..32767) */
void usb_status();
int usb_selftest();                 /* control-transfer sanity test: 0 = pass */

#endif
