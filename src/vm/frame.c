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

static struct frame * evict (void);

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
}

static struct frame *
evict ()
{
	struct list_elem *e;
	lock_acquire (&used_lock);
  for (e = list_begin (&frame_used_list); e != list_end (&frame_used_list);
       e = list_next (e))
    {
      struct frame *f = list_entry (e, struct frame, elem);
      if (pagedir_is_accessed (f->spte->pd, f->spte->vaddr))
      	{
      		pagedir_set_accessed (f->spte->pd, f->spte->vaddr, false);
      		continue;
      	}
      // if not pinned
      if (!f->pinned)
      	{
      		f->pinned = true;	
      		page_unload (f->spte);
					list_remove (&f->elem);
					lock_release (&used_lock);
      		f->pinned = false;
      		return f;
      	}	

    }
  lock_release (&used_lock);
  return NULL;
}

// SYNCH
struct frame *
frame_get ()
{
	struct frame *f = NULL;
	lock_acquire (&free_lock);	
	if (list_empty (&frame_free_list))
		{
			lock_release (&free_lock);
			while (!f)
				f = evict ();
		}
	else
		{
			struct list_elem *e = list_pop_front (&frame_free_list);
			lock_release (&free_lock);

			f = list_entry (e, struct frame, elem);
		}

	lock_acquire (&used_lock);
	list_push_back (&frame_used_list, &f->elem);
	lock_release (&used_lock);
	return f;
}

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
  	PANIC ("THIS LIST SHOULD NOT BE EMPTY");
  
}
// CLEAN UP FRAME TABLE SHIT