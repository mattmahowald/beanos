#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"
#include "filesys/filesys.h"
#include <hash.h>
#include "threads/synch.h"

struct cache_entry
{
	struct hash_elem elem;
	block_sector_t sector;
	uint8_t data[BLOCK_SECTOR_SIZE];
	bool dirty;
	bool accessed;
	struct lock lock;
};


/*
	 	sizeof struc list_elem 			       = 8
	+	sizeof uint32_t                    = 4
	+	sizeof uint8_t * BLOCK_SECTOR_SIZE = 512
	+	sizeof bool                        = 1
	+	sizeof struct lock                 = (4 + (16 + 4) + 8) = 32
  --------------------------------------------------------------
    sizeof struct cache_entry          = 557
*/

void cache_init (void);
void cache_read (block_sector_t, uint8_t *, size_t, size_t);
void cache_write (block_sector_t, const uint8_t *, size_t, size_t);

#endif /* filesys/cache.h */


