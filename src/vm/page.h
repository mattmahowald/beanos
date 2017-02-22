#ifndef VM_PAGE_H
#define VM_PAGE_H

// imports
#include <hash.h>
#include <inttypes.h>
#include <stdlib.h>
#include "filesys/file.h"

#define LAZY true

/* States the pages location. */
enum page_location
  {
    DISK,    	 /* . */
    SWAP,        /* . */
    ZERO         /* . */
  };

struct spte {
    struct hash_elem elem;

    enum page_location location;
    void *vaddr;
    struct frame *frame;

    struct file *file;
	off_t ofs;
    size_t read_bytes;
    size_t zero_bytes;

    bool writable;
};

void page_init (struct hash *spt);
bool page_add_spte (enum page_location, void *, struct file *, off_t, size_t, size_t, bool, bool);
bool page_load (void *);
void page_remove_spte (void *);



#endif /* vm/page.h */