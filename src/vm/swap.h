#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <inttypes.h>
#include "devices/block.h"

typedef block_sector_t swapid_t;

void swap_init (void);
swapid_t swap_write_page (void *);
void swap_read_page (swapid_t);

#endif /* vm/swap.h */