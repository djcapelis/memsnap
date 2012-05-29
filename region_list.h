#include <sys/types.h>
#include <unistd.h>

#include "djc_defines.h"

#define MODFLAG_GROWN       (1 << 0) /* Memory region has grown */                      /* bit 0 */
#define MODFLAG_SHRUNK      (1 << 1) /* Memory region has shrunk */                     /* bit 1 */
#define MODFLAG_NEW         (1 << 2) /* New memory region */                            /* bit 2 */
#define MODFLAG_DEL         (1 << 3) /* Removed memory region, address range = 0 */     /* bit 3 */

struct region_list
{
    void * begin;
    void * end;
    int modflags; /* Per defines, above */
    struct region_list * next;
};

struct region_list * new_region_list(pid_t pid);
struct region_list * free_region_list(struct region_list * rl);
struct region_list * update_region_list(pid_t pid, struct region_list * rl);
struct region_list * clean_region_list(struct region_list * rl);
