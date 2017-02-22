#include <debug.h>
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "vm/page.h"

void page_init (struct hash *spt);
void page_allocate (bool);
unsigned page_hash (const struct hash_elem *p_, void *aux UNUSED);
bool page_less (const struct hash_elem *a_, const struct hash_elem *b_, 
                void *aux UNUSED);

void
page_init (struct hash *spt)
{
  hash_init (spt, page_hash, page_less, NULL);
}

void 
page_allocate (bool writable)
{
    struct spte *spte = malloc (sizeof *spte);
    if (spte == NULL)
      PANIC ("Malloc failed in allocating a virtual page");

    /* Set meta-data. */
    spte->writable = writable;
    spte->vaddr = NULL;
    hash_insert (&thread_current ()->spt, &spte->elem);

    return spte->vaddr;
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