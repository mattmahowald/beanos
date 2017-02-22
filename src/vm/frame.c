#include <debug.h>
#include "threads/palloc.h"
#include "threads/synch.h"
#include "vm/frame.h"

static struct lock bitmap_lock;
static struct lock array_lock;

void 
frame_init ()
{
	size_t i, num_allocated = MAX_FRAME_TABLE_ENTRIES;
	for (i = 0; i < MAX_FRAME_TABLE_ENTRIES; i++) 
		{	
			frame_table[i] = palloc_get_page (PAL_USER);
			if (frame_table[i] == NULL) 
				{
					num_allocated = i;
					break;
				}
		}
	/* Initializes bitmap to MAX_FRAME_TABLE_ENTRIES bits set to true. */
	frame_bitmap = bitmap_create (num_allocated);
	bitmap_set_multiple (frame_bitmap, 0, num_allocated, true);

	lock_init (&bitmap_lock);
	lock_init (&array_lock);
}


// SYNCH

void *
frame_get ()
{

	// TODO: Panic if swap is full or no page eviction

	// find first bitmapped free and return it
	lock_acquire (&bitmap_lock);
	size_t index = bitmap_scan_and_flip (frame_bitmap, 0, 1, /* bit that is */ true);
	lock_release (&bitmap_lock);

	if (index != BITMAP_ERROR)
		return frame_table[index];

			// choose a frame to evict
			// remove references to the frame from any page
			// write page to file system or to swap
	PANIC ("FRAME_GET Failed.");
	return NULL;
}

void
frame_free (void *frame)
{
	size_t i;
	lock_acquire (&array_lock);
	for (i = 0; i < bitmap_size (frame_bitmap); i++)
		if (frame_table[i] == frame)
			{
				bitmap_set (frame_bitmap, i, false);
				break;
			}
	lock_release (&array_lock);
}

void
frame_cleanup ()
{
	int i = 0;
	while (frame_table[i] != NULL)
		palloc_free_page(frame_table[i++]);

	bitmap_destroy (frame_bitmap);
}
// CLEAN UP FRAME TABLE SHIT