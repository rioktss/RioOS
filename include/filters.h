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

#ifndef FILTERS_H
#define FILTERS_H

/* Text filters (grep wc head tail sort uniq cut tr tee) usable as `cmd file` and as pipeline stages,
   plus general pipelines whose first stage is any built-in command (`ls | sort -r | head -n 3`). */
int filters_handle_pipeline(const char* line);          /* 1 if the line was a built-in/filter pipeline */
int filters_is_filter(const char* name);
int filters_run_file_command(const char* name, const char* args);   /* e.g. head -n 3 file; returns 1 if handled */

int filters_capture(const char* cmd, char* out, int cap);           /* run a command, capture its output (NUL-terminated); returns length */
int filters_handle_redirect(const char* line);                      /* > >> 2> 2>> &> 2>&1 for any built-in (echo keeps its legacy handler) */
int filters_capturing();                                            /* 1 while a command's output is being captured (pipes, $(...), redirects) */
int filters_glob(const char* in, char* out, int cap);               /* expand * and ? in unquoted words after the command */

#endif
