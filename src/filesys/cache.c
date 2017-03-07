#include <debug.h>
#include "filesys/cache.h"
#include <string.h>
#include "threads/malloc.h"

#define BUFFER_SIZE 58

static struct cache_entry *evict (void);
static struct cache_entry *add_to_cache (block_sector_t sector);
static struct cache_entry *get_cache_entry (block_sector_t sector);
unsigned cache_hash (const struct hash_elem *p_, void *aux UNUSED);
bool cache_less (const struct hash_elem *a_, const struct hash_elem *b_, 
           void *aux UNUSED);

static struct lock cache_lock;
static struct hash buffer_cache;
static struct hash_iterator *clock_hand;

void 
cache_init ()
{
	hash_init (&buffer_cache, cache_hash, cache_less, NULL);
	lock_init (&cache_lock);
	clock_hand = NULL;
}

static struct cache_entry *
evict ()
{
	if (clock_hand == NULL)
		hash_first (clock_hand, &buffer_cache);

	struct cache_entry *evicted = NULL;
	while (evicted != NULL)
    {
      struct cache_entry *entry = hash_entry (hash_cur (clock_hand),
      																	   struct cache_entry, elem);
      lock_acquire (&entry->lock);
      if (!entry->accessed)
      	{
      		evicted = entry;
      	}
      if (entry->accessed)
      	{
      		entry->accessed = false;
      		lock_release (&entry->lock);
      	}

      if (hash_next (clock_hand))
      	hash_first (clock_hand, &buffer_cache);
    }
	return evicted;
}

static struct cache_entry *
add_to_cache (block_sector_t sector)
{

	struct cache_entry *entry;

	if (hash_size (&buffer_cache) == BUFFER_SIZE)
		entry = evict ();
	else
	  { 
			entry = malloc (sizeof *entry);
			entry->accessed = true;
			entry->dirty = false;
			struct hash_elem *e = hash_insert (&buffer_cache, &entry->elem);
		  if (e != NULL)
		    PANIC ("TODO fuck this and fuck us -- race in cache.c");
			lock_init (&entry->lock);
			lock_acquire (&entry->lock);
		}

	entry->sector = sector;

	return entry;
}

static struct cache_entry *
get_cache_entry (block_sector_t sector)
{
	struct cache_entry tmp;
 	struct cache_entry *entry;
	tmp.sector = sector;
	lock_acquire (&cache_lock);
 	struct hash_elem *elem = hash_find (&buffer_cache, &tmp.elem);
	
	if (elem)
		{
			entry = hash_entry (elem, struct cache_entry, elem);
			lock_acquire (&entry->lock);
		}	
	else 
		entry = add_to_cache (sector);

  lock_release (&cache_lock);
  return entry;
}

/* Reads from given sector into given buffer. If the sector is not
	 already cached, sector is cached. */
void
cache_read (block_sector_t sector, uint8_t *buffer, size_t ofs, 
						size_t to_read)
{
	struct cache_entry *entry = get_cache_entry (sector);
  memcpy (buffer, entry->data + ofs, to_read);
  lock_release (&entry->lock);
}

/* Writes from the given buffer into given sector on disk. If the 
	 sector is not already cached, sector is cached. */
void
cache_write (block_sector_t sector, const uint8_t *buffer, size_t ofs, 
						size_t to_write)
{
	struct cache_entry *entry = get_cache_entry (sector);
	memcpy (entry->data + ofs, buffer, to_write);
	lock_release (&entry->lock);
}


// void 
// cache_cleanup (struct hash *spt) 
// {
//   hash_destroy (spt, free);
// }

/* Hash function for cache entry. */
unsigned 
cache_hash (const struct hash_elem *p_, void *aux UNUSED)
{
  const struct cache_entry *p = hash_entry (p_, struct cache_entry, elem);
  return hash_int (p->sector);
}

/* Comparator function for cache entry. */
bool 
cache_less (const struct hash_elem *a_, const struct hash_elem *b_, 
           void *aux UNUSED)
{
  const struct cache_entry *a = hash_entry (a_, struct cache_entry, elem);
  const struct cache_entry *b = hash_entry (b_, struct cache_entry, elem);

  return a->sector < b->sector;
}
