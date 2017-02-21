#ifndef VM_FRAME_H
#define VM_FRAME_H

// imports
#include <bitmap.h>

// #defines, enums, typedefs
#define MAX_FRAME_TABLE_ENTRIES 1024

/* Big frame explanation comment. */

struct frame {
    int index;
    void *data;
};

/* Array and 8iju*/
void *frame_table[MAX_FRAME_TABLE_ENTRIES];
struct bitmap *frame_bitmap;

void frame_init (void);
void *frame_get_free (void);
void frame_free (void *);

#endif /* vm/frame.h */