#include <debug.h>
#include <round.h>
#include <stdio.h>
#include <string.h>
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/syscall.h"
#include "vm/frame.h"
#include "vm/page.h"

#define PUSHA_OFFSET 32
#define STACK_LIMIT PHYS_BASE - (256 * PGSIZE)

inline void *round_to_page (void *vaddr);
static struct spte *hash_lookup_spte (struct hash *spt, void *vaddr);
unsigned page_hash (const struct hash_elem *p_, void *aux UNUSED);
bool page_less (const struct hash_elem *a_, const struct hash_elem *b_, 
                void *aux UNUSED);
void free_spte (struct hash_elem *e, void *aux UNUSED);

/* Rounds a virtual address down to its page base. */
inline void *
round_to_page (void *vaddr)
{
  return (void *) ROUND_DOWN ((uintptr_t) vaddr, PGSIZE);
}

/* Finds a virtual address in the supplementary page table hash.
   Input virtual address must be rounded to page. */
static struct spte *
hash_lookup_spte (struct hash *spt, void *vaddr)
{
  struct spte lookup;
  lookup.vaddr = vaddr;
  struct hash_elem *elem = hash_find (spt, &lookup.elem);

  return elem == NULL ? NULL : hash_entry (elem, struct spte, elem);
}

/* Initializes supplementary page table hash. */
void
page_init (struct hash *spt)
{
  hash_init (spt, page_hash, page_less, NULL);
}

/* Adds an entry to the current thread's supplementary page table. */
void 
page_add_spte (enum page_location loc, void *vaddr, struct spte_file file_data, 
               bool writable, bool lazy)
{
  ASSERT ((int) vaddr % PGSIZE == 0);

  /* Allocate and set meta-data for spt entry. */
  struct spte *spte = malloc (sizeof *spte);
  if (spte == NULL)
    PANIC ("Malloc failed in allocating a supplementary page table entry");

  spte->location = loc;
  spte->vaddr = vaddr;
  spte->frame = NULL;
  spte->writable = writable;
  spte->file_data = file_data;
  spte->loaded = false;
  /* Insert the spte into the spt, panicking on fail. */
  struct hash_elem *e = hash_insert (&thread_current ()->spt, &spte->elem);
  if (e != NULL)
    PANIC ("Element at address 0x%" PRIXPTR " already in table", 
           (uintptr_t) vaddr);

  /* If the page must not be loaded lazily, load a frame into the page. */
  if (!lazy)
    page_load (vaddr);
}

/* Find the spte that corresponds to addr. */
struct spte * 
page_get_spte (void *vaddr)
{
  void *page_base = round_to_page (vaddr);
  return hash_lookup_spte (&thread_current ()->spt, page_base);
}

/* Allocates a new page if and only if the fault address is within 
   PUSHA_OFFSET bytes of the esp and above the limit. */
bool
page_extend_stack (uint8_t *fault_addr, uint8_t *esp)
{
  /* Impose a limit on the total stack size, failing to grow if exceeding. */
  if (fault_addr < (uint8_t *) STACK_LIMIT)
    return false;

  /* Validate the write call is within PUSHA_OFFSET of the esp. */
  if (fault_addr + PUSHA_OFFSET < esp)
    return false;

  /* Allocate a new stack page. */
  struct spte_file no_file = {NULL, 0, 0, 0};
  page_add_spte (ZERO, round_to_page (fault_addr), no_file, WRITABLE, !LAZY);
  return true;
}

/* Remove the entry, freeing the frame if it exists, the hash entry, and the
   memory associated with the spte itself. */
void 
page_remove_spte (void *vaddr)
{
  /* Lookup vaddr in supplementary page table. */
  vaddr = round_to_page (vaddr);
  struct hash *spt = &thread_current ()->spt;
  struct spte *found = hash_lookup_spte (spt, vaddr);

  /* Free frame if it exists. */
  if (found->frame != NULL)
    frame_free (found->frame);

  /* Remove entry from supplementary page table. */
  hash_delete (spt, &found->elem);

  /* Free the entry itself. */
  free (found);
}

/* Loads a frame into the virtual address VADDR, */
bool 
page_load (void *vaddr)
{
  /* Lookup vaddr in supplementary page table. */
  vaddr = round_to_page (vaddr);
  struct hash *spt = &thread_current ()->spt;
  struct spte *spte = hash_lookup_spte (spt, vaddr);

  /* if the hash does not contain the entry, the memory access is invalid. */
  if (spte == NULL)
    return false;

  ASSERT (spte->frame == NULL)

  // NOTE: if swap is full, frame will panic
  /* Allocate a frame for the virtual page. */
  spte->frame = frame_get ();

  /* Determine where the entry and retrieve. */
  switch (spte->location)
    {
    case DISK:
      /* Read the file from the filesystem from the appropriate offset. */
      syscall_acquire_filesys_lock ();
      file_seek (spte->file_data.file, spte->file_data.ofs);
      int read = file_read (spte->file_data.file, spte->frame, 
                            spte->file_data.read);
      syscall_release_filesys_lock ();

      if (read != (int) spte->file_data.read)
        { 
          page_remove_spte (spte->vaddr);
          return false; 
        }

      /* Zero the remainder of the frame space for security. */
      memset ((uint8_t *) spte->frame + spte->file_data.read, 0, 
              spte->file_data.zero);
      break;
    case SWAP:
      PANIC ("Swap not implemented in page_load");
      break;
    case ZERO:
      // TODO needs to be done in the background
      memset (spte->frame, 0, PGSIZE);
      break;
    }
  /* Point the pagedir for the current thread to the appropriate frame. */
  pagedir_set_page (thread_current ()->pagedir, vaddr, spte->frame, 
                    spte->writable);
  spte->loaded = true;
  return true;
}

/* Cleanup the supplementary page table. */
void 
page_spt_cleanup (struct hash *spt) 
{
  hash_destroy (spt, free_spte);
}

/* Hash function for spte. */
unsigned 
page_hash (const struct hash_elem *p_, void *aux UNUSED)
{
  const struct spte *p = hash_entry (p_, struct spte, elem);
  return hash_bytes (&p->vaddr, sizeof p->vaddr);
}

/* Comparator function for spte. */
bool 
page_less (const struct hash_elem *a_, const struct hash_elem *b_, 
           void *aux UNUSED)
{
  const struct spte *a = hash_entry (a_, struct spte, elem);
  const struct spte *b = hash_entry (b_, struct spte, elem);

  return a->vaddr < b->vaddr;
}

/* Destructor for spte. */
void
free_spte (struct hash_elem *e, void *aux UNUSED)
{
  free (hash_entry (e, struct spte, elem));
}

// TODO remove
void
page_validate (struct hash *spt)
{
  struct hash_iterator i;
  int idx = 1;
  hash_first (&i, spt);
  while (hash_next (&i))
    {
      struct spte *spte = hash_entry (hash_cur (&i), struct spte, elem);
      char *loc;
      switch (spte->location) 
        {
        case (DISK) : loc = "DISK"; break;
        case (SWAP) : loc = "SWAP"; break;
        case (ZERO) : loc = "ZERO"; break;
        default     : loc = "UNKN";
        }
      printf ("PAGE TABLE ENTRY %d in %s\nVirtual Address  %p\nPhysical Address %p\nis %swritable\n\n", 
              idx++, loc, spte->vaddr, spte->frame, spte->writable ? "" : "not ");
    }
}