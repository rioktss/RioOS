#ifndef FILTERS_H
#define FILTERS_H

/* Text filters (grep wc head tail sort uniq cut tr tee) usable as `cmd file` and as pipeline stages,
   plus general pipelines whose first stage is any built-in command (`ls | sort -r | head -n 3`). */
int filters_handle_pipeline(const char* line);          /* 1 if the line was a built-in/filter pipeline */
int filters_is_filter(const char* name);
int filters_run_file_command(const char* name, const char* args);   /* e.g. head -n 3 file; returns 1 if handled */

#endif
