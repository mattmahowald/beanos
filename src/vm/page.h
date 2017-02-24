#ifndef VM_PAGE_H
#define VM_PAGE_H

// imports
#include <hash.h>
#include <inttypes.h>
#include <stdlib.h>
#include "filesys/file.h"

#define LAZY true
#define WRITABLE true

/* States the pages location. */
enum page_location
{
  DISK,        /* . */
  SWAP,        /* . */
  ZERO         /* . */
};


struct spte_file
{
  struct file *file;
  off_t ofs;
  size_t read;
  size_t zero;
};

struct spte 
{
  struct hash_elem elem;        /* Hash element for the spt. */

  enum page_location location;  /* Location of the data. */
  void *vaddr;                  /* User virtual address. */
  void *frame;                  /* Kernel virtual address. */

  struct spte_file file_data;   /* File information. */

  bool writable;                /* */
};

void page_add_spte (enum page_location, void *, struct spte_file, bool, bool);
void page_remove_spte (void *);
bool page_contains_spte (void *);
bool page_load (void *);

// TODO these names are a little inconsistent
void page_init (struct hash *spt);
void page_spt_cleanup (struct hash *); 
bool page_extend_stack (uint8_t *, uint8_t *);

// TODO Remove
void page_validate (struct hash *);

#endif /* vm/page.h */