#include "devices/block.h"
#include <bitmap.h>
#include "vm/swap.h"
#include "vm/frame.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include <stdio.h>

#define SECTORS_PER_PAGE PGSIZE / BLOCK_SECTOR_SIZE

/* Swap partition, table, and lock. */
static struct block *swap_block;
static struct bitmap *swap_map;
static struct lock bitmap_lock;

static block_sector_t get_free_sectors (void);

/* Initializes the swap partition of the disk and the bitmap. */
void
swap_init ()
{
	swap_block = block_get_role (BLOCK_SWAP);
	if (swap_block == NULL)
		PANIC ("Unable to get swap block in swap.c");
	size_t num_sectors = block_size (swap_block);
	swap_map = bitmap_create (num_sectors);
	if (!swap_map)
		PANIC ("Unable to allocate bitmap in swap.c");
	lock_init (&bitmap_lock);
}

/* Writeas a page to the swap partition. */
swapid_t
swap_write_page (uint8_t *vaddr)
{
	block_sector_t sector = get_free_sectors ();
	
	size_t i;
	for (i = 0; i < SECTORS_PER_PAGE; i++)
		block_write (swap_block, sector + i, vaddr + i * BLOCK_SECTOR_SIZE);
	
	return sector;
}

/* Reads a page into the virtual address from swapid. */
void
swap_read_page (uint8_t *vaddr, swapid_t swapid)
{
	if (vaddr)
		{
			size_t i; 
			for (i = 0; i < SECTORS_PER_PAGE; i++)
			block_read (swap_block, swapid + i, vaddr + i * BLOCK_SECTOR_SIZE);
		}

	lock_acquire (&bitmap_lock);
	bitmap_set_multiple (swap_map, (block_sector_t) swapid, SECTORS_PER_PAGE, false);
	lock_release (&bitmap_lock);
}

/* */
static block_sector_t
get_free_sectors ()
{
	lock_acquire (&bitmap_lock);
	size_t index = bitmap_scan_and_flip (swap_map, 0, SECTORS_PER_PAGE, /* bits that are */ false);
	lock_release (&bitmap_lock);
	if (index == BITMAP_ERROR)
		PANIC ("No swap location available.");
	return (block_sector_t) index;
}