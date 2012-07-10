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

void print_usage();
void alrm_hdlr(int useless);
void err_msg(char * msg);

/* External data */
extern char * optarg;
extern int optind;      /* Index to first non-arg parameter */

/* I love the smell of global variables at 5 in the morning. */
struct region_list * rl;
struct region_list * cur;

/* More globals */
int snap = 1;
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
bool OPT_F = false;

int termsnap;
int interval;
pid_t pid;

void print_usage()
{
    fprintf(stderr, "Usage: memsnap -hsu -t <sec> -m <ms> -p <pid> -d <path> -f <snaps>\n");
    fprintf(stderr, "\t-h Print usage\n");
    fprintf(stderr, "\t-p <pid> Attach to <pid>\n");
    fprintf(stderr, "\t-t <sec> Specify time interval between snapshots in seconds\n");
    fprintf(stderr, "\t-m <ms> Specify time interval between snapshots in milliseconds\n");
    fprintf(stderr, "\t-f <snaps> Finish after taking <snaps> number of snapshots\n");
    fprintf(stderr, "THE REMAINING OPTIONS ARE CURRENTLY UNIMPLEMENTED\n");
}

int main(int argc, char * argv[])
{
    /* Timer setup */
    timer_create(CLOCK_MONOTONIC, NULL, &timer);
    t.it_value.tv_sec = 1;
    t.it_value.tv_nsec = 0;
    t.it_interval.tv_sec = 0;
    t.it_interval.tv_nsec = 0;

    /* Argument parsing */
    char opt;
    char * strerr = NULL;
    long arg;
    while((opt = getopt(argc, argv, "+ht:m:p:sud:f:")) != -1)
    {
        switch(opt)
        {
            case 'p':
                if(OPT_P)
                    err_msg("memsnap can only attach one process at a time\n\n");
                OPT_P = true;
                arg = strtol(optarg, &strerr, 10);
                if(arg > INT_MAX || arg < 0 || strerr[0] != 0)
                    err_msg("Unable to parse -p argument correctly, should be a pid\n\n");
                pid = (pid_t) arg;
                optarg = NULL;
                break;
            case 't':
            case 'm':
                if(opt == 't')
                    OPT_T = true;
                else
                    OPT_M = true;
                if(OPT_M && OPT_T)
                    err_msg("-t and -m mutally exclusive\nPlease specify time in either seconds or milliseconds\n\n");
                arg = strtol(optarg, &strerr, 10);
                if(arg > INT_MAX || arg < 0 || strerr[0] != 0)
                {
                    if(opt == 't')
                        err_msg("Unable to parse -t argument correctly, should be number of seconds\n\n");
                    else
                        err_msg("Unable to parse -m argument correctly, should be number of milliseconds\n\n");
                }
                if(opt == 'm')
                {
                    t.it_value.tv_nsec = (arg % 1000) * 1000000;
                    t.it_value.tv_sec = (arg - (arg % 1000)) / 1000;
                }
                else
                    t.it_value.tv_sec = arg;
                optarg = NULL;
                break;
            case 'f':
                if(OPT_F)
                    err_msg("-f specified two or more times\n\n");
                OPT_F = true;
                arg = strtol(optarg, &strerr, 10);
                if(strerr[0] != 0)
                    err_msg("Unable to parse -f argument correctly, should be number of snapshots\n\n");
                if(arg < 0)
                    err_msg("Number of snapshots specified for -f argument is negative.\n\n");
                termsnap = arg;
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
        err_msg("memsnap requires a pid\n\n");
    //exit(0); // Option testing

    sem_init(&sem, 0, 0);

    // Call the handler to set up the signal handling and post to the sem for the first time
    alrm_hdlr(0);

    /* Timer set */
    timer_settime(timer, 0, &t, NULL);

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
            snprintf(buffer, 48, "%s%d%s%d", "snap", snap, "_seg", i);
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
    
        printf("Snap: %d\n", snap);
        if(OPT_F && snap == termsnap)
        {
            ptrace(PTRACE_DETACH, pid, NULL, NULL);
            return 0;
        }
        snap++;

        free_region_list(rl);
    }
    goto retry_sem; // Yes, yes, I know.

    return 0;

err:
    perror("main");
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    return -1;
}

void alrm_hdlr(int __attribute__((unused)) useless)
{
    signal(SIGALRM, &alrm_hdlr);

    sem_post(&sem);
    return;
}

void err_msg(char * msg)
{
    fprintf(stderr, msg);
    print_usage();
    exit(-1);
}
