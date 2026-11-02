/* Shared object loaded at run time by dltest via dlopen(). */
typedef unsigned long size_t;
extern "C" int printf(const char*, ...);
extern "C" int getpid(void);

static int counter = 10;                       /* writable data, needs RELATIVE reloc for the pointer below */
static const char* const names[] = { "alpha", "beta", "gamma" };

extern "C" int libhello_add(int a, int b) { return a + b + counter; }
extern "C" const char* libhello_name(int i) { return names[i % 3]; }
extern "C" int libhello_bump(void) { return ++counter; }
extern "C" void libhello_greet(const char* who) { printf("libhello: hi %s from pid %d\n", who, getpid()); }
static int mul(int a, int b) { return a * b; }
static int (*op_table[])(int, int) = { libhello_add, mul };
extern "C" int libhello_call(int idx, int a, int b) { return op_table[idx](a, b); }
