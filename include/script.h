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

#ifndef SCRIPT_H
#define SCRIPT_H

/* Shell scripting: variables, $? $# $0..$9, if/elif/else/fi, for/while/do/done,
   break/continue/exit/shift, test/[ ], let (integer arithmetic), sh/source. */
int script_handle_compound(const char* line);   /* line starts with if/for/while/sh/source/. : returns 1 if consumed */
int script_handle_simple(char* line);           /* after expansion: NAME=val, let, test, [, true, false, exit, shift, break, continue */
int script_expand(const char* in, char* out, int cap);   /* $VAR ${VAR} $? $# $0-$9 */
int script_run_file(const char* path, const char* args);  /* returns exit status or -1 if cannot read */
void script_set_status(int status);
int script_status();

int script_call_function(const char* line);

#endif
