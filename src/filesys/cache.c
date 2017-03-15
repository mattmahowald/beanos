#include <debug.h>
#include "devices/timer.h"
#include "filesys/cache.h"
#include <string.h>
#include <stdio.h>
#include "threads/thread.h"
#include "threads/malloc.h"

#define BUFFER_SIZE 64

static void flush_thread (void *aux UNUSED);
static void flush (struct cache_entry *entry);
static size_t evict (void);

static struct cache_entry *add_to_cache (block_sector_t sector);
static struct cache_entry *get_cache_entry (block_sector_t sector);

void free_hash_entry (struct hash_elem *e, void *aux UNUSED);
unsigned cache_hash (const struct hash_elem *p_, void *aux UNUSED);
bool cache_less (const struct hash_elem *a_, const struct hash_elem *b_, 
                 void *aux UNUSED);
unsigned flush_hash (const struct hash_elem *p_, void *aux UNUSED);
bool flush_less (const struct hash_elem *a_, const struct hash_elem *b_, 
                 void *aux UNUSED);

static struct lock cache_lock;
static struct hash buffer_cache;
static struct hash flush_entries;
static size_t clock_hand;
static struct condition flush_complete;
static struct lock flusher_lock;
static bool done;
static struct cache_entry *entry_array;

void 
cache_init ()
{

  entry_array = malloc (BUFFER_SIZE * sizeof (struct cache_entry));

  hash_init (&buffer_cache, cache_hash, cache_less, NULL);
  hash_init (&flush_entries, flush_hash, flush_less, NULL);
  cond_init (&flush_complete);
  lock_init (&cache_lock);
  lock_init (&flusher_lock);
  done = false;
  clock_hand = 0;
  thread_create ("flusher", PRI_DEFAULT, flush_thread, NULL);
}

static void
flush_thread (void *aux UNUSED)
{
  for (;;)
    {
      timer_sleep (30);
      lock_acquire (&flusher_lock);
      lock_acquire (&cache_lock);
      size_t size = hash_size (&buffer_cache);
      lock_release (&cache_lock);
      size_t i;
      for (i = 0; i < size; i++)
        {
          struct cache_entry *entry = &entry_array [i];
          if (lock_try_acquire (&entry->lock))
            {
              if (entry->flags & DIRTY)
                {
                  entry->flags &= ~DIRTY;
                  block_write (fs_device, entry->sector, entry->data);
                }
              lock_release (&entry->lock);
            }
        }
      lock_release (&flusher_lock);
      if (done)
      {
        free (entry_array);
        return;
      }
    }
}

static void
flush (struct cache_entry *entry)
{
  struct flush_entry flush;
  flush.sector = entry->sector;
  hash_insert (&flush_entries, &flush.elem);
  lock_release (&cache_lock);
  block_write (fs_device, entry->sector, entry->data);
  lock_acquire (&cache_lock);
  hash_delete (&flush_entries, &flush.elem);
  cond_broadcast (&flush_complete, &cache_lock);
}

static size_t
evict ()
{
  struct cache_entry *evicted = NULL;
  size_t index;
  while (evicted == NULL)
    {
      struct cache_entry *entry = &entry_array[clock_hand];
      if (lock_try_acquire (&entry->lock))
        {
          if (!(entry->flags & ACCESSED))
            {
              evicted = entry;

              struct hash_entry tmp;
              tmp.sector = evicted->sector;
              struct hash_elem *e = hash_find (&buffer_cache, &tmp.elem);
              hash_delete (&buffer_cache, &tmp.elem);
              free (hash_entry (e, struct hash_entry, elem));
              if (evicted->flags & DIRTY)
                flush (evicted);

              index = clock_hand;
            }
          else
            {
              entry->flags &= ~ACCESSED;
              lock_release (&entry->lock);
            }
        }
        clock_hand = (clock_hand + 1) % BUFFER_SIZE;
    }

  return index;
}

static struct cache_entry *
add_to_cache (block_sector_t sector)
{
  struct cache_entry *entry;
  size_t size = hash_size (&buffer_cache);
  struct hash_entry *he = malloc (sizeof (*he));
  if (size != BUFFER_SIZE)
    {
      entry = &entry_array[size];
      he->array_index = size;
      lock_init (&entry->lock);
      lock_acquire (&entry->lock);
    }
  else
    {
      struct flush_entry tmp;
      tmp.sector = sector;
      struct hash_elem *elem = hash_find (&flush_entries, &tmp.elem);
      while (elem)
        {
          cond_wait (&flush_complete, &cache_lock);
          struct hash_entry tmp_cache;
          tmp_cache.sector = sector;
          struct hash_elem *e = hash_find (&buffer_cache, &tmp_cache.elem);
          if (e)
            {
              entry = &entry_array[hash_entry (elem, struct hash_entry, elem)->array_index];
              lock_acquire (&entry->lock);
              return entry;
            }
          elem = hash_find (&flush_entries, &tmp.elem);
        }
      he->array_index = evict ();
      entry = &entry_array [he->array_index];
    }

  entry->sector = sector;
  entry->flags = ACCESSED;

  he->sector = sector;
  struct hash_elem *e = hash_insert (&buffer_cache, &he->elem);
  
  ASSERT (!e);

  return entry;
}

static struct cache_entry *
get_cache_entry (block_sector_t sector)
{
  struct hash_entry tmp;
  struct cache_entry *entry;
  tmp.sector = sector;
  lock_acquire (&cache_lock);

  struct hash_elem *elem = hash_find (&buffer_cache, &tmp.elem);
  
  if (elem)
    {
      entry = &entry_array[hash_entry (elem, struct hash_entry, elem)->array_index];
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
cache_read (block_sector_t sector, void *buffer, size_t ofs, 
            size_t to_read)
{
  struct cache_entry *entry = get_cache_entry (sector);
  memcpy (buffer, entry->data + ofs, to_read);
  entry->flags |= ACCESSED;
  lock_release (&entry->lock);
}

/* Writes from the given buffer into given sector on disk. If the 
   sector is not already cached, sector is cached. */
void
cache_write (block_sector_t sector, const void *buffer, size_t ofs, 
            size_t to_write)
{
  struct cache_entry *entry = get_cache_entry (sector);
  memcpy (entry->data + ofs, buffer, to_write);
  entry->flags |= DIRTY | ACCESSED;
  lock_release (&entry->lock);
}


void 
free_hash_entry (struct hash_elem *e, void *aux UNUSED)
{  
  struct hash_entry *entry = hash_entry (e, struct hash_entry, elem);
  free (entry);
}

void 
cache_cleanup () 
{
  lock_acquire (&flusher_lock);
  done = true;
  lock_release (&flusher_lock);
  hash_destroy (&buffer_cache, free_hash_entry);
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
  const struct flush_entry *a = hash_entry (a_, struct flush_entry, elem);
  const struct flush_entry *b = hash_entry (b_, struct flush_entry, elem);

  return a->sector < b->sector;
}


/* Hash function for cache entry. */
unsigned 
flush_hash (const struct hash_elem *p_, void *aux UNUSED)
{
  const struct flush_entry *p = hash_entry (p_, struct flush_entry, elem);
  return hash_int (p->sector);
}

/* Comparator function for cache entry. */
bool 
flush_less (const struct hash_elem *a_, const struct hash_elem *b_, 
           void *aux UNUSED)
{
  const struct flush_entry *a = hash_entry (a_, struct flush_entry, elem);
  const struct flush_entry *b = hash_entry (b_, struct flush_entry, elem);

  return a->sector < b->sector;
}