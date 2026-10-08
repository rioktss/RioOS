#ifndef ENV_H
#define ENV_H

int env_set(const char* assignment);
const char* env_get(const char* name);
void env_print();

#endif
