/* Implements eviction algorithm? Or maybe that goes in timer but
   actually moving and fetching pages from the swap goes in here.

   Also remember to keep track of all evicted pages we write to swap
   in our supplementary page table.

   Another question is how is this going to be stored? What happens
   when we write to disk? Can we maintain a pointer or a file 
   descriptor or what? */
