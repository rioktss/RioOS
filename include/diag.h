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

#ifndef DIAG_H
#define DIAG_H

#include "types.h"

/* Log levels: a message is printed when level <= current threshold. */
enum LogLevel
{
    LOG_ERROR = 0,
    LOG_WARN  = 1,
    LOG_INFO  = 2,
    LOG_DEBUG = 3
};

void diag_set_level(int level);
int  diag_get_level();
const char* diag_level_name(int level);

/* Prints "[LEVEL] message" on the serial console when enabled. */
void klog(int level, const char* message);

uint64_t diag_log_printed();
uint64_t diag_log_suppressed();

/* Stops the machine with a visible report. IRQs are masked first. */
void kpanic(const char* message, const char* file, int line) __attribute__((noreturn));

#define PANIC(msg) kpanic((msg), __FILE__, __LINE__)

/* Always-on assertion (the kernel is small; checks are cheap). */
#define KASSERT(cond) \
    do { if (!(cond)) kpanic("assertion failed: " #cond, __FILE__, __LINE__); } while (0)

#endif
