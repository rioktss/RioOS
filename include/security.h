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
int security_login_as(const char* name, const char* password);
int security_user_add(const char* name, const char* password);   /* root only */
int security_user_del(const char* name);                         /* root only */
int security_set_password(const char* name, const char* old_password, const char* new_password);
void security_list_accounts();
int security_read_password(char* out, uint64_t cap);

#endif
