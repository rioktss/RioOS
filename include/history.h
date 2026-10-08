#ifndef HISTORY_H
#define HISTORY_H

void history_init();
void history_add(const char* line);
int history_count();
const char* history_get(int index);
void history_clear();
void history_print(int last_n);

#endif
