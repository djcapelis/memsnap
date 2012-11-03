/***********************************************************************
 * region_list.h                                                       *
 *                                                                     *
 * Everything you'd want in a header file for a region_list structure  *
 *                                                                     *
 **********************************************************************/

#include <sys/types.h>
#include <unistd.h>

#include "djc_defines.h"

#define RL_FLAG_RWANON      (1 << 0) /* bit 0 */ /* Only grab rw anonymous mappings */

struct region_list
{
    void * begin;
    void * end;
    struct region_list * next;
};

struct region_list * new_region_list(pid_t pid, int flags);
struct region_list * free_region_list(struct region_list * rl);
