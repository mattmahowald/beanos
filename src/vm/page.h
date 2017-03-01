#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include <inttypes.h>
#include <stdlib.h>
#include "filesys/file.h"
#include "userprog/pagedir.h"
#include "vm/swap.h"
#include "threads/synch.h"

#define LAZY true
#define WRITABLE true

// TODO im not sure why this wouldnt work as swap is imported
// typedef uint32_t swapid_t;

/* States the location of the page's frame. */
enum page_location
{
  DISK,  /* Page is a file located on disk. */
  EXEC,  /* Page is an executable file. */
  SWAP,  /* Page has been evicted and placed on the swap. */
  ZERO   /* Page does not exist anywhere, must be alloc'd and zero'd. */
};

/* File data held by an spte. */
struct spte_file
{
  struct file *file;
  off_t ofs;
  size_t read;
  size_t zero;
};

/* Supplementary Page Table Entry

   The spte struct holds data for a single entry in a process's supplementary 
   page table. The purpose of this table is two-fold:

     1. On a page fault, the supplementary page table supplies the kernel with
        the location of the frame, one of three locations defined by the 
        enumeration page_location. 
     2. The kernel uses the supplementary page table to cleanup up memory 
        associated with a process on exit.

   The supplementary page table is defined as a hash table within each 
   process. By placing the hash table as a member of the thread struct, 
   pintOS cannot support sharing, but this allowed the hash function to 
   use the user virtual address rather than the kernel virtual address. */
struct spte 
{
  struct hash_elem elem;        /* Hash element for the spt. */
  uint32_t *pd;                 /* Page directory for the spte's thread. */
  enum page_location location;  /* Location of the frame. */
  void *vaddr;                  /* User virtual address. */
  struct frame *frame;                  /* Kernel virtual address. */
  struct spte_file file_data;   /* File information. */
  bool writable;                /* Process has read-write privileges. */
  swapid_t swapid;
  struct lock spte_lock;
};


inline void *round_to_page (void *vaddr);
void page_add_spte (enum page_location, void *, struct spte_file, bool, bool);
void page_remove_spte (void *);
struct spte * page_get_spte (void *);
bool page_load (void *);
void page_unload (struct spte *);
// TODO these names are a little inconsistent
void page_init (struct hash *spt);
void page_spt_cleanup (struct hash *); 
// TODO uint8_t a bit inconsistent
bool page_extend_stack (uint8_t *, uint8_t *);

// TODO Remove
void page_validate (struct hash *);

#endif /* vm/page.h */