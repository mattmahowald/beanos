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

#define HAND_DISTANCE 16

static struct lock free_lock;
static struct lock used_lock;
static struct list frame_free_list;
static struct list frame_used_list;
static struct list_elem *clock_hand;


static struct frame * evict (void);

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

/* Atomically selects a frame to evict using a two-hand clock algorithm,
	 ignoring pages that have been pinned. */
static struct frame *
evict ()
{
	lock_acquire (&used_lock);
  
  if (clock_hand == NULL)
  	clock_hand = list_begin (&frame_used_list);

  /* TODO This doesn't make any sense anymore*/
  if (clock_hand == list_end (&frame_used_list))
  	return NULL;

  struct list_elem *start = clock_hand;
  do
  	{
      struct frame *f = list_entry (clock_hand, struct frame, elem);
			
			if (!f->pinned && !pagedir_is_accessed (f->spte->pd, f->spte->vaddr))
      	{

      		f->pinned = true;	
		      lock_release (&used_lock);
		      page_unload (f->spte);
		      return f;
		    }

      if (!f->pinned && pagedir_is_accessed (f->spte->pd, f->spte->vaddr))
      		pagedir_set_accessed (f->spte->pd, f->spte->vaddr, false);

  		clock_hand = list_next (clock_hand);
  		if (clock_hand == list_end (&frame_used_list))
  			clock_hand = list_begin (&frame_used_list); 	
  	} 
  while (clock_hand != start);

  lock_release (&used_lock);
  return NULL;
}

// SYNCH
// FRAME GET returns a pinned frame
struct frame *
frame_get ()
{
	struct frame *f = NULL;
	while (!f)
		{
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
			f = evict ();
		}

	ASSERT (f);
	return f;
}

void
frame_free (struct frame *f)
{
	lock_acquire (&used_lock);
	// TODO remember this is somewhat racy
	ASSERT (!f->pinned);
	list_remove (&f->elem);
	lock_release (&used_lock);
	lock_acquire (&free_lock);
	list_push_back (&frame_free_list, &f->elem);
	lock_release (&free_lock);
}

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
  if (!list_empty (&frame_used_list))
  	{
  		while (!list_empty (&frame_used_list))
    	{
      		struct list_elem *e = list_pop_front (&frame_used_list);
      		struct frame *f = list_entry (e, struct frame, elem); 
      		printf("fuck thus shti %p\n", f->paddr);
      		printf("%p\n", f->spte->frame);
      	}
  		PANIC ("THIS LIST SHOULD BE EMPTY");
  	}	
  
}
// CLEAN UP FRAME TABLE SHIT