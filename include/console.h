#ifndef RIOOS_CONSOLE_H
#define RIOOS_CONSOLE_H

#include "types.h"

/* Framebuffer text terminal (VT100 subset, 16 colours, scrollback).
   Every byte written through uart_putc() is mirrored here once the console
   is active, so the QEMU window shows exactly what the serial shell shows. */

/* Take over the framebuffer; returns 0 on success, -1 if no pixel surface. */
int console_start();
int console_active();
void console_putc(char c);
void console_write(const char* text);
/* Draw anything that changed since the last flush (cheap when idle). */
void console_flush();
/* Positive lines = scroll back into history, negative = toward the newest. */
void console_scroll_lines(int lines);
void console_scroll_pages(int pages);
/* Jump back to the live screen (called on key input and on new output). */
void console_snap_to_bottom();
int console_scroll_offset();

/* Geometry / state, for diagnostics and tests. */
int console_columns();
int console_rows();
int console_scrollback_lines();

/* Draw text on the framebuffer only (never sent to the UART) using ANSI
   palette colour 0..7 for the foreground, optionally bold/bright. */
void console_write_attr(const char* text, int fg, int bold);

#ifdef HOST_TEST
uint16_t console_test_cell(int row, int col);
void console_test_cursor(int* row, int* col);
void console_test_reset();
#endif

#endif
