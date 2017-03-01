#ifndef VM_FRAME_H
#define VM_FRAME_H

// imports
#include <bitmap.h>
#include <list.h>

// #defines, enums, typedefs
#define MAX_FRAME_TABLE_ENTRIES 1024

/* Frame Table

	 The frame struct holds the information to get, free, evict, */

struct frame 
{
    void *paddr;							/* The kernel virtual address of the frame. */
    struct spte *spte;				/* The spte to refer to pagedir and vaddr. */
    struct list_elem elem;		/* List element for the used and free lists. */
    bool pinned;							/* Pinned flag to keep frames from eviction. */
};

void frame_init (void);
struct frame *frame_get (void);
void frame_free (struct frame *);
void frame_cleanup (void);

#endif /* vm/frame.h */