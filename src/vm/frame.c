#include <debug.h>
#include "threads/palloc.h"
#include "threads/synch.h"
#include "vm/frame.h"
#include <stdio.h>
#include <list.h>
#include "threads/malloc.h"

#define HAND_DISTANCE 16

static struct lock free_lock;
static struct lock used_lock;
static struct list frame_free_list;
static struct list frame_used_list;

void 
frame_init ()
{
	lock_init (&free_lock);
	lock_init (&used_lock);
	list_init (&frame_free_list);
	list_init (&frame_used_list);

	for(;;)
		{
			void *addr = palloc_get_page (PAL_USER);
			if (!addr)
				break;
			struct frame *f = malloc (sizeof *f);
			f->paddr = addr;
			f->spte = NULL;
			list_push_back (&frame_free_list, &f->elem);
		}
}

// static void
// evict (void *frame)
// {
//   // if (pagedir_is_dirty (frame))
//   //   // write to disk
//   // return frame;
// }

// SYNCH
struct frame *
frame_get ()
{
	lock_acquire (&free_lock);
	struct list_elem *e = list_pop_front (&frame_free_list);
	lock_release (&free_lock);

	if (!e)
		PANIC ("RAN OUT OF MEM");

	struct frame *f = list_entry (e, struct frame, elem);
	lock_acquire (&used_lock);
	list_push_back (&frame_used_list, &f->elem);
	lock_release (&used_lock);
	return f;
}

void
frame_free (struct frame *f)
{
	printf("freeing frame corresponding to vaddr %p\n", f->vaddr);
	lock_acquire (&used_lock);
	list_remove (&f->elem);
	lock_release (&used_lock);
	lock_acquire (&free_lock);
	list_push_back (&frame_free_list, &f->elem);
	lock_release (&free_lock);
}

void
frame_free_paddr (void *paddr)
{

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