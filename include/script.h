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
