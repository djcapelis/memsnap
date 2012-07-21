#define _GNU_SOURCE /* strnlen(), itimers*/
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
#include<errno.h>
#include<string.h>

#include<sys/ptrace.h>
#include<sys/types.h>
#include<sys/wait.h>
#include<sys/mman.h>
#include<sys/stat.h>
#include<time.h>
#include<sys/time.h>

#include "region_list.h"

/* memsnap defines */
#define BUFFER_SIZE 4096  /* Must align with system page size */

struct piditem
{
    pid_t pid;
    struct piditem * next;
};
struct piditem * head;

void print_usage();
void alrm_hdlr(int useless);
void err_msg(char * msg);
void ptrace_all_pids(int cmd);
void free_pid_list(struct piditem * ele);

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
bool is_attached;

/* Options */
bool OPT_H = false;
bool OPT_T = false;
bool OPT_M = false;
bool OPT_U = false;
bool OPT_P = false;
bool OPT_S = false;
bool OPT_G = false;
bool OPT_D = false;
bool OPT_F = false;
bool OPT_L = false;
bool OPT_A = false;

int termsnap;
int interval;
int destdirlen;
char * destdir;

void print_usage()
{
    fprintf(stderr, "Usage: memsnap [options] -p pid\n");
    fprintf(stderr, "\t-h Print usage\n");
    fprintf(stderr, "\t-p <pid> Attach to <pid>\n");
    fprintf(stderr, "\t-d <dir> Specify destination directory for snapshots\n");
    fprintf(stderr, "\t-t <sec> Specify time interval between snapshots in seconds\n");
    fprintf(stderr, "\t-m <ms> Specify time interval between snapshots in milliseconds\n");
    fprintf(stderr, "\t-u <us> Specify time interval between snapshots in microseconds\n");
    fprintf(stderr, "\t-f <snaps> Finish after taking <snaps> number of snapshots\n");
    fprintf(stderr, "\t-g Snapshot all regions into one file globbed together\n");
    /*fprintf(stderr, "\t-s Snapshot into a sparse file with regions at accurate offsets in file\n");*/ /* UNDOCUMENTED */
    fprintf(stderr, "\t-l Snap live, without pausing the process being snapshotted\n");
    fprintf(stderr, "\t-a Snapshot all readable regions, including read-only segs & mapped files\n");
}

