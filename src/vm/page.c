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

static inline void *round_to_page (void *vaddr);
static struct spte *hash_lookup_spte (struct hash *spt, void *vaddr);
unsigned page_hash (const struct hash_elem *p_, void *aux UNUSED);
bool page_less (const struct hash_elem *a_, const struct hash_elem *b_, 
                void *aux UNUSED);
void free_spte (struct hash_elem *e, void *aux UNUSED);

static inline void *
round_to_page (void *vaddr)
{
  return (void *) ROUND_DOWN ((uintptr_t) vaddr, PGSIZE);

}

static struct spte *
hash_lookup_spte (struct hash *spt, void *vaddr)
{
  struct spte lookup;
  lookup.vaddr = vaddr;
  struct hash_elem *elem = hash_find (spt, &lookup.elem);

  return elem == NULL ? NULL : hash_entry (elem, struct spte, elem);
}

void
page_init (struct hash *spt)
{
  hash_init (spt, page_hash, page_less, NULL);
}

bool 
page_add_spte (enum page_location loc, void *vaddr, struct file *f, off_t ofs, 
               size_t read_bytes, size_t zero_bytes, bool writable, bool lazy)
{
  ASSERT ((int) vaddr % PGSIZE == 0);

  struct spte *spte = malloc (sizeof *spte);
  if (spte == NULL)
    PANIC ("Malloc failed in allocating a virtual page");

  /* Set meta-data. */
  spte->location = loc;
  spte->vaddr = vaddr;
  spte->frame = NULL;

  spte->file = f;
  spte->ofs = ofs;
  spte->read_bytes = read_bytes;
  spte->zero_bytes = zero_bytes;
  spte->writable = writable;
  struct hash_elem *e = hash_insert (&thread_current ()->spt, &spte->elem);
  if (e != NULL)
    PANIC ("Element at address 0x%" PRIXPTR " already in table", 
           (uintptr_t) vaddr);

  if (!lazy)
    page_load (vaddr);

  return true;
}

bool
page_extend_stack (uint8_t *fault_addr, uint8_t *esp)
{
  if (fault_addr < (uint8_t *) STACK_LIMIT)
    return false;
  if (fault_addr + PUSHA_OFFSET >= esp)
    {
      page_add_spte (ZERO, round_to_page (fault_addr), NULL, 0, 0, 0, true, !LAZY);
      return true;
    }
  return false;
}

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

bool 
page_load (void *vaddr)
{
  /* Lookup vaddr in supplementary page table. */
  vaddr = round_to_page (vaddr);
  struct hash *spt = &thread_current ()->spt;
  struct spte *spte = hash_lookup_spte (spt, vaddr);

  // if does not exist, we want to page fault, return false
  if (spte == NULL)
  {
    return false;
  }

  /* This page should not have a frame (due to eviction or laziness). */
  ASSERT (spte->frame == NULL)

  // NOTE: if swap is full, frame will panic
  /* Allocate a frame for the virtual page. */
  spte->frame = frame_get ();
  switch (spte->location)
    {
    case DISK:
      syscall_acquire_filesys_lock ();
      file_seek (spte->file, spte->ofs);
      int read = file_read (spte->file, spte->frame, spte->read_bytes);
      syscall_release_filesys_lock ();
      if (read != (int) spte->read_bytes)
        { 
          page_remove_spte (spte->vaddr);
          return false; 
        }

      memset ((uint8_t *) spte->frame + spte->read_bytes, 0, spte->zero_bytes);
      break;
    case SWAP:
      PANIC ("Swap not implemented in page_load");
      break;
    case ZERO:
      // TODO needs to be done in the background
      memset (spte->frame, 0, PGSIZE);
      break;
    }
  pagedir_set_page (thread_current ()->pagedir, vaddr, spte->frame, 
                    spte->writable);
  return true;
}

unsigned 
page_hash (const struct hash_elem *p_, void *aux UNUSED)
{
    const struct spte *p = hash_entry (p_, struct spte, elem);
    return hash_bytes (&p->vaddr, sizeof p->vaddr);
}

bool 
page_less (const struct hash_elem *a_, const struct hash_elem *b_, 
           void *aux UNUSED)
{
  const struct spte *a = hash_entry (a_, struct spte, elem);
  const struct spte *b = hash_entry (b_, struct spte, elem);

  return a->vaddr < b->vaddr;
}

void
free_spte (struct hash_elem *e, void *aux UNUSED)
{
  free (hash_entry (e, struct spte, elem));
}

void 
page_spt_cleanup (struct hash *spt) 
{
  hash_destroy (spt, free_spte);
}


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