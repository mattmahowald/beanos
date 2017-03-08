#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"
#include "filesys/filesys.h"
#include <hash.h>
#include "threads/synch.h"

#define ACCESSED 0b00000001
#define DIRTY    0b00000010
#define PRESENT  0b00000100
#define METADATA 0b00001000

struct cache_entry
{
  struct hash_elem elem;
  block_sector_t sector;
  uint8_t data[BLOCK_SECTOR_SIZE];

  uint8_t flags;

  uint8_t num_users;
  struct lock entry_lock;
  struct condition flushing;
  struct lock flush_lock;
};

void cache_init (void);
void cache_read (block_sector_t, uint8_t *, size_t, size_t);
void cache_write (block_sector_t, const uint8_t *, size_t, size_t);
void cache_cleanup (void); 

#endif /* filesys/cache.h */


