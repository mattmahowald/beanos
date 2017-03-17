#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"
#include "filesys/filesys.h"
#include <hash.h>
#include "threads/synch.h"

#define ACCESSED 0b00000001
#define DIRTY    0b00000010

/* An array of cache_entry structs forms the foundation of our cache. These
   structs hold the block sector data themselves, as well as some meta 
   information about to block. */
struct cache_entry
{
  struct hash_elem elem;           /* List elem. */
  block_sector_t sector;           /* Block sector whose data is held. */
  uint8_t data[BLOCK_SECTOR_SIZE]; /* Actual block data from disk. */
  uint8_t flags;                   /* Holds ACCESSED and DIRTY. */
  struct lock lock;                /* Lock for synch. */
};

/* A hash of hash_entry structs tracks those sectors currently active in the
   buffer cache. This struct allows our cache to find the physical location of
   that sector in the cache_entry array with array_index. */
struct hash_entry
{
  struct hash_elem elem;           /* Hash elem. */
  block_sector_t sector;           /* Block sector. */
  size_t array_index;              /* Index into cache_entry array. */
  bool present;
};

/* A hash of flush_entry structs tracks those sectors that may not be present
   in the cache_buffer_hash, but are currently being flushed back to disk. */
struct flush_entry
{
  struct hash_elem elem;           /* Hash elem. */
  block_sector_t sector;           /* Block sector. */
};

void cache_init (void);
void cache_read (block_sector_t, void *, size_t, size_t);
void cache_write (block_sector_t, const void *, size_t, size_t);
void cache_cleanup (void); 
void cache_add_to_read_ahead (block_sector_t sector);


#endif /* filesys/cache.h */


