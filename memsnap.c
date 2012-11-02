/* Includes */
#define _GNU_SOURCE /* strnlen(), itimers*/
#include<stdio.h>
#include<signal.h>
#include<stdlib.h>
#include<unistd.h>
#include<stdbool.h>
#include<limits.h>
#include<fcntl.h>
#include<semaphore.h>
#include<getopt.h>
#include<errno.h>
#include<string.h>
#include<time.h>

/* Includes from sys/ */
#include<sys/ptrace.h>
#include<sys/types.h>
#include<sys/wait.h>
#include<sys/stat.h>

/* Local project includes */
#include "region_list.h"
#include "djc_defines.h"

/* memsnap defines */
#define BUFFER_SIZE 4096  /* Must align with system page size */

/* piditem list, used to keep track of the pids memsnap is operating on */
struct piditem
{
    pid_t pid;
    struct piditem * next;
};
struct piditem * head;

/* Functions */
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
bool OPT_s = false;
bool OPT_M = false;
bool OPT_U = false;
bool OPT_P = false;
bool OPT_S = false;
bool OPT_G = false;
bool OPT_D = false;
bool OPT_F = false;
bool OPT_L = false;
bool OPT_A = false;
bool OPT_C = false;
bool OPT_Q = false;

/* Globals which totally work and are fine, shush. */
int termsnap;
int interval;
int destdirlen;
char * destdir;

/* Does what it says on the tin */
void print_usage()
{
    fprintf(stderr, "Usage: memsnap [options] [command]\n");
    fprintf(stderr, "\t-h Print usage\n");
    fprintf(stderr, "\t-p <pid> Attach to <pid>\n");
    fprintf(stderr, "\t-d <dir> Specify destination directory for snapshots\n");
    fprintf(stderr, "\t-s <sec> Specify time interval between snapshots in seconds\n");
    fprintf(stderr, "\t-m <ms> Specify time interval between snapshots in milliseconds\n");
    fprintf(stderr, "\t-u <us> Specify time interval between snapshots in microseconds\n");
    fprintf(stderr, "\t-f <snaps> Finish after taking <snaps> number of snapshots\n");
    fprintf(stderr, "\t-g Snapshot all segments into one file globbed together\n");
    /*fprintf(stderr, "\t-c Produce corefiles by tickling each pid with gcore\n");*/ /* UNDOCUMENTED */
    /*fprintf(stderr, "\t-S Snapshot into a sparse file with regions at accurate offsets in file\n");*/ /* UNDOCUMENTED */
    fprintf(stderr, "\t-l Snap live, without pausing the process(es) being snapshot\n");
    fprintf(stderr, "\t-a Snapshot all readable segments, incl read-only segs & mapped files\n");
    fprintf(stderr, "\t-q Be quiet, do not output anything but errors\n");
}

