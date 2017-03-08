#include <debug.h>
#include "devices/timer.h"
#include "filesys/cache.h"
#include <string.h>
#include <stdio.h>
#include "threads/thread.h"
#include "threads/malloc.h"

#define BUFFER_SIZE 58

static void flush_thread (void *aux UNUSED);
static struct cache_entry *evict (void);
static struct cache_entry *add_to_cache (block_sector_t sector);
static struct cache_entry *get_cache_entry (block_sector_t sector);
void free_cache_entry (struct hash_elem *e, void *aux UNUSED);
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
	thread_create ("flusher", PRI_DEFAULT, flush_thread, NULL);
}


static void
flush_thread (void *aux UNUSED)
{
	for (;;)
		{
			timer_sleep (30);
			lock_acquire (&cache_lock);
			struct hash_iterator i;
      hash_first (&i, &buffer_cache);
      while (hash_next (&i))
        {
          struct cache_entry *entry = hash_entry (hash_cur (&i), struct cache_entry, elem);
          if (lock_try_acquire (&entry->lock))
	          {
	          	if (entry->dirty)
	          	{
								// TODO figure out a way to do this syncly
								block_write (fs_device, entry->sector, entry->data);
								entry->dirty = false;
	          	}
	          	lock_release (&entry->lock);
	          }
        }
      lock_release (&cache_lock);  
		}
}

static struct cache_entry *
evict ()
{
	if (clock_hand == NULL)
		{
			clock_hand = malloc (sizeof *clock_hand);
			hash_first (clock_hand, &buffer_cache);
		}
	struct cache_entry *evicted = NULL;
	while (evicted == NULL)
    {
      struct cache_entry *entry = hash_entry (hash_cur (clock_hand),
      																	   struct cache_entry, elem);
      if (lock_try_acquire (&entry->lock))
	      {
		      if (!entry->accessed)
		      	{
		      		evicted = entry;
		      		if (evicted->dirty)
		      			block_write (fs_device, entry->sector, entry->data);
		      	}
		      else
		      	{
		      		entry->accessed = false;
		      		lock_release (&entry->lock);
		      	}
	      }
      if (hash_next (clock_hand) == NULL)
      	hash_first (clock_hand, &buffer_cache);
    }

  hash_delete (&buffer_cache, &evicted->elem);
	
	return evicted;
}

static struct cache_entry *
add_to_cache (block_sector_t sector)
{

	struct cache_entry *entry;
	bool cache_full = (hash_size (&buffer_cache) == BUFFER_SIZE);
	entry = cache_full ? evict () : malloc (sizeof *entry);

	entry->sector = sector;
	entry->accessed = true;
	entry->dirty = false;
	lock_init (&entry->lock);
	lock_acquire (&entry->lock);
	
	struct hash_elem *e = hash_insert (&buffer_cache, &entry->elem);
	// TODO take this out later
	ASSERT (!e);

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
			lock_release (&cache_lock);
		}	
	else
		{
			entry = add_to_cache (sector);		
			lock_release (&cache_lock);
			block_read (fs_device, sector, entry->data);
		}
  return entry;
}

/* Reads from given sector into given buffer. If the sector is not
	 already cached, sector is cached. */
void
cache_read (block_sector_t sector, uint8_t *buffer, size_t ofs, 
						size_t to_read)
{
	// TODO implement read ahead once we figure out our inode implementation
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
	entry->dirty = true;
	lock_release (&entry->lock);
}


void 
free_cache_entry (struct hash_elem *e, void *aux UNUSED)
{  
  struct cache_entry *entry = hash_entry (e, struct cache_entry, elem);
  if (entry->dirty)
  	block_write (fs_device, entry->sector, entry->data);
  free (entry);
}

void 
cache_cleanup () 
{
  hash_destroy (&buffer_cache, free_cache_entry);
}

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
