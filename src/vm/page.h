#ifndef VM_PAGE_H
#define VM_PAGE_H

// imports
#include <hash.h>
#include <inttypes.h>
#include <stdlib.h>
#include "filesys/file.h"

/* States the pages location. */
enum page_location
  {
    DISK,    	 /* . */
    SWAP,        /* . */
    FRAME,    	 /* . */
    ZERO         /* . */
  };

struct spte {
    struct hash_elem elem;
    enum page_location location;
    void *vaddr;

    struct file *f;
    bool writable;
    size_t read_bytes;
    size_t zero_bytes;
	off_t ofs;

    // file descriptor?
};

void page_init (struct hash *spt);
void page_allocate (bool);
void page_free (void *);



#endif /* vm/page.h */