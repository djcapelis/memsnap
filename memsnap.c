#include<stdio.h>

#include "region_list.h"

int main()
{
    struct region_list * rl;
    struct region_list * cur;
    rl = new_region_list(1779, RL_FLAG_RWANON);
    cur = rl;
    while(cur != NULL)
    {
        printf("%p-%p\n", cur->begin, cur->end);
        cur = cur->next;
    }
    free_region_list(rl);
    return 0;
}
