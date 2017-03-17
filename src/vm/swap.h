#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <inttypes.h>
#include "devices/block.h"

typedef block_sector_t swapid_t;

void swap_init (void);
swapid_t swap_write_page (uint8_t *);
void swap_read_page (uint8_t *, swapid_t);

#endif /* vm/swap.h */