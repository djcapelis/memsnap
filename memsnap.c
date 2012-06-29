#include<stdio.h>
#include<signal.h>
#include<unistd.h>
#include<stdlib.h>
#include<stdbool.h>
#include<limits.h>
#include<fcntl.h>
#include<inttypes.h>
#include<semaphore.h>
#include<getopt.h>

#include<sys/ptrace.h>
#include<sys/types.h>
#include<sys/wait.h>
#include<sys/mman.h>
#include<sys/stat.h>
#define __USE_POSIX199309
#include<time.h>
#include<sys/time.h>

#include "region_list.h"

void alrm_hdlr(int useless);
void print_usage();

/* External data */
extern char * optarg;
extern int optind;      /* Index to first non-arg parameter */

/* I love the smell of global variables at 5 in the morning. */
struct region_list * rl;
struct region_list * cur;

/* More globals */
int cycle = 0;
timer_t timer;
struct itimerspec t;
sem_t sem;

/* Options */
bool OPT_H = false;
bool OPT_T = false;
bool OPT_M = false;
bool OPT_P = false;
bool OPT_S = false;
bool OPT_U = false;
bool OPT_D = false;
bool OPT_C = false;

int termcyc;
int interval;
pid_t pid;

void print_usage()
{
    fprintf(stderr, "Usage: memsnap -hsu -t <sec> -m <ms> -p <pid> -d <path>\n");
    fprintf(stderr, "\t-h Print usage\n");
    fprintf(stderr, "\t-p <pid> Attach to <pid>\n");
    fprintf(stderr, "THE REMAINING OPTIONS ARE CURRENTLY UNIMPLEMENTED\n");
}

int main(int argc, char * argv[])
{
    /* Argument parsing */
    char opt;
    char * strerr = NULL;
    long arg;
    while((opt = getopt(argc, argv, "+ht:m:p:sud:c:")) != -1)
    {
        switch(opt)
        {
            case 'p':
                if(OPT_P)
                {
                    fprintf(stderr, "memsnap can only attach one process at a time\n\n");
                    print_usage();
                    exit(-1);
                }
                OPT_P = true;
                arg = strtol(optarg, &strerr, 10);
                if(arg > INT_MAX || arg < 0 || strerr[0] != 0)
                {
                    fprintf(stderr, "Unable to parse pid correctly\n\n");
                    print_usage();
                    exit(-1);
                }
                pid = (pid_t) arg;
                optarg = NULL;
                break;
            case 'h':
            default:
                print_usage();
                return 0;
        }
    }
    /* Option validity checks */
    if(!OPT_P)
    {
        fprintf(stderr, "memsnap requires a pid\n\n");
        print_usage();
        exit(-1);
    }

    sem_init(&sem, 0, 0);

    /* Timer setup */
    timer_create(CLOCK_MONOTONIC, NULL, &timer);
    t.it_value.tv_sec = 1;
    t.it_value.tv_nsec = 0;
    t.it_interval.tv_sec = 0;
    t.it_interval.tv_nsec = 0;
    timer_settime(timer, 0, &t, NULL);

    // Call the handler to set up the signal handling and post to the sem for the first time
    alrm_hdlr(0);

retry_sem:
    while(sem_wait(&sem) == 0)
    {
        int status;
        int i, j;
        char * buffer;
        int mem_fd;
        int seg_fd;
        int seg_len;
        off_t offset;

        buffer = calloc(1, 4096);
        snprintf(buffer, 24, "%s%d%s", "/proc/", (int) pid, "/mem");
        
        ptrace(PTRACE_ATTACH, pid, NULL, NULL);
        wait(&status);

        mem_fd = open(buffer, O_RDONLY);
        rl = new_region_list(pid, RL_FLAG_RWANON);
        cur = rl;
        for(i=0; cur != NULL; i++)
        {
            seg_len = (int)((intptr_t) cur->end - (intptr_t) cur->begin);
            snprintf(buffer, 48, "%s%d%s%d", "cycle", cycle, "_seg", i);
            seg_fd = open(buffer, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);

            offset = 0;
            lseek(mem_fd, (intptr_t) cur->begin, SEEK_SET);
            for(j=0; j<seg_len; j+=1024)
            {
                offset = read(mem_fd, buffer, 1024);
                err_chk(offset == -1);
                offset = write(seg_fd, buffer, 1024);
                err_chk(offset == -1);
            }

            close(seg_fd);

            cur = cur->next;
        }
        close(mem_fd);
    
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        timer_settime(timer, 0, &t, NULL);
    
        printf("Cycle: %d\n", cycle);
        cycle++;

        free_region_list(rl);
    }
    goto retry_sem; // Yes, yes, I know.

    return 0;

err:
    perror("main");
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    return 0;
}

void alrm_hdlr(int __attribute__((unused)) useless)
{
    signal(SIGALRM, &alrm_hdlr);

    sem_post(&sem);
    return;
}