/* Entrypoint, argument parsing and core memory dumping functionality */
int main(int argc, char * argv[])
{
    is_attached = false;
    struct piditem * curitem;
    struct piditem * previtem;

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
    while((opt = getopt(argc, argv, "+hs:m:u:p:Sglad:f:cq")) != -1)
    {
        switch(opt)
        {
            case 'a':
                OPT_A = true;
                break;
            case 'S':
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
                {
                    perror("Error parsing -d argument:"); /* Undefined error, handle using perror() */
                    exit(-1);
                }
                if(!S_ISDIR(dirstat.st_mode))
                    err_msg("Path specified by -d is not a directory\n\n");
                optarg = NULL;
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
            case 's':
            case 'm':
            case 'u':
                if(OPT_s || OPT_M || OPT_U)
                    err_msg("-s -m -u mutally exclusive\nPlease specify only one\n\n");
                if(opt == 's')
                    OPT_s = true;
                else if(opt == 'm')
                    OPT_M = true;
                else
                    OPT_U = true;
                arg = strtol(optarg, &strerr, 10);
                if(arg > INT_MAX || arg < 0 || strerr[0] != 0)
                {
                    if(opt == 's')
                        err_msg("Unable to parse -s argument correctly, should be number of seconds\n\n");
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
            case 'c':
                OPT_C = true;
                /* TODO: Suck less by doing your own ELF output */
                if(WEXITSTATUS(system("gcore > /dev/null")) != 2)
                    err_msg("Unable to execute gcore, which is required for the -c flag, ensure it is installed and in your path.\n\n");
                fprintf(stderr, "Warning: The -c option to memsnap is marked as experimental.\nIt is available beacuse it is a useful feature, but the implementation is a poor hack which just calls gcore.\nYou must have this utility installed and in your path.\nYou must use the -l flag for live tracing and the regions captured by gcore are not the same as in the other memsnap output modes.\n\n");
                break;
            case 'q':
                OPT_Q = true;
                break;
            case 'h':
            default:
                print_usage();
                return 0;
        }
    }
    /* Option validity checks */
    if(OPT_G && OPT_S)
        err_msg("Options -g and -S are mutually exclusive\n\n");
    if(OPT_A && OPT_C)
        fprintf(stderr, "Warning: -a is irrelevant when used with -c, gcore does its own stunts.\n\n");
    if(OPT_C && !OPT_L)
        err_msg("memsnap only supports -c with -l in this release.\n\n");
    if(!OPT_D) /* Set default destdir to current directory if none is specified */
    {
        destdirlen = 2; /* extra room because it's, just shush */
        destdir = calloc(1, 2);
        destdir[0] = '.';
        destdir[1] = '\0';
    }

    /* Start a process if specified on the command line */
    if(argc > optind)
    {
        /* Fork */
        pid_t ret = 0;
        ret = fork();
        err_chk(ret == -1);

        /* Which side of the fork are we on? */
        if(ret == 0) /* We are the child */
        {
            int i;
            char ** arglist;
            arglist = calloc((argc - optind) + 1, sizeof(char *)); /* Enough for all the args, plus a null */
            for(i = optind;i <= argc;i++)
                arglist[i - optind] = argv[i];
            arglist[argc - optind] = NULL;
            chk = execvp(argv[optind], arglist);  /* Never returns */
            err_chk(chk == -1);
        }
        else /* We are the parent */
        {
            /* Insert new pid into pidlist */
            curitem = head;
            while(curitem->next != NULL)
                curitem = curitem->next; // Go to last list item
            curitem->pid = ret;
            curitem->next = calloc(1, sizeof(struct piditem));
            curitem = curitem->next;
            curitem->next = NULL;
        }
    }

    /* Check that there's something to snapshot */
    if(head->pid == 0)
        err_msg("No processes specified to snapshot\n\n");

    sem_init(&sem, 0, 0);

    // Call the handler to set up the signal handling and post to the sem for the first time
    alrm_hdlr(0);

    /* Timer set */
    timer_settime(timer, 0, &t, NULL);

    /* Main memory dumping functionality and loop */
    while(1)
    {
        if(sem_wait(&sem) != 0) /* Signals cause returns, wait for a real sem_post() */
            continue;
        is_attached = true; /* From here on out, error handlers will PTRACE_DETACH */
        ptrace_all_pids(PTRACE_ATTACH);
        curitem = head;
        /* Snapshot memory for each pid */
        while(curitem->next != NULL)
        {
            pid_t pid;
            int i, j, len;
            char * buffer;
            int mem_fd;
            int regions_fd;
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
            if(rl == NULL) /* Region list failed, process likely dead */
            {
                if(!OPT_Q)
                    fprintf(stderr, "No longer snapshotting pid %d, unable to read maps\n", curitem->pid);

                /* Fixup the pidlist */
                if(head == curitem) /* current item is head of list */
                {
                    if(head->next->next == NULL) /* We are done */
                    {
                        if(!OPT_Q)
                            fprintf(stderr, "No pids left to snapshot, terminating.\n");
                        free_pid_list(head);
                        return 0;
                    }
                    else
                    {
                        previtem = head;
                        head = curitem->next;
                        free(previtem);
                        previtem = NULL;
                        curitem = curitem->next;
                        continue;
                    }
                }
                else /* current item is not the head of the list */
                {
                    previtem = head;
                    while(previtem->next != curitem) /* Find item previous to current item */
                    {
                        previtem = previtem->next;
                    }
                    previtem->next = curitem->next;
                    free(curitem);
                    curitem = previtem->next;
                    continue;
                }
            }
            cur = rl;
            if(OPT_C)
            {
                char * hackbuffer;
                hackbuffer = calloc(1, 4096);
                snprintf(buffer, 4096, "gcore -o %s%s%d%s%d %d > /dev/null", destdir, "/pid", pid, "_snap", snap, pid);
                system(buffer);
                snprintf(buffer, 4096, "%s%s%d%s%d.%d", destdir, "/pid", pid, "_snap", snap, pid);
                snprintf(hackbuffer, 4096, "%s%s%d%s%d", destdir, "/pid", pid, "_snap", snap);
                rename(buffer, hackbuffer);
                free(hackbuffer);
            }
            else
            {
                if(!OPT_G)
                {
                    snprintf(buffer, 4096, "%s%s%d%s%d%s", destdir, "/pid", pid, "_snap", snap, "_regions");
                    regions_fd = open(buffer, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
                    i = 0;
                    while(cur != NULL)
                    {
                        len = snprintf(buffer, 4096, "seg %d: %p-%p\n", i, cur->begin, cur->end);
                        offset = write(regions_fd, buffer, len);
                        while(offset != len)
                        {
                            offset += write(regions_fd, buffer + offset, len - offset);
                        }
                        i++;
                        cur = cur->next;
                    }
                    close(regions_fd);
                    cur=rl;
                }
                for(i=0; cur != NULL; i++)
                {
                    seg_len = (int)((intptr_t) cur->end - (intptr_t) cur->begin);
                    if(!OPT_S && !OPT_G) /* Normal execution, without -S or -g */
                    {
                        snprintf(buffer, 4096, "%s%s%d%s%d%s%d", destdir, "/pid", pid, "_snap", snap, "_seg", i);
                        seg_fd = open(buffer, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
                    }
                    else if((OPT_S || OPT_G) && i == 0) /* Run on the first seg for -S or -g */
                    {
                        snprintf(buffer, 4096, "%s%s%d%s%d", destdir, "/pid", pid, "_snap", snap);
                        seg_fd = open(buffer, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
                    }

                    offset = 0;
                    lseek(mem_fd, (intptr_t) cur->begin, SEEK_SET);
                    if(OPT_S)
                    {
                        chk = lseek(seg_fd, (intptr_t) cur->begin, SEEK_SET);
                        if(chk == -1 && errno == EINVAL)
                            fprintf(stderr, "Seek failed in output sparse file, this is why -S is undocumented in memsnap.\n");
                    }

                    /* read/write loop */
                    for(j=0; j<seg_len; j+=BUFFER_SIZE)
                    {
                        offset = read(mem_fd, buffer, BUFFER_SIZE);
                        err_chk(offset == -1);
                        while(offset != BUFFER_SIZE) /* This usually shouldn't happen, but keep trying to read if need be */
                        {
                            chk = read(mem_fd, buffer + offset, BUFFER_SIZE - offset);
                            err_chk(chk == -1);
                            offset += chk;
                        }
                        offset = write(seg_fd, buffer, BUFFER_SIZE);
                        err_chk(offset == -1);
                        while(offset != BUFFER_SIZE) /* This usually shouldn't happen, but keep trying to write if need be */
                        {
                            chk = write(seg_fd, buffer + offset, BUFFER_SIZE - offset);
                            err_chk(chk == -1);
                            offset += chk;
                        }
                    }

                    if((!OPT_S && !OPT_G) || cur->next == NULL) /* If last seg, close even if -S or -g */
                        close(seg_fd);

                    cur = cur->next;
                }
            }
            close(mem_fd);
            free(buffer);
            free_region_list(rl);
            if(!OPT_Q)
                printf("snap: %d, pid: %d\n", snap, pid);
            curitem = curitem->next;
        }
        /* Reset timer */
        timer_settime(timer, 0, &t, NULL);

        /* We're done, detach */
        ptrace_all_pids(PTRACE_DETACH);
        is_attached = false;

        /* Check for termination */
        if(OPT_F && snap == termsnap)
        {
            if(!OPT_D)
                free(destdir);
            timer_delete(timer);
            free_pid_list(head);
            return 0;
        }
        snap++;
    }


    return 0;

/* Error handler called by the err_chk macro */
err:
    perror("memsnap");
    if(is_attached)
        ptrace_all_pids(PTRACE_DETACH);
    if(!OPT_D)
        free(destdir);
    timer_delete(timer);
    free_pid_list(head);
    return -1;
}

/* Signal handler that triggers on every timer fire */
void alrm_hdlr(int __attribute__((unused)) useless)
{
    signal(SIGALRM, &alrm_hdlr);

    sem_post(&sem);
    return;
}

/* Output an error message and bail */
void err_msg(char * msg)
{
    fprintf(stderr, msg);
    print_usage();
    exit(-1);
}

/* Execute ptrace commands to the appropriate pids if the options are appropriate */
void ptrace_all_pids(int cmd)
{
    int status;
    if(OPT_L)
        return;
    struct piditem * cur = head;
    while(cur->next != NULL)
    {
        if(OPT_C) /* Not supported gracefully by gcore, so -c requires -l for now*/
        {
            if(cmd == PTRACE_ATTACH)
                kill(cur->pid, SIGSTOP);
            else if(cmd == PTRACE_DETACH)
                kill(cur->pid, SIGCONT);
            else
                fprintf(stderr, "Unsupported ptrace command issued during -c flag, which uses signals instead of ptrace.\nPlease file a bug!\n");
            continue;
        }
        status = ptrace(cmd, cur->pid, NULL, NULL);
        if(status == -1 && errno != ESRCH)
        {
            char * perrormsg;
            status = errno;
            perrormsg = calloc(128, 1);
            snprintf(perrormsg, 128, "ptrace failed for pid %d", cur->pid);
            errno = status;
            if(!OPT_Q)
                perror(perrormsg);
            free(perrormsg);
        }
        if(cmd == PTRACE_ATTACH && status != -1)
            wait(&status);
        cur = cur->next;
    }
}

/* Frees all piditems subsequent in the list */
void free_pid_list(struct piditem * ele)
{
    if(ele->next != NULL)
        free_pid_list(ele->next);
    free(ele);
}
