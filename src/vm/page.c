#include <debug.h>
#include <round.h>
#include <stdio.h>
#include <string.h>
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include "vm/page.h"


static inline void *round_to_page (void *vaddr);
static struct spte *hash_lookup_spte (struct hash *spt, void *vaddr);
unsigned page_hash (const struct hash_elem *p_, void *aux UNUSED);
bool page_less (const struct hash_elem *a_, const struct hash_elem *b_, 
                void *aux UNUSED);



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
  printf("%s\n", "about to init this hash");
  hash_init (spt, page_hash, page_less, NULL);
  printf("%s\n", "inited hash");
}

bool 
page_add_spte (enum page_location loc, void *vaddr, struct file *f, off_t ofs, 
               size_t read_bytes, size_t zero_bytes, bool writable)
{
  ASSERT ((int) vaddr % PGSIZE == 0);

  struct spte *spte = malloc (sizeof *spte);
  if (spte == NULL)
    PANIC ("Malloc failed in allocating a virtual page");

  /* Set meta-data. */
  spte->location = loc;
  spte->vaddr = vaddr;
  spte->frame = NULL;

  // This could load the frame itself if we want
  // spte->frame = lazy ? NULL : frame_get ();

  spte->file = f;
  spte->ofs = ofs;
  spte->read_bytes = read_bytes;
  spte->zero_bytes = zero_bytes;
  spte->writable = writable;
  struct hash_elem *e = hash_insert (&thread_current ()->spt, &spte->elem);
  if (e != NULL)
    PANIC ("Element at address 0x%" PRIXPTR " already in table", 
           (uintptr_t) vaddr);

  return true;
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
    return false;

  /* This page should not have a frame (due to eviction or laziness). */
  ASSERT (spte->frame == NULL)

  // NOTE: if swap is full, frame will panic
  /* Allocate a frame for the virtual page. */
  spte->frame = frame_get ();

  switch (spte->location)
    {
    case DISK:
      if (file_read (spte->file, spte->frame, spte->read_bytes) != (int) spte->read_bytes)
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