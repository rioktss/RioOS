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

#ifndef SECURITY_H
#define SECURITY_H

#include "types.h"

enum SecurityRole
{
    SECURITY_USER = 0,
    SECURITY_ROOT = 1
};

void security_init();
SecurityRole security_role();
int security_is_root();
const char* security_username();
char security_prompt_char();
int security_login_user(const char* password);
int security_login_root(const char* password);
void security_logout_to_user();
/* Account database (hashed passwords, uid/gid, privilege separation). */
uint32_t security_uid();
uint32_t security_gid();
void security_apply_fs_credentials();
int security_verify(const char* name, const char* password);
int security_ids_of(const char* name, uint32_t* uid, uint32_t* gid);   /* 0 ok, -1 unknown account */
int security_account_at(int index, char* name16, uint32_t* uid, uint32_t* gid);   /* index-th account, no secrets; -1 past the end */
int security_login_as(const char* name, const char* password);
int security_user_add(const char* name, const char* password);   /* root only */
int security_user_del(const char* name);                         /* root only */
int security_set_password(const char* name, const char* old_password, const char* new_password);
void security_list_accounts();
int security_read_password(char* out, uint64_t cap);

#endif
