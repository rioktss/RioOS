#ifndef SHELLEXT_H
#define SHELLEXT_H

/* Shell extras: alias/unalias, printf, expr, more/less, history -w/-r.
   shellext_handle returns 1 when the command was handled. */
int shellext_handle(const char* command, char* argument);
int shellext_alias_expand(const char* line, char* out, int cap);   /* 1 if the first word was an alias (result in out) */
void shellext_history_load(int verbose);

#endif
