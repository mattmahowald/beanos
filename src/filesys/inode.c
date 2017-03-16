#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include <stdio.h>
#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44
#define NUM_DIRECT 123
#define NUM_INDIRECT 128 
#define NUM_DOUBLY_INDIRECT NUM_INDIRECT * NUM_INDIRECT
#define MAX_SECTORS NUM_DIRECT + NUM_INDIRECT + NUM_DOUBLY_INDIRECT
#define META_BYTES 8
#define INDIRECT_OFFS BLOCK_SECTOR_SIZE - 12
#define DOUBLY_OFFS BLOCK_SECTOR_SIZE - 8
/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    block_sector_t direct[NUM_DIRECT];
    block_sector_t indirect;
    block_sector_t doubly_indirect;
    bool dir;                           /* Does this inode represent a directory. */  
    uint8_t unused[3];
  };

struct indirect_block
  {
    block_sector_t sectors[NUM_INDIRECT]; 
  };

static size_t allocate_direct_blocks (struct inode_disk *inode, size_t start, size_t end);
static size_t allocate_indirect_blocks (struct inode_disk *inode, size_t start, size_t end);
static size_t allocate_doubly_blocks (struct inode_disk *inode, size_t start, size_t end);
static bool extend_file (struct inode_disk *inode, size_t length);

static char zeros[BLOCK_SECTOR_SIZE];

bool
inode_is_inode (block_sector_t sector) {
  unsigned magic;
  cache_read (sector, &magic, 4, sizeof (unsigned));
  return (magic == INODE_MAGIC);
}

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    size_t length;
    bool dir;
    struct lock lock;
  };

static block_sector_t sector_index_to_sector (block_sector_t sector_index, const struct inode *inode);

/* Returns the block_sector_t that corresponds directly to the on-disk
   block holding the inode. */
static block_sector_t
sector_index_to_sector (block_sector_t sector_index, const struct inode *inode)
{
  size_t offset;
  block_sector_t indirect_block;
  
  if (sector_index < NUM_DIRECT)
    {
      /* In the case that the sector is direct, the block pointing to the
      cache entry is the inode itself. */
      offset = META_BYTES + sector_index * sizeof (block_sector_t);
      indirect_block = inode->sector;
    }
  else if (sector_index < NUM_DIRECT + NUM_INDIRECT)
    {
      /* In the case that the sector is indirect, read in the indirect
         block and calculate the offset into that block. */
      offset = (sector_index - NUM_DIRECT) * sizeof (block_sector_t);
      cache_read (inode->sector, &indirect_block, INDIRECT_OFFS, 
                  sizeof (block_sector_t));
    }
  else
    {
      /* In the case that the sector is doubly indirect, first read in the 
         doubly indirect block index and calculate the offset into that 
         block that corresponds to the entry for the indirect block.*/
      block_sector_t doubly_indirect;
      cache_read (inode->sector, &doubly_indirect, DOUBLY_OFFS, 
                  sizeof (block_sector_t));
      block_sector_t doubly_offset = (sector_index - NUM_DIRECT - NUM_INDIRECT) 
                                     / NUM_INDIRECT;

      /* Calculate the offset into the indirect block that corresponds to
         the direct block's entry and read in the indirect block index. */                               
      offset = ((sector_index - NUM_DIRECT - NUM_INDIRECT) % NUM_INDIRECT) 
               * sizeof (block_sector_t);

      cache_read (doubly_indirect, &indirect_block, 
                  doubly_offset * sizeof (block_sector_t), 
                  sizeof (block_sector_t));
    }

  /* Read and return the direct block index with the calculated values. */
  block_sector_t direct_block;
  cache_read (indirect_block, &direct_block, offset, sizeof (block_sector_t));
  return direct_block;
}

bool
inode_isdir (struct inode *inode)
{
  if (inode == NULL)
    return false;
  return inode->dir;
}

