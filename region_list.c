#include<errno.h>
#include<stdlib.h>
#include<stdio.h>
#include<string.h>
#include<sys/types.h>
#include<sys/stat.h>
#include<fcntl.h>

#include "region_list.h"

int main()
{
    new_region_list(1779);
    return 0;
}

struct region_list * new_region_list(pid_t pid)
{
    char * path = calloc(1, 25);
    int maps_fd = -1;

    /* Open maps */
    snprintf(path, 24, "%s%d%s", "/proc/", (int) pid, "/maps");
    maps_fd = open(path, O_RDONLY);
    err_chk(maps_fd == -1);

    close(maps_fd);
    return NULL;

err: /* Error handling */
    perror("new_region_list");
    if(maps_fd != -1)
        close(maps_fd);
    return NULL;
}

struct region_list * free_region_list(struct region_list * rl)
{
    if(rl){};
    return NULL;
}

struct region_list * update_region_list(pid_t pid, struct region_list * rl)
{
    if(pid){};
    return rl;
}

struct region_list * clean_region_list(struct region_list * rl)
{
    return rl;
}
