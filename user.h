#ifndef USER_H
#define USER_H

#include "types.h"

void user_execute(const char* path);
int user_current_pid();
void user_set_exit_code(int code);

extern "C" void user_enter(uint64_t entry, uint64_t stack_top);
extern "C" void user_exit_return();

#endif
