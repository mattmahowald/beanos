#ifndef VM_FRAME_H
#define VM_FRAME_H

// imports
#include <bitmap.h>
#include <list.h>

// #defines, enums, typedefs
#define MAX_FRAME_TABLE_ENTRIES 1024

/* Big frame explanation comment. */

struct frame {
    void *paddr;
    struct spte *spte;
    struct list_elem elem;
    bool pinned;
    bool loaded;
};

/* Array and 8iju*/
void *frame_table[MAX_FRAME_TABLE_ENTRIES];
struct bitmap *frame_bitmap;

void frame_init (void);
struct frame *frame_get (void);
void frame_free (struct frame *);
void frame_cleanup (void);

#endif /* vm/frame.h */