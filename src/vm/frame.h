#ifndef VM_FRAME_H
#define VM_FRAME_H

// imports
#include <bitmap.h>

// #defines, enums, typedefs
#define FRAME_TABLE_ENTRIES 64

/* Big frame explanation comment. */

struct frame {
    bool free;
    void *data;
};

/* Array and */
struct frame *frame_table[FRAME_TABLE_ENTRIES];
struct bitmap *frame_table_free;

#endif /* vm/frame.h */