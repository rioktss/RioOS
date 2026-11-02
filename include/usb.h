#ifndef USB_H
#define USB_H

#include "types.h"

/* xHCI (USB 3) host controller driver, polling, HID boot-protocol keyboard and mouse. */
void usb_init();                    /* find xHCI on PCI, reset, enumerate root-hub ports */
int usb_present();                  /* controller initialised */
int usb_keyboard_count();
int usb_mouse_count();
void usb_poll();                    /* process events; cheap, called from the keyboard idle path */
int usb_try_get_key(char* out);     /* next key typed on a USB keyboard (ASCII), 1 if any */
void usb_status();
int usb_selftest();                 /* control-transfer sanity test: 0 = pass */

#endif
