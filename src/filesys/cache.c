#include <debug.h>
#include "filesys/cache.h"
#include <string.h>
#include "threads/malloc.h"

#define BUFFER_SIZE 58

static struct cache_entry *evict (void);
static struct cache_entry *add_to_cache (block_sector_t sector);
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
      struct cache_entry *ce = hash_entry (hash_cur (clock_hand),
      																	   struct cache_entry, elem);
      lock_acquire (&ce->lock);
      if (!ce->accessed)
      	{
      		evicted = ce;
      	}
      if (ce->accessed)
      	{
      		ce->accessed = false;
      		lock_release (&ce->lock);
      	}

      if (hash_next (clock_hand))
      	hash_first (clock_hand, &buffer_cache);
    }
	return evicted;
}

static struct cache_entry *
add_to_cache (block_sector_t sector)
{

	struct cache_entry *ce;

	if (hash_size (&buffer_cache) == BUFFER_SIZE)
		ce = evict ();
	else
	  { 
			ce = malloc (sizeof *ce);
			ce->accessed = true;
			ce->dirty = false;
			struct hash_elem *e = hash_insert (&buffer_cache, &ce->elem);
		  if (e != NULL)
		    PANIC ("TODO FUCK OURSELVES");
			lock_init (&ce->lock);
			lock_acquire (&ce->lock);
		}


	ce->sector = sector;

	return ce;
}

/* Looks for */
void
cache_read (block_sector_t sector, uint8_t *buffer, size_t ofs, 
						size_t to_read)
{

	/* Find the element in the buffer. */
	struct cache_entry tmp;
 	struct cache_entry *ce;
	tmp.sector = sector;
	lock_acquire (&cache_lock);
 	struct hash_elem *elem = hash_find (&buffer_cache, &tmp.elem);
	ce = elem == NULL ? add_to_cache (sector) 
										: hash_entry (elem, struct cache_entry, elem);
  lock_release (&cache_lock);

  /* Copy data into the passed buffer. */
  memcpy (buffer, ce->data + ofs, to_read);
}

void
cache_write ()
{
	return;
  // block_write (fs_device, sector_idx, buffer + bytes_written);
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
