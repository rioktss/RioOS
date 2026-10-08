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
void security_login_root();
void security_logout_to_user();
int security_read_password(char* out, uint64_t cap);

#endif
