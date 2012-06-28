#include<stdio.h>
#include<signal.h>
#include<unistd.h>
#include<stdlib.h>
#include<fcntl.h>
#include<inttypes.h>
#include<semaphore.h>

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

/* I love the smell of global variables at 5 in the morning. */
struct region_list * rl;
struct region_list * cur;

int cycle = 0;
timer_t timer;
struct itimerspec t;
sem_t sem;

// TODO: Parse command line options.  Meanwhile, hardcode:
pid_t pid = 1694;

int main()
{
    /* Timer setup */
    timer_create(CLOCK_MONOTONIC, NULL, &timer);
    t.it_value.tv_sec = 1;
    t.it_value.tv_nsec = 0;
    t.it_interval.tv_sec = 0;
    t.it_interval.tv_nsec = 0;
    timer_settime(timer, 0, &t, NULL);

    sem_init(&sem, 0, 0);

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
