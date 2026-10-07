#ifndef DIAG_H
#define DIAG_H

#include "types.h"

/* Log levels: a message is printed when level <= current threshold. */
enum LogLevel
{
    LOG_ERROR = 0,
    LOG_WARN  = 1,
    LOG_INFO  = 2,
    LOG_DEBUG = 3
};

void diag_set_level(int level);
int  diag_get_level();
const char* diag_level_name(int level);

/* Prints "[LEVEL] message" on the serial console when enabled. */
void klog(int level, const char* message);

uint64_t diag_log_printed();
uint64_t diag_log_suppressed();

/* Stops the machine with a visible report. IRQs are masked first. */
void kpanic(const char* message, const char* file, int line) __attribute__((noreturn));

#define PANIC(msg) kpanic((msg), __FILE__, __LINE__)

/* Always-on assertion (the kernel is small; checks are cheap). */
#define KASSERT(cond) \
    do { if (!(cond)) kpanic("assertion failed: " #cond, __FILE__, __LINE__); } while (0)

#endif