int main(int argc, char * argv[])
{
    is_attached = false;
    struct piditem * curitem;
    /* piditem setup */
    head = calloc(1, sizeof(struct piditem));
    head->next = NULL;

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
    struct stat dirstat;
    int chk;
    while((opt = getopt(argc, argv, "+ht:m:u:p:sglad:f:")) != -1)
    {
        switch(opt)
        {
            case 'a':
                OPT_A = true;
                break;
            case 's':
                OPT_S = true;
                break;
            case 'g':
                OPT_G = true;
                break;
            case 'd':
                if(OPT_D)
                    err_msg("Two or more -d arguments, please specify only one destination path\n\n");
                OPT_D = true;
                destdir = optarg;
                destdirlen = strnlen(optarg, 2049);
                if(destdirlen > 2048)
                    err_msg("Memsnap limits the destination directory (-d argument) to 2048 characters\n\n");
                chk = stat(destdir, &dirstat);
                if(chk == -1 && errno == ENOENT)
                    err_msg("Invalid path specified by -d option\n\n");
                else if(chk == -1)
                    err_chk(1); /* Undefined error, handle using perror() */
                if(!S_ISDIR(dirstat.st_mode))
                    err_msg("Path specified by -d is not a directory\n\n");
                break;
            case 'p':
                OPT_P = true;
                arg = strtol(optarg, &strerr, 10);
                if(arg > INT_MAX || arg < 0 || strerr[0] != 0)
                    err_msg("Unable to parse -p argument correctly, should be a pid\n\n");
                curitem = head;
                while(curitem->next != NULL)
                    curitem = curitem->next; // Go to last list item
                curitem->pid = (pid_t) arg;
                curitem->next = calloc(1, sizeof(struct piditem));
                curitem = curitem->next;
                curitem->next = NULL;
                optarg = NULL;
                break;
            case 't':
            case 'm':
            case 'u':
                if(OPT_T || OPT_M || OPT_U)
                    err_msg("-t -m -u mutally exclusive\nPlease specify only one\n\n");
                if(opt == 't')
                    OPT_T = true;
                else if(opt == 'm')
                    OPT_M = true;
                else
                    OPT_U = true;
                arg = strtol(optarg, &strerr, 10);
                if(arg > INT_MAX || arg < 0 || strerr[0] != 0)
                {
                    if(opt == 't')
                        err_msg("Unable to parse -t argument correctly, should be number of seconds\n\n");
                    else if(opt == 'm')
                        err_msg("Unable to parse -m argument correctly, should be number of milliseconds\n\n");
                    else
                        err_msg("Unable to parse -u argument correctly, should be number of microseconds\n\n");
                }
                if(opt == 'm')
                {
                    t.it_value.tv_nsec = (arg % 1000) * 1000000;
                    t.it_value.tv_sec = arg / 1000; /* truncates per Section 2.5 of K&R 2nd ed */
                }
                else if(opt == 'u')
                {
                    t.it_value.tv_nsec = (arg % 1000000) * 1000;
                    t.it_value.tv_sec = arg / 1000000;
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
            case 'l':
                OPT_L = true;
                break;
            case 'h':
            default:
                print_usage();
                return 0;
        }
    }
    /* Option validity checks */
    if(OPT_G && OPT_S)
        err_msg("Options -g and -s are mutually exclusive\n\n");
    if(!OPT_P)
        err_msg("memsnap requires a pid\n\n");
    if(!OPT_D)
    {
        destdirlen = 0;
        destdir = calloc(1, 2);
        destdir[0] = '.';
        destdir[1] = '\0';
    }
    //exit(0); // Option testing

    sem_init(&sem, 0, 0);

    // Call the handler to set up the signal handling and post to the sem for the first time
    alrm_hdlr(0);

    /* Timer set */
    timer_settime(timer, 0, &t, NULL);

retry_sem:
    while(sem_wait(&sem) == 0)
    {
        is_attached = true;
        ptrace_all_pids(PTRACE_ATTACH);
        curitem = head;
        while(curitem->next != NULL)
        {
            pid_t pid;
            int i, j;
            char * buffer;
            int mem_fd;
            int seg_fd;
            int seg_len;
            off_t offset;

            pid = curitem->pid;

            buffer = calloc(1, 4096);
            snprintf(buffer, 24, "%s%d%s", "/proc/", (int) pid, "/mem");

            mem_fd = open(buffer, O_RDONLY);
            if(OPT_A)
                rl = new_region_list(pid, 0);
            else
                rl = new_region_list(pid, RL_FLAG_RWANON);
            cur = rl;
            for(i=0; cur != NULL; i++)
            {
                seg_len = (int)((intptr_t) cur->end - (intptr_t) cur->begin);
                if(!OPT_S && !OPT_G) /* Normal execution, without -s or -g */
                {
                    snprintf(buffer, 4096 - destdirlen, "%s%s%d%s%d%s%d", destdir, "/pid", pid, "_snap", snap, "_seg", i);
                    seg_fd = open(buffer, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
                }
                else if((OPT_S || OPT_G) && i == 0) /* Run on the first seg for -s or -g */
                {
                    snprintf(buffer, 4096 - destdirlen, "%s%s%d%s%d", destdir, "/pid", pid, "_snap", snap);
                    seg_fd = open(buffer, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
                }

                offset = 0;
                lseek(mem_fd, (intptr_t) cur->begin, SEEK_SET);
                if(OPT_S)
                {
                    chk = lseek(seg_fd, (intptr_t) cur->begin, SEEK_SET);
                    if(chk == -1 && errno == EINVAL)
                        fprintf(stderr, "Seek failed in output sparse file, this is why -s is undocumented in memsnap.\n");
                }
                for(j=0; j<seg_len; j+=BUFFER_SIZE)
                {
                    offset = read(mem_fd, buffer, BUFFER_SIZE);
                    err_chk(offset == -1);
                    while(offset != BUFFER_SIZE)
                    {
                        chk = read(mem_fd, buffer + offset, BUFFER_SIZE - offset);
                        err_chk(chk == -1);
                        offset += chk;
                    }
                    offset = write(seg_fd, buffer, BUFFER_SIZE);
                    err_chk(offset == -1);
                    while(offset != BUFFER_SIZE)
                    {
                        chk = write(seg_fd, buffer + offset, BUFFER_SIZE - offset);
                        err_chk(chk == -1);
                        offset += chk;
                    }
                }

                if((!OPT_S && !OPT_G) || cur->next == NULL) /* If last seg, close even if -s or -g */
                    close(seg_fd);

                cur = cur->next;
            }
            close(mem_fd);
            free_region_list(rl);
            printf("snap: %d, pid: %d\n", snap, pid);
            curitem = curitem->next;
        }
        timer_settime(timer, 0, &t, NULL);

        ptrace_all_pids(PTRACE_DETACH);
        is_attached = false;

        if(OPT_F && snap == termsnap)
            return 0;
        snap++;
    }
    goto retry_sem; // Yes, yes, I know.

    free_pid_list(head);

    return 0;

err:
    perror("memsnap");
    if(is_attached)
        ptrace_all_pids(PTRACE_DETACH);
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

void ptrace_all_pids(int cmd)
{
    int status;
    if(OPT_L)
        return;
    struct piditem * cur = head;
    while(cur->next != NULL)
    {
        ptrace(cmd, cur->pid, NULL, NULL);
        if(cmd == PTRACE_ATTACH)
            wait(&status);
        cur = cur->next;
    }
}

/* Frees all piditems subsequent in the list */
void free_pid_list(struct piditem * ele)
{
    if(ele->next != NULL)
        free_pid_list(ele->next);
    free(ele->next);
    ele->next = NULL;
}
