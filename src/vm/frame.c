#include <debug.h>
#include "threads/palloc.h"
#include "threads/synch.h"
#include "vm/frame.h"
#include <stdio.h>
#include <list.h>
#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include "threads/thread.h"
#include "vm/page.h"

/* Frame table data structures. */
static struct list frame_free_list;
static struct list frame_used_list;

/* Locks used to synchronize the frame table. */
static struct lock free_lock;
static struct lock used_lock;

/* Clock hand for the eviction algorithm. */
static struct list_elem *clock_hand;

static struct frame *evict (void);

/* Initializes the system wide frame table and locks. Init pallocs as
	 many pages as possible and places them in the free list.*/
void 
frame_init ()
{
	lock_init (&free_lock);
	lock_init (&used_lock);
	list_init (&frame_free_list);
	list_init (&frame_used_list);
	swap_init ();

	/* Allocate frame structs for every pallocable page in the user pool. */
	for(;;)
		{
			void *addr = palloc_get_page (PAL_USER);
			if (!addr)
				break;
			struct frame *f = malloc (sizeof *f);
			f->paddr = addr;	
			f->spte = NULL;
			f->pinned = false;
			list_push_back (&frame_free_list, &f->elem);
		}
	clock_hand = NULL;
}

/* Atomically select a frame to evict using a two-hand clock algorithm,
	 ignoring pages that have been pinned. */
static struct frame *
evict ()
{
	/* Acquire the lock to prevent other evictors. */
	lock_acquire (&used_lock);
  
  /* Initialize iteration data. */
  if (clock_hand == NULL)
  	clock_hand = list_begin (&frame_used_list);
  struct list_elem *start = clock_hand;

	/* TODO This doesn't make any sense anymore. */
	if (clock_hand == list_end (&frame_used_list))
		return NULL;

  /* Iterate over every frame in the used list to choose one to evict. */
  do
  	{
      struct frame *f = list_entry (clock_hand, struct frame, elem);
			
			/* Atomically test that we can evict and then evict the frame. */
			lock_acquire (&f->spte->spte_lock);
			if (!f->pinned && !pagedir_is_accessed (f->spte->pd, f->spte->vaddr))
      	{
      		/* Pin the frame to ensure no other process will evict this 
      			 frame. */
      		f->pinned = true;	

      		/* Release the lock, as a frame has been selected. No other process
      			 can evict this frame due to the pinned flag, so another process
      			 can search for another frame to evict. */
		      lock_release (&used_lock);
		      page_unload (f->spte);
		      lock_release (&f->spte->spte_lock);
					f->spte = NULL;
		      return f;
		    }

		  /* Set the accessed bit to false if true. */
      if (!f->pinned && pagedir_is_accessed (f->spte->pd, f->spte->vaddr))
      	pagedir_set_accessed (f->spte->pd, f->spte->vaddr, false);
			lock_release (&f->spte->spte_lock);

			/* Tick clock. */
  		clock_hand = list_next (clock_hand);
  		if (clock_hand == list_end (&frame_used_list))
  			clock_hand = list_begin (&frame_used_list); 	
  	} 
  /* Only cycle the clock once to check if any frames have been freed. */
  while (clock_hand != start);

  lock_release (&used_lock);
  return NULL;
}

/* Find a free frame or evict a used frame, returning the pinned frame. */
struct frame *
frame_get ()
{
	struct frame *f = NULL;

	/* Loop between searching the free list and eviction algorithm to 
		 find a free frame. */
	while (!f)
		{
			/* Search the free list and move frame to used if found. */
			lock_acquire (&free_lock);
			if (!list_empty (&frame_free_list))
				{
					struct list_elem *e = list_pop_front (&frame_free_list);
					lock_release (&free_lock);
					f = list_entry (e, struct frame, elem);
					f->pinned = true;
					lock_acquire (&used_lock);
					list_push_back (&frame_used_list, &f->elem);
					lock_release (&used_lock);
					continue;
				}
			lock_release (&free_lock);

			/* No free fame available. Evict a used frame. */
			f = evict ();
		}

	return f;
}

/* Moves frame from used list to free list. */
void
frame_free (struct frame *f)
{
	lock_acquire (&used_lock);
	list_remove (&f->elem);
	lock_release (&used_lock);
	lock_acquire (&free_lock);
	list_push_back (&frame_free_list, &f->elem);
	lock_release (&free_lock);
}

/* Cleans all memory associated with the frame table, including the palloc'd
   pages. */
void
frame_cleanup ()
{
	while (!list_empty (&frame_free_list))
    {
      struct list_elem *e = list_pop_front (&frame_free_list);
      struct frame *f = list_entry (e, struct frame, elem);
      palloc_free_page (f->paddr);
      free (f);
    }	
}