size_t
inode_num_open (struct inode *inode)
{
  return inode->open_cnt;
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  block_sector_t sector_index = pos / BLOCK_SECTOR_SIZE;
  return sector_index_to_sector (sector_index, inode);
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

static size_t
allocate_direct_blocks (struct inode_disk *inode, size_t start_sectors, 
                        size_t end_sectors)
{
  if (start_sectors >= NUM_DIRECT)
    return 0;

  size_t end_direct = end_sectors > NUM_DIRECT ? NUM_DIRECT : end_sectors;
  size_t num_direct = end_direct - start_sectors;
  if (!free_map_allocate_not_consecutive (num_direct, 
                                          &inode->direct[start_sectors]))
    return 0;

  size_t sector;
  for (sector = start_sectors; sector < end_direct; sector++)
    cache_write (inode->direct[sector], zeros, 0, BLOCK_SECTOR_SIZE);
  return num_direct;
}

static size_t
allocate_indirect_blocks (struct inode_disk *inode, size_t start_sectors, 
                          size_t end_sectors)
{
  if (start_sectors >= NUM_DIRECT + NUM_INDIRECT || end_sectors <= NUM_DIRECT)
    return 0;

  size_t start_indirect = start_sectors > NUM_DIRECT 
                          ? start_sectors - NUM_DIRECT : 0;
  size_t end_indirect = end_sectors > NUM_DIRECT + NUM_INDIRECT 
                        ? NUM_INDIRECT : end_sectors - NUM_DIRECT;
  size_t num_indirect = end_indirect - start_indirect;
  struct indirect_block *indirect = malloc (sizeof *indirect);
  if (!indirect)
    return 0;

  cache_read (inode->indirect, indirect, 0, BLOCK_SECTOR_SIZE);
  
  if (!free_map_allocate_not_consecutive (num_indirect, 
                                          &indirect->sectors[start_indirect]))
    {
      free (indirect);
      return 0;
    }
  size_t sector;
  for (sector = start_indirect; sector < end_indirect; sector++)
    cache_write (indirect->sectors[sector], zeros, 0, BLOCK_SECTOR_SIZE);
  cache_write (inode->indirect, indirect, 0, BLOCK_SECTOR_SIZE);
  
  free (indirect);
  return num_indirect;
}

static size_t
allocate_doubly_blocks (struct inode_disk *inode, 
            size_t start_sectors, size_t end_sectors)
{
  if (end_sectors <= NUM_DIRECT + NUM_INDIRECT)
    return 0;

  size_t start_offset = start_sectors <= NUM_DIRECT + NUM_INDIRECT ? 0 : start_sectors - NUM_DIRECT - NUM_INDIRECT;
  size_t end_offset = end_sectors - NUM_DIRECT - NUM_INDIRECT;

  struct indirect_block *doubly_indirect = malloc (sizeof *doubly_indirect);
  if (!doubly_indirect)
    return 0;

  cache_read (inode->doubly_indirect, doubly_indirect, 0, BLOCK_SECTOR_SIZE);

  struct indirect_block *temp_indirect = malloc (sizeof *temp_indirect);
  if (!temp_indirect)
  {
    free (doubly_indirect);
    return 0;
  }

  size_t first_indirect = start_offset / NUM_INDIRECT;
  size_t last_indirect = end_offset / NUM_INDIRECT;
  size_t num_indirect = last_indirect - first_indirect;

  size_t start = start_offset % NUM_INDIRECT;
  bool read = start != 0;

  size_t to_allocate = read ? num_indirect - 1 : num_indirect;
  size_t first_new_indirect = read ? first_indirect + 1 : first_indirect;

  if (!free_map_allocate_not_consecutive (to_allocate, &doubly_indirect->sectors[first_new_indirect]))
  {
    free (doubly_indirect);
    free (temp_indirect);
    return 0;
  }

  size_t sectors_written = 0;
  size_t indirect_index;
  for (indirect_index = first_indirect; indirect_index <= last_indirect; indirect_index++)  
  {
    size_t end = indirect_index == last_indirect ? end_offset % NUM_INDIRECT : NUM_INDIRECT;
    size_t total = end - start;
    if (read)
    cache_read (doubly_indirect->sectors[indirect_index], temp_indirect, 0, BLOCK_SECTOR_SIZE);
    if (!free_map_allocate_not_consecutive (total, &temp_indirect->sectors[start]))
    {
      free (doubly_indirect);
      free (temp_indirect);
      // also release allocated blocks
      return sectors_written;
    }
    cache_write (doubly_indirect->sectors[indirect_index], temp_indirect, 0, BLOCK_SECTOR_SIZE);
    sectors_written += NUM_INDIRECT;
    read = false;
    start = 0;
  }
  return sectors_written;
}


static bool
extend_file (struct inode_disk *inode, size_t new_size)
{
  size_t num_start_sectors = DIV_ROUND_UP (inode->length, BLOCK_SECTOR_SIZE);
  size_t num_end_sectors = DIV_ROUND_UP (new_size, BLOCK_SECTOR_SIZE);
  ASSERT (num_start_sectors <= num_end_sectors);
  if (num_start_sectors == num_end_sectors)
    return true;
  
  size_t num_allocated = 
    allocate_direct_blocks (inode, num_start_sectors, num_end_sectors) 
    + allocate_indirect_blocks (inode, num_start_sectors, num_end_sectors)
    + allocate_doubly_blocks (inode, num_start_sectors, num_end_sectors);
  
  if (num_allocated == (num_end_sectors - num_start_sectors))
    {
      inode->length = new_size;
      return true;
    }
  // else
    // ASSERT (4 == 5);

  return true;
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, bool isdir)
{ 
  struct inode_disk *disk_inode = NULL;
  // printf("Creating file of size %d\n", length);
  ASSERT (length >= 0);
  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);
  bool success = false;
  disk_inode = calloc (1, sizeof *disk_inode);  

  if (disk_inode != NULL)
    {      
      /* Set metadata. */
      disk_inode->length = 0;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->dir = isdir;
      /* Allocate sectors for our indirect and doubly indirect blocks */
      if (!free_map_allocate_not_consecutive (2, &disk_inode->indirect))
        {
          free (disk_inode);
          return false;
        }

      /* Extend file from 0 to requested length. */  
      success = extend_file (disk_inode, length);
      if (success)
        cache_write (sector, disk_inode, 0, BLOCK_SECTOR_SIZE);

      // TODO free map free indirect blocks on failure too
      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init (&inode->lock);
  cache_read (sector, &inode->length, 0, sizeof (size_t));
  cache_read (sector, &inode->dir, BLOCK_SECTOR_SIZE - 4, sizeof (bool));
  list_push_front (&open_inodes, &inode->elem);

  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    {
      lock_acquire (&inode->lock);
      inode->open_cnt++;
      lock_release (&inode->lock);
    }
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

static void
deallocate (struct inode *inode)
{
  size_t length = inode->length;
  size_t num_sectors = DIV_ROUND_UP (length, BLOCK_SECTOR_SIZE);
  size_t i;
  for (i = 0; i < num_sectors; i++)
    {
      free_map_release (sector_index_to_sector (i, inode), 1);
    }
  
  block_sector_t indirect;
  cache_read (inode->sector, &indirect, INDIRECT_OFFS, sizeof (block_sector_t));
  free_map_release (indirect, 1);

  block_sector_t doubly_indirect;
  cache_read (inode->sector, &doubly_indirect, DOUBLY_OFFS, sizeof (block_sector_t));
  int num_doubly = length - NUM_DIRECT - NUM_INDIRECT;
  if (num_doubly > 0)
    {
      struct indirect_block *doubly = malloc (sizeof *doubly);
      cache_read (doubly_indirect, doubly, 0, BLOCK_SECTOR_SIZE);
      for (i = 0; i < NUM_INDIRECT; i++)
        {
          block_sector_t sector = doubly->sectors[i];
          if (sector)
            free_map_release (doubly->sectors[i], 1);
          else
            break; /* Reached end of allocated if 0. */
        }
    }

  free_map_release (doubly_indirect, 1);
  if (inode->sector != 0 && inode->sector != 1) // TODO don't think we need this
    free_map_release (inode->sector, 1);
}


/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;
  /* Release resources if this was the last opener. */
  lock_acquire (&inode->lock);
  bool not_open = --inode->open_cnt == 0;
  lock_release (&inode->lock);

  if (not_open)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          // and cache_close
          // TODO add a free_map_release_all, as well as free map lock
          deallocate (inode);
          // free_map_release (inode->sector, 1);
          // free_map_release (inode->data.start,
          //                   bytes_to_sectors (inode->data.length)); 
        } else { 
          lock_acquire (&inode->lock);
          cache_write (inode->sector, &inode->length, 0, sizeof (size_t));
          lock_release (&inode->lock);
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  // TODO sync this up, if open cnt is 0 deallocate blocks
  ASSERT (inode != NULL);
  lock_acquire (&inode->lock);
  inode->removed = true;
  lock_release (&inode->lock);
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  // printf ("inode.c inode_read_at called\n");
  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;
      
      cache_read (sector_idx, buffer + bytes_read, sector_ofs, chunk_size);
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  // printf ("inode.c inode_read_at done\n");

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if an error occurs. */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  // printf ("inode.c inode_write_at called\n");

  // TODO potentially sync this
  if (inode->deny_write_cnt)
    return 0;

  if ((size_t) offset + size > inode->length) 
    {
      // printf ("time to extend");
      struct inode_disk *inode_disk = malloc (sizeof *inode_disk);
      if (!inode_disk)
        return 0;
      cache_read (inode->sector, inode_disk, 0, BLOCK_SECTOR_SIZE);
      if (!extend_file (inode_disk, offset + size))
        {
          // TODO maybe do cleanup here
          free (inode_disk);
          return 0;
        }
      cache_write (inode->sector, inode_disk, 0, BLOCK_SECTOR_SIZE);
      free (inode_disk);
      inode->length = offset + size;
      // printf("successfully extended\n");
      // TODO synch the above
    }
  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;
      cache_write (sector_idx, buffer + bytes_written, sector_ofs, chunk_size);
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  // printf ("inode.c inode_write_at done\n");

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  // TODO sync this
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  // TODO sync this as well
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  // TODO remove the length field from inode
  // TODO potentially sync this
  return inode->length;
}
