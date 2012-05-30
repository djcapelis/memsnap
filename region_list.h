#include <sys/types.h>
#include <unistd.h>

#include "djc_defines.h"

#define RL_FLAG_STACK       (1 << 0) /* bit 0 */ /* Mem region is [stack] */
#define RL_FLAG_HEAP        (1 << 1) /* bit 1 */ /* Mem region is [heap] */

#define RL_FLAG_RWANON      (1 << 0) /* bit 0 */ /* Only grab rw anonymous mappings */

struct region_list
{
    void * begin;
    void * end;
    int flags; /* Per defines, above */
    struct region_list * next;
};

struct region_list * new_region_list(pid_t pid, int flags);
struct region_list * free_region_list(struct region_list * rl);
