#ifndef USER_H
#define USER_H

#include "types.h"

#define USER_ARGS_MAX 128

void user_execute(const char* path, const char* args = 0);
const char* user_get_args();
int user_current_pid();
void user_set_exit_code(int code);

extern "C" void user_enter(uint64_t entry, uint64_t stack_top);
extern "C" void user_exit_return();

#endif
