#include<errno.h>
#include<stdlib.h>
#include<stdio.h>
#include<string.h>
#include<fcntl.h>
#include<inttypes.h>
#include<unistd.h>

#include<sys/types.h>
#include<sys/stat.h>
#include<sys/mman.h>

#include "region_list.h"

int main()
{
    new_region_list(1779, 1);
    return 0;
}

struct region_list * new_region_list(pid_t pid, int flags)
{
    char * path = NULL;
    int maps_fd = -1;
    int maps_len;
    char * maps = NULL;
    int i = 0;
    off_t offset = 0;
    char i_hate_proc;
    struct region_list * head;
    struct region_list * cur;
    char * tok/*en_of_my_appreciation*/;

    /* Initialize */
    path = calloc(1, 25);
    err_chk(path == NULL);
    head = calloc(sizeof(struct region_list), 1);
    err_chk(head == NULL);
    cur = head;

    /* Open maps */
    snprintf(path, 24, "%s%d%s", "/proc/", (int) pid, "/maps");
    maps_fd = open(path, O_RDONLY);
    err_chk(maps_fd == -1);

    /* read maps into memory */
    for(maps_len = 0; read(maps_fd, &i_hate_proc, 1) == 1; maps_len++); /* find length because files in proc are silly */
    lseek(maps_fd, 0, SEEK_SET);
    maps = calloc(maps_len + 1, 1);
    err_chk(maps == NULL);
    while(offset != maps_len)
        offset += read(maps_fd, maps + offset, maps_len - offset);

    /* parse */
    while(1)
    {
        cur->next = calloc(sizeof(struct region_list), 1);
        err_chk(cur->next == NULL);
        cur->next->begin = (void *) strtol(maps + i, &tok, 16);
        cur->next->end = (void *) strtol(tok + 1, &tok, 16);
        for(;maps[i] != '\n';i++);
        if(flags & RL_FLAG_RWANON)
        {
            if(tok[1] != 'r' || tok[2] != 'w' || tok[21] != '0' || tok[22] != ' ')
            {
                free(cur->next);
                cur->next = NULL;
                i++;
                if(i == maps_len)
                    break;
                else
                    continue;
            }
        }
        if(i+1 == maps_len)
            break;
        else
            i++;
        cur = cur->next;
    }

    cur = head->next;
    /* print regionlist */
    while(cur != NULL)
    {
        printf("%p-%p\n", cur->begin, cur->end);
        cur = cur->next;
    }

    /* clean up */
    cur = head->next;
    free(head);
    free(path);
    free(maps);
    close(maps_fd);
    return cur;

err: /* Error handling */
    perror("new_region_list");
    if(path)
        free(path);
    if(maps)
        free(maps);
    if(maps_fd != -1)
        close(maps_fd);
    return NULL;
}

struct region_list * free_region_list(struct region_list * rl)
{
    if(rl->next != NULL)
        free_region_list(rl->next);
    free(rl);
    return NULL;
}
