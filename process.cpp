#include "process.h"
#include "uart.h"
#include "string.h"

#define MAX_PROCESSES 32
#define PROCESS_NAME_SIZE 32

struct Process
{
    int used;
    int pid;
    int state;
    int kind;
    int exit_code;
    uint64_t user_low;
    uint64_t user_high;
    char name[PROCESS_NAME_SIZE];
};

static Process processes[MAX_PROCESSES];
static int next_pid;

static void print_int(int value)
{
    if (value == 0) { uart_putc('0'); return; }
    if (value < 0) { uart_putc('-'); value = -value; }
    char b[16]; int p=0;
    while(value>0){b[p++]=(char)('0'+value%10);value/=10;}
    while(p)uart_putc(b[--p]);
}

void process_init()
{
    next_pid=0;
    for(int i=0;i<MAX_PROCESSES;i++){
        processes[i].used=0; processes[i].pid=-1; processes[i].state=PROCESS_UNUSED;
        processes[i].kind=PROCESS_KERNEL; processes[i].exit_code=0;
        processes[i].user_low=0; processes[i].user_high=0; processes[i].name[0]='\0';
    }
    int k=process_create("kernel");
    int s=process_create("shell");
    process_set_state(k, PROCESS_RUNNING);
    (void)s;
    uart_puts("Process manager initialized.\r\n");
}

int process_create(const char* name)
{
    if(!name)return -1;
    for(int i=0;i<MAX_PROCESSES;i++)if(!processes[i].used){
        processes[i].used=1; processes[i].pid=next_pid++; processes[i].state=PROCESS_READY;
        processes[i].kind=PROCESS_KERNEL; processes[i].exit_code=0; processes[i].user_low=0; processes[i].user_high=0;
        str_copy(processes[i].name,name,PROCESS_NAME_SIZE); return processes[i].pid;
    }
    return -1;
}

int process_configure_user(int pid,uint64_t low,uint64_t high)
{
    for(int i=0;i<MAX_PROCESSES;i++)if(processes[i].used&&processes[i].pid==pid){
        if(low>=high)return -1; processes[i].kind=PROCESS_USER; processes[i].user_low=low; processes[i].user_high=high; return 0;
    }
    return -1;
}
int process_mark_exit(int pid,int code)
{
    for(int i=0;i<MAX_PROCESSES;i++)if(processes[i].used&&processes[i].pid==pid){processes[i].exit_code=code;processes[i].state=PROCESS_ZOMBIE;return 0;}
    return -1;
}
int process_is_user(int pid){for(int i=0;i<MAX_PROCESSES;i++)if(processes[i].used&&processes[i].pid==pid)return processes[i].kind==PROCESS_USER;return 0;}
uint64_t process_user_low(int pid){for(int i=0;i<MAX_PROCESSES;i++)if(processes[i].used&&processes[i].pid==pid)return processes[i].user_low;return 0;}
uint64_t process_user_high(int pid){for(int i=0;i<MAX_PROCESSES;i++)if(processes[i].used&&processes[i].pid==pid)return processes[i].user_high;return 0;}
int process_exit_code(int pid){for(int i=0;i<MAX_PROCESSES;i++)if(processes[i].used&&processes[i].pid==pid)return processes[i].exit_code;return -1;}

int process_destroy(int pid)
{
    if (pid == 0 || pid == 1) return -2;
    for(int i=0;i<MAX_PROCESSES;i++)if(processes[i].used&&processes[i].pid==pid){
        processes[i].used=0; processes[i].pid=-1; processes[i].state=PROCESS_UNUSED; processes[i].kind=PROCESS_KERNEL; processes[i].name[0]='\0'; return 0;
    }
    return -1;
}
int process_count(){int n=0;for(int i=0;i<MAX_PROCESSES;i++)if(processes[i].used)n++;return n;}
int process_pid_at(int slot){if(slot<0||slot>=MAX_PROCESSES||!processes[slot].used)return -1;return processes[slot].pid;}
int process_set_state(int pid,int state){for(int i=0;i<MAX_PROCESSES;i++)if(processes[i].used&&processes[i].pid==pid){processes[i].state=state;return 0;}return -1;}
int process_get_state(int pid){for(int i=0;i<MAX_PROCESSES;i++)if(processes[i].used&&processes[i].pid==pid)return processes[i].state;return -1;}

void process_list()
{
    uart_puts("PID   STATE      TYPE   NAME\r\n-----------------------------\r\n");
    for(int i=0;i<MAX_PROCESSES;i++)if(processes[i].used){
        print_int(processes[i].pid); uart_puts("     ");
        switch(processes[i].state){case PROCESS_READY:uart_puts("READY      ");break;case PROCESS_RUNNING:uart_puts("RUNNING    ");break;case PROCESS_SLEEPING:uart_puts("SLEEPING   ");break;case PROCESS_ZOMBIE:uart_puts("ZOMBIE     ");break;default:uart_puts("UNUSED     ");break;}
        if(processes[i].kind==PROCESS_USER)uart_puts("USER   "); else uart_puts("KERNEL ");
        uart_puts(processes[i].name); uart_puts("\r\n");
    }
}
