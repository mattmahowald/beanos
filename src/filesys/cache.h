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

  struct lock lock;
};

struct hash_entry
{
  struct hash_elem elem;
  block_sector_t sector;
  size_t array_index;
};

struct flush_entry
{
  struct hash_elem elem;
  block_sector_t sector;
};

void cache_init (void);
void cache_read (block_sector_t, void *, size_t, size_t);
void cache_write (block_sector_t, const void *, size_t, size_t);
void cache_cleanup (void); 
void cache_add_to_read_ahead (block_sector_t sector);


#endif /* filesys/cache.h */


