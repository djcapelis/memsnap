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

void dumpmem(int useless);

/* I love the smell of global variables at 5 in the morning. */
struct region_list * rl;
struct region_list * cur;

int cycle = 0;
timer_t timer;
struct itimerspec t;
sem_t sem;

// TODO: Parse command line options.  Meanwhile, hardcode:
pid_t pid = 1779;

int main()
{
    dumpmem(0);

    /* Timer setup */
    timer_create(CLOCK_MONOTONIC, NULL, &timer);
    t.it_value.tv_sec = 1;
    t.it_value.tv_nsec = 0;
    t.it_interval.tv_sec = 1;
    t.it_interval.tv_nsec = 0;
    timer_settime(timer, 0, &t, NULL);

    sem_init(&sem, 0, 1);
    printf("Init'd semaphore, waiting...\n");
    while(sem_wait(&sem) == 0)
    {
        int status;
        int i;
        char * path;
        int mem_fd;
        int seg_fd;
        int seg_len;
        char * memseg;
        off_t offset;
        char hack = '\0';

        path = calloc(1, 50);
        snprintf(path, 24, "%s%d%s", "/proc/", (int) pid, "/mem");
        
        ptrace(PTRACE_ATTACH, pid, NULL, NULL);
        wait(&status);

        mem_fd = open(path, O_RDONLY);
        rl = new_region_list(pid, RL_FLAG_RWANON);
        cur = rl;
        for(i=0; cur != NULL; i++)
        {
            seg_len = (int)((intptr_t) cur->end - (intptr_t) cur->begin);
            snprintf(path, 48, "%s%d%s%d", "cycle_", cycle, "_seg_", i);
            seg_fd = open(path, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);

            printf("%d\n", lseek(seg_fd, seg_len, SEEK_SET));
            printf("%d\n", write(seg_fd, &hack, 1));
            lseek(seg_fd, 0, SEEK_SET);
            memseg = mmap(NULL, seg_len, PROT_READ | PROT_WRITE, MAP_SHARED, seg_fd, 0);

            memseg[4] = '\0';

            offset = 0;
            lseek(mem_fd, 0, (intptr_t) cur->begin);
            while(offset != seg_len)
            {
                printf("mem_fd: %d, memseg: %p, offset: %d, seg_len: %p, seg_fd: %d, cur->begin: %p, cur->end: %p\n", mem_fd, memseg, offset, seg_len, seg_fd, cur->begin, cur->end);
                offset += read(mem_fd, memseg + offset, seg_len - offset);
                perror("read:");
            }

            munmap(memseg, seg_len);
            close(seg_fd);

            cur = cur->next;
        }
        close(mem_fd);
    
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
    
        printf("Cycle: %d\n", cycle);
        cycle++;

        free_region_list(rl);
    }

    return 0;
}

void dumpmem(int __attribute__((unused)) useless)
{
    printf("In signal handler\n");

    signal(SIGALRM, &dumpmem);

    sem_post(&sem);
    return;
}
