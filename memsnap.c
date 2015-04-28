/***********************************************************************
 * memsnap.c                                                           *
 *                                                                     *
 * The main program logic of memsnap, all in a jumble.                 *
 *                                                                     *
 **********************************************************************/

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
struct piditem * head; /* Head node for the master list of pids */

/* Functions */
void print_usage();
void alrm_hdlr(int useless);
void err_msg(char * msg);
void ptrace_all_pids(int cmd);
void free_pid_list(struct piditem * ele);

/* External data for argument parsing */
extern char * optarg;
extern int optind;      /* Index to first non-arg parameter */

/* I love the smell of global variables at 5 in the morning. */
/* These are used to provide timing and synchronization between the signal handler and the main code */
timer_t timer;
struct itimerspec t;
sem_t sem;

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
    /* Variable declarations */
    bool is_attached = false;   /* Control whether the exit/error handler runs a PTRACE_DETACH */
    bool exit_error = false;    /* Used for error/exit handling */
    struct piditem * curitem;   /* Generic item pointer for piditems */
    struct piditem * previtem;  /* Used to keep track of previous piditem nodes in lists */
    int snap = 1;               /* Current snapshot number */
    int termsnap;               /* Snapshot number we should end on, specified by option -f */
    int destdirlen;             /* Keeps track of the length of the destination directory */

    /* Things that need to be in scope to get cleaned up by error/exit handlers */
    char * destdir;             /* The destination directory, as specified by option -d */
    char * buffer;              /* The buffer we use for pretty much everything... */
    int mem_fd;                 /* The fd for /proc/<pid>/mem */
    int regions_fd;             /* The fd for the _regions output file */
    int seg_fd;                 /* The fd for output segments */
    struct region_list * rl;    /* Region list pointer */

    /* setup piditem list */
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
            /* Set a flag to capture all readable segments, not just the r/w ones */
            case 'a':
                OPT_A = true;
                break;
            /* Set a flag to snapshot into a sparse file */
            /* This option is UNDOCUMENTED and EXPERIMENTAL because 64-bit systems produce 64-bit sparse files and a lot of filesystems don't actually support full 64-bit file addresses. */
            case 'S':
                OPT_S = true;
                break;
            /* Glob all segments into one snapshot file */
            case 'g':
                OPT_G = true;
                break;
            /* Directory to put snapshots in */
            case 'd':
                /* TODO: Unicode safe? */
                if(OPT_D) /* ... if argument already specified */
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
                    exit(EXIT_FAILURE);
                }
                if(!S_ISDIR(dirstat.st_mode))
                    err_msg("Path specified by -d is not a directory\n\n");
                optarg = NULL;
                break;
            /* Specify pid(s) to snapshot */
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
            /* Options that change snapshot interval timing */
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
                if(opt == 'm') /* interval is in milliseconds */
                {
                    t.it_value.tv_nsec = (arg % 1000) * 1000000;
                    t.it_value.tv_sec = arg / 1000; /* truncates per Section 2.5 of K&R 2nd ed */
                }
                else if(opt == 'u') /* interval is in microseconds */
                {
                    t.it_value.tv_nsec = (arg % 1000000) * 1000;
                    t.it_value.tv_sec = arg / 1000000;
                }
                else /* interval is in seconds */
                    t.it_value.tv_sec = arg;
                optarg = NULL;
                break;
            /* Specify number of snapshots to take before exiting */
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
            /* Set a flag to trace live and not use ptrace */
            case 'l':
                OPT_L = true;
                break;
            /* Set an undocumented option to use gcore to produce corefiles */
            case 'c':
                /* TODO: Suck less by doing your own ELF output, this code is terrible */
                OPT_C = true;
                /* check if gcore is in the path, error out if it isn't */
                if(WEXITSTATUS(system("gcore > /dev/null")) != 2)
                    err_msg("Unable to execute gcore, which is required for the -c flag, ensure it is installed and in your path.\n\n");
                /* Warn the user that they're using an experimental option which is poorly written */
                fprintf(stderr, "Warning: The -c option to memsnap is marked as experimental.\nIt is available because it is a useful feature, but the implementation is a poor hack which just calls gcore.\nYou must have this utility installed and in your path.\nYou must use the -l flag for live tracing and the regions captured by gcore are not the same as in the other memsnap output modes.\n\n");
                break;
            /* Set a flag to lessen the omit the routine output messages */
            case 'q':
                OPT_Q = true;
                break;
            /* Print usage and exit */
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


    /* Signals start */
    // Initialize the semaphore used for making the signal handler play nice
    sem_init(&sem, 0, 0);
    // Call the handler to set up the signal handling and post to the sem for the first time
    alrm_hdlr(0);

    /* Timer set */
    timer_settime(timer, 0, &t, NULL);

    /* Main memory dumping functionality and loop */
    while(1)
    {
        /* Wait for the signal handler to tell us to go */
        errno = 0; /* Reset errno, so it can be properly checked after sem_wait */
        if(sem_wait(&sem) != 0) /* Signals cause returns, wait for a real sem_post() */
        {
            err_chk(errno == EINVAL);   /* Handle EINVAL, which shouldn't happen */
            continue;                   /* Handle EINTR, which can definitely happen */
        }

        /* Attach the processes */
        is_attached = true; /* From here on out, error handlers will PTRACE_DETACH */
        ptrace_all_pids(PTRACE_ATTACH);
        curitem = head;

        /* Snapshot memory for each pid */
        while(curitem->next != NULL)
        {
            pid_t pid;
            int i, j, len;
            int seg_len;
            off_t offset;

            /* Current pid to snapshot */
            pid = curitem->pid;

            /* Open the file with the process's memory */
            buffer = calloc(1, 4096);
            snprintf(buffer, 24, "%s%d%s", "/proc/", (int) pid, "/mem");
            mem_fd = open(buffer, O_RDONLY);

            /* gcore has no need for a region list... use mem_fd figure out if it's alive */
            if(!OPT_C)
            {
                if(OPT_A)
                    rl = new_region_list(pid, 0);
                else
                    rl = new_region_list(pid, RL_FLAG_RWANON);
            }

            if(rl == NULL || mem_fd == -1) /* Assume the process is dead */
            {
                if(!OPT_Q)
                    fprintf(stderr, "Error snapshotting pid %d, process likely dead, dropping...\n", curitem->pid);

                /* Fixup the pidlist */
                if(head == curitem) /* current item is head of list */
                {
                    if(head->next->next == NULL) /* We are done */
                    {
                        if(!OPT_Q)
                            fprintf(stderr, "No pids left to snapshot, terminating.\n");
                        goto cleanup_and_term;
                    }
                    else /* make a new pid the head of the list, remove current pid, continue snapshotting rest */
                    {
                        previtem = head;
                        head = curitem->next;
                        free(previtem);
                        previtem = NULL;
                        goto nextpid;
                    }
                }
                else /* current item is not the head of the list, remove the pid from the pidlist */
                {
                    previtem = head;
                    while(previtem->next != curitem) /* Find item previous to current item */
                        previtem = previtem->next;
                    previtem->next = curitem->next;
                    free(curitem);
                    curitem = previtem;
                    goto nextpid;
                }
            }

            /* "snapshot" via asking gcore for coredumps */
            if(OPT_C)
            {
                char * buffer2;

                /* Run gcore */
                buffer2 = calloc(1, 4096);
                snprintf(buffer, 4096, "gcore -o %s%s%d%s%d %d > /dev/null", destdir, "/pid", pid, "_snap", snap, pid);
                system(buffer);

                /* rename output file */
                snprintf(buffer, 4096, "%s%s%d%s%d.%d", destdir, "/pid", pid, "_snap", snap, pid);
                snprintf(buffer2, 4096, "%s%s%d%s%d", destdir, "/pid", pid, "_snap", snap);
                rename(buffer, buffer2);

                free(buffer2);
                goto nextpid;
            }

            /* Begin snapshot */

            /* List all the regions into the regions file */
            struct region_list * cur;
            cur = rl;
            snprintf(buffer, 4096, "%s%s%d%s%d%s", destdir, "/pid", pid, "_snap", snap, "_regions");
            regions_fd = open(buffer, O_CREAT | O_WRONLY, S_IRUSR | S_IWUSR);
            i = 0;

            /* Build write regions into the _regions file */
            while(cur != NULL)
            {
                len = snprintf(buffer, 4096, "seg %d: %p-%p\n", i, cur->begin, cur->end);
                err_chk(len < 1);

                offset = 0;
                while(offset != len)
                {
                    chk = write(regions_fd, buffer + offset, len - offset);
                    err_chk(chk == -1);
                    offset += chk;
                }

                cur = cur->next;
                i++;
            }
            close(regions_fd);
            regions_fd = 0;
            cur = rl;

            /* For each segment... */
            for(i=0; cur != NULL; i++)
            {
                /* Open a new segment file for each segment */
                if(!OPT_S && !OPT_G) /* Normal execution, without -S or -g */
                {
                    snprintf(buffer, 4096, "%s%s%d%s%d%s%d", destdir, "/pid", pid, "_snap", snap, "_seg", i);
                    seg_fd = open(buffer, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
                }

                /* Given -S or -g, open a segment file for *all* segments */
                else if((OPT_S || OPT_G) && i == 0) /* Run on the first seg for -S or -g */
                {
                    snprintf(buffer, 4096, "%s%s%d%s%d", destdir, "/pid", pid, "_snap", snap);
                    seg_fd = open(buffer, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
                }

                lseek(mem_fd, (intptr_t) cur->begin, SEEK_SET);

                /* If writing a sparse output file, seek to the right place */
                if(OPT_S)
                {
                    chk = lseek(seg_fd, (intptr_t) cur->begin, SEEK_SET);
                    if(chk == -1 && errno == EINVAL)
                        fprintf(stderr, "Seek failed in output sparse file, this is why -S is undocumented in memsnap.\n");
                }

                seg_len = (int)((intptr_t) cur->end - (intptr_t) cur->begin);

                /* read/write loop */
                for(j=0; j<seg_len; j+=BUFFER_SIZE)
                {
                    offset = 0;
                    while(offset != BUFFER_SIZE)
                    {
                        chk = read(mem_fd, buffer + offset, BUFFER_SIZE - offset);
                        err_chk(chk == -1);
                        offset += chk;
                    }
                    offset = 0;
                    while(offset != BUFFER_SIZE)
                    {
                        chk = write(seg_fd, buffer + offset, BUFFER_SIZE - offset);
                        err_chk(chk == -1);
                        offset += chk;
                    }
                }

                /* If we're done with the segment file, close it */
                if((!OPT_S && !OPT_G) || cur->next == NULL)
                {
                    close(seg_fd);
                    seg_fd = 0;
                }

                cur = cur->next;
            }

            /* We finished snapshotting this pid */
            if(!OPT_Q)
                printf("snap: %d, pid: %d\n", snap, pid);

nextpid:
            /* Cleanup and move to next pid */
            if(mem_fd != 0 && mem_fd != -1)
            {
                close(mem_fd);
                mem_fd = 0;
            }
            if(buffer)
            {
                free(buffer);
                buffer = NULL;
            }
            if(rl)
            {
                free_region_list(rl);
                rl = NULL;
            }
            curitem = curitem->next;
        }

        /* Reset timer */
        timer_settime(timer, 0, &t, NULL);

        /* Detach */
        ptrace_all_pids(PTRACE_DETACH);
        is_attached = false;

        /* Are we done with all our snapshots? */
        if(OPT_F && snap == termsnap)
            goto cleanup_and_term;
        snap++;
    }

/* Error handler called by the err_chk macro */
err:
    perror("memsnap");
    exit_error = true;
    /* fallthrough */

/* We're done, exit cleanly */
cleanup_and_term:
    if(is_attached)
        ptrace_all_pids(PTRACE_DETACH);

/* Cleanup */
    if(!OPT_D)
        free(destdir);
    timer_delete(timer);
    sem_destroy(&sem);
    free_pid_list(head);
    if(mem_fd != 0 && mem_fd != -1)
        close(mem_fd);
    if(seg_fd != 0 && seg_fd != -1)
        close(seg_fd);
    if(regions_fd != 0 && regions_fd != -1)
        close(mem_fd);
    if(buffer)
        free(buffer);
    if(rl)
        free_region_list(rl);

/* Exit */
    if(exit_error == true)
        exit(EXIT_FAILURE);
    exit(EXIT_SUCCESS);
}

/* Signal handler that triggers on every timer fire */
void alrm_hdlr(int __attribute__((unused)) useless)
{
    /* Arm the signal to fire again */
    signal(SIGALRM, &alrm_hdlr);

    /* Post to the semaphore allowing the main loop to go ahead */
    sem_post(&sem);
    return; /* Yield */
}

/* Output an error message, print a usage message and bail */
void err_msg(char * msg)
{
    fprintf(stderr, msg);
    print_usage();
    exit(EXIT_FAILURE);
}

/* Execute ptrace commands to the appropriate pids if the options are appropriate */
void ptrace_all_pids(int cmd)
{
    int status;                     /* Used to check return values and statuses */
    struct piditem * cur = head;    /* Used to iterate through the pidlist */

    /* If user wants to memsnap live, do not mess with processes via ptrace */
    if(OPT_L)
        return;

    /* Send a ptrace command to each item in the pidlist */
    while(cur->next != NULL)
    {
        /* DEAD CODE BELOW */
        if(OPT_C) /* Not supported gracefully by gcore, so -c requires -l for now */
        {
            /*********************************************************************
            * This code uses bare signals to control processes instead of ptrace *
            * It may be useful in the future, currently it should never execute. *
            *                                                                    *
            * This was originally considered for gcore, which also uses ptrace,  *
            * but it turns out it wasn't all that happy with signals either...   *
            *********************************************************************/
            if(cmd == PTRACE_ATTACH)
                kill(cur->pid, SIGSTOP);
            else if(cmd == PTRACE_DETACH)
                kill(cur->pid, SIGCONT);
            else
                fprintf(stderr, "Unsupported ptrace command issued during -c flag, which uses signals instead of ptrace.\nPlease file a bug!\n");
            continue;
        }
        /* DEAD CODE ABOVE */

        /* Send the ptrace command */
        status = ptrace(cmd, cur->pid, NULL, NULL);

        /* Error handling */
        if(status == -1 && errno != ESRCH) /* Warn if error, but don't worry if the process doesn't exist, we'll catch that later */
        {
            char * perrormsg;
            status = errno; /* Stash errno in a temporary variable while we call other functions */
            perrormsg = calloc(128, 1); /* 128 bytes is enough for anyone... */
            snprintf(perrormsg, 128, "ptrace failed for pid %d", cur->pid);
            errno = status; /* Bring back saved errno */
            status = -1;
            if(!OPT_Q)
                perror(perrormsg);
            free(perrormsg);
        }
        
        /* If we're attaching and there wasn't an error, wait() on child */
        if(cmd == PTRACE_ATTACH && status != -1)
            wait(&status); /* TODO: defer wait until PTRACE_ATTACH sent to all processes, then wait on them all. */
        
        /* Go on to the next pid */
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
