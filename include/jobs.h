#ifndef JOBS_H
#define JOBS_H

#include "types.h"

/* Shell job control: background jobs (`cmd &`), ELF pipelines (`a | b`),
   jobs/fg/bg/kill/wait built-ins. All return 1 when they consumed the line. */
int jobs_handle_line(const char* line);
void jobs_poll();                       /* report/reap finished background jobs */
void jobs_note_stopped(int pid, const char* cmd);
int jobs_resolve_program(const char* name, char* out_path, int cap);
int jobs_run_foreground(const char* path, const char* args, int in_fd, int out_fd);

#endif
