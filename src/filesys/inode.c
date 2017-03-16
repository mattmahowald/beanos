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

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. 

   The structure of the inode includes direct, indirect and doubly indirect
   blocks as well as some meta data, arranged *(not to scale, each entry is
   four bytes):

   +-----------------------------+
   |            length           |
   +-----------------------------+
   |            magic            |
   +-----------------------------+
   |          direct[0]          | -> a sector corresponding to a block of data
   +-----------------------------+
   |          direct[1]          | ->                 ""
   +-----------------------------+
   |          direct[2]          | ->                 ""
   +-----------------------------+
   |             .               |
   +-----------------------------+
   |             .               |
   +-----------------------------+
   |             .               |
   +-----------------------------+
   |      direct[NUM_DIRECT]     | ->                 ""
   +-----------------------------+   
   |                             |    a sector corresponding to an indirect
   |          indirect           | -> block that holds NUM_INDIRECT sectors
   |                             |    corresponding to blocks of data.
   +-----------------------------+     
   |                             |    a sector corresponding to an indirect
   |      doubly_indirect        | -> block that holds NUM_INDIRECT sectors,
   |                             |    each corresponding to indirect blocks.
   +-----------------------------+   
   |    dir + 3 bytes unused     |    
   +-----------------------------+   
*/

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* The number of direct blocks possible in the inode structure. */
#define NUM_DIRECT 123

/* The number of indirect blocks possible in the inode structure. */
#define NUM_INDIRECT 128 

/* The number of doubly indirect blocks possible in the inode structure. */
#define NUM_DOUBLY_INDIRECT NUM_INDIRECT * NUM_INDIRECT

/* The maximum number of blocks one inode can hold. */
#define MAX_SECTORS NUM_DIRECT + NUM_INDIRECT + NUM_DOUBLY_INDIRECT

/* The number of meta_fields above the data in the disk_inode. */
#define META_FIELDS 2

/* The offset of fields in the disk_inode */
#define DIRECT_OFFS META_FIELDS * sizeof(unsigned)
#define INDIRECT_OFFS (META_FIELDS + NUM_DIRECT) * sizeof (unsigned)
#define DOUBLY_OFFS (META_FIELDS + NUM_DIRECT + 1) * sizeof (unsigned)
struct inode_disk
  {
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    block_sector_t direct[NUM_DIRECT];  /* Direct blocks' sector numbers. */
    block_sector_t indirect;            /* Indirect block's sector number. */
    block_sector_t doubly_indirect;     /* Doubly indirect sector. */
    bool dir;                           /* Is a directory? */  
    uint8_t unused[3];
  };

/* An indirect block holds NUM_INDIRECT sectors, each pointing to either a
   direct block of the sector is an indirect block or an indirect block if
   the block is doubly indirect. */
struct indirect_block
  {
    block_sector_t sectors[NUM_INDIRECT]; 
  };

static size_t allocate_direct_blocks (struct inode_disk *inode, size_t start, 
                                      size_t end);
static size_t allocate_indirect_blocks (struct inode_disk *inode, size_t start, 
                                        size_t end);
static size_t allocate_doubly_blocks (struct inode_disk *inode, size_t start, 
                                      size_t end);
static bool extend_file (struct inode_disk *inode, size_t length);
static size_t extension_helper (struct inode *inode, off_t size, off_t offset);

static block_sector_t sector_index_to_sector (block_sector_t sector_index, 
                                              const struct inode *inode);

/* Reads magic to assure the inode has not corrupted. */
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
    struct list_elem elem;           /* Element in inode list. */
    block_sector_t sector;           /* Sector number of disk location. */
    int open_cnt;                    /* Number of openers. */
    bool removed;                    /* True if deleted, false otherwise. */
    int deny_write_cnt;              /* 0: writes ok, >0: deny writes. */
    size_t length;                   /* Length of the data in bytes. */
    bool dir;                        /* Inode is a directory. */
    struct lock lock;                /* Lock for struct inode's fields. */
    struct lock extension_lock;      /* Prevents to extensions at once. */
    struct lock dir_lock;            /* Locks dirent fields. */
  };

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;
static char zeros[BLOCK_SECTOR_SIZE];

/* Returns the block_sector_t that corresponds directly to the on-disk
   block holding the inode. */
static block_sector_t
sector_index_to_sector (block_sector_t sector_index, 
                        const struct inode *inode)
{
  size_t offset;
  block_sector_t indirect_block;
  
  if (sector_index < NUM_DIRECT)
    {
      /* In the case that the sector is direct, the block pointing to the
      cache entry is the inode itself. */
      offset = DIRECT_OFFS + sector_index * sizeof (block_sector_t);
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
      block_sector_t doubly_offs = (sector_index - NUM_DIRECT - NUM_INDIRECT) 
                                   / NUM_INDIRECT;

      /* Calculate the offset into the indirect block that corresponds to
         the direct block's entry and read in the indirect block index. */                               
      offset = ((sector_index - NUM_DIRECT - NUM_INDIRECT) % NUM_INDIRECT) 
               * sizeof (block_sector_t);

      cache_read (doubly_indirect, &indirect_block, 
                  doubly_offs * sizeof (block_sector_t), 
                  sizeof (block_sector_t));
    }

  /* Read and return the direct block index with the calculated values. */
  block_sector_t direct_block;
  cache_read (indirect_block, &direct_block, offset, sizeof (block_sector_t));
  return direct_block;
}

/* Returns true if the inode holds a directory, false if not. */
bool
inode_isdir (struct inode *inode)
{
  if (inode == NULL)
    return false;
  return inode->dir;
}

/* Returns the open count of an inode. */
size_t
inode_num_open (struct inode *inode)
{
  lock_acquire (&inode->lock);
  size_t cnt = inode->open_cnt;
  lock_release (&inode->lock);
  return cnt;
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
  struct indirect_block *doubly_indirect = NULL;
  struct indirect_block *temp_indirect = NULL;
  size_t sectors_written = 0;

  if (end_sectors <= NUM_DIRECT + NUM_INDIRECT)
    goto done;
  
  /* Index in doubly blocks of last existing and last to allocate sector. */
  size_t start_offset = start_sectors <= NUM_DIRECT + NUM_INDIRECT ? 0 : start_sectors - NUM_DIRECT - NUM_INDIRECT;
  size_t end_offset = end_sectors - NUM_DIRECT - NUM_INDIRECT;

  /* Read current doubly indirect sector into doubly_indirect. */
  doubly_indirect = malloc (sizeof *doubly_indirect);
  if (!doubly_indirect)
    goto done;

  cache_read (inode->doubly_indirect, doubly_indirect, 0, BLOCK_SECTOR_SIZE);

  /* Allocate temporary indirect block to hold newly allocated direct. */
  temp_indirect = malloc (sizeof *temp_indirect);
  if (!temp_indirect)
    goto done;

  /* Index of first and last indirect blocks in the doubly block to edit. */
  size_t first_indirect = start_offset / NUM_INDIRECT;
  size_t last_indirect = end_offset / NUM_INDIRECT;
  size_t to_allocate = last_indirect - first_indirect;
  if (first_indirect % NUM_INDIRECT == 0)
    to_allocate++;

  /* If not clean break for new indirect, we'll have to go in and edit an already allocated indirect block. */
  size_t start = start_offset % NUM_INDIRECT;
  bool read = start != 0;

  /* Allocate sectors for our new indirect blocks. */ 
  size_t first_new_indirect = read ? first_indirect + 1 : first_indirect;

  if (to_allocate > 0 && !free_map_allocate_not_consecutive (to_allocate, 
                               &doubly_indirect->sectors[first_new_indirect]))
    goto done;

  size_t indirect_index;
  for (indirect_index = first_indirect; indirect_index <= last_indirect; indirect_index++)  
  {
    /* Handle case where last block might not be entirely filled. */
    size_t end = indirect_index == last_indirect ? end_offset % NUM_INDIRECT 
                                                 : NUM_INDIRECT;

    size_t total = end - start; /* partially filled first block. */
    if (read)
      cache_read (doubly_indirect->sectors[indirect_index], temp_indirect, 0, BLOCK_SECTOR_SIZE);
    
    /* Allocate direct blocks in temporary indirect block. */
    if (total > 0 && !free_map_allocate_not_consecutive (total, 
                                    &temp_indirect->sectors[start]))
      goto done;
    
    /* Write zeros to each newly allocated sector. */
    size_t i;
    for (i = start; i < end; i++)
      cache_write (temp_indirect->sectors[i], zeros, 0, BLOCK_SECTOR_SIZE);
    
    /* Write temporary indirect block back to doubly indirect sector. */
    cache_write (doubly_indirect->sectors[indirect_index], temp_indirect, 0, 
                 BLOCK_SECTOR_SIZE);
    sectors_written += total;
    read = false;
    start = 0;
  }

  /* Write doubly indirect back to disk. */
  cache_write (inode->doubly_indirect, doubly_indirect, 0, BLOCK_SECTOR_SIZE);

 done:
  if (doubly_indirect)
    free (doubly_indirect);
  if (temp_indirect)
    free (temp_indirect);
  return sectors_written;
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
  lock_init (&inode->extension_lock);
  lock_init (&inode->dir_lock);
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
  if (not_open)
    list_remove (&inode->elem);
  lock_release (&inode->lock);

  if (not_open)
    {
      /* Remove from inode list and release lock. */
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          // and cache_close
          // TODO add a free_map_release_all, as well as free map lock
          deallocate (inode);
          // free_map_release (inode->sector, 1);
          // free_map_release (inode->data.start,
          //                   bytes_to_sectors (inode->data.length)); 
        }
      else
        {
          cache_write (inode->sector, &inode->length, 0, sizeof (size_t));
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
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
  size_t length = inode_length (inode);

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = length - offset;
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

  if (offset % BLOCK_SECTOR_SIZE == 0 && offset < inode_length (inode))
    cache_add_to_read_ahead (byte_to_sector(inode, offset));
  
  return bytes_read;
}

static bool
extend_file (struct inode_disk *inode, size_t new_size)
{
  size_t num_start_sectors = DIV_ROUND_UP (inode->length, BLOCK_SECTOR_SIZE);
  size_t num_end_sectors = DIV_ROUND_UP (new_size, BLOCK_SECTOR_SIZE);
  ASSERT (num_start_sectors <= num_end_sectors);
  if (num_start_sectors == num_end_sectors)
    {
      inode->length = new_size;
      return true;
    }
  
  size_t num_allocated = 
    allocate_direct_blocks (inode, num_start_sectors, num_end_sectors) 
    + allocate_indirect_blocks (inode, num_start_sectors, num_end_sectors)
    + allocate_doubly_blocks (inode, num_start_sectors, num_end_sectors);
  
  if (num_allocated == (num_end_sectors - num_start_sectors))
    {
      inode->length = new_size;
      return true;
    }
  else
    PANIC ("Should have allocated %d, instead allocated %d", num_end_sectors - num_start_sectors, num_allocated);

  return true;
}

static size_t
extension_helper (struct inode *inode, off_t size, off_t offset)
{
  lock_acquire (&inode->extension_lock);
  size_t length = inode->length;
  bool extend = (size_t) offset + size > inode->length;
  if (!extend)
    lock_release (&inode->extension_lock);
  
  lock_release (&inode->lock);
  if (extend)
    {
      struct inode_disk *inode_disk = malloc (sizeof *inode_disk);
      if (!inode_disk)
        {
          lock_release (&inode->extension_lock);
          return 0;
        }
        
      cache_read (inode->sector, inode_disk, 0, BLOCK_SECTOR_SIZE);
      if (!extend_file (inode_disk, offset + size))
        {
          // TODO maybe do cleanup here
          lock_release (&inode->extension_lock);
          free (inode_disk);
          return 0;
        }
      length = (size_t) offset + size;
      cache_write (inode->sector, inode_disk, 0, BLOCK_SECTOR_SIZE);
      free (inode_disk);
    }
  return length;
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

  lock_acquire (&inode->lock);
  if (inode->deny_write_cnt)
  {
    lock_release (&inode->lock);
    return 0;
  }
  size_t length = extension_helper (inode, size, offset);
  if (length == 0)
    return 0;
  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = length - offset;
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
  if (length > inode->length)
    {
      lock_acquire (&inode->lock);
      inode->length = length;
      lock_release (&inode->lock);
      lock_release (&inode->extension_lock);
    }

  return bytes_written;
}

void
inode_acquire_dir_lock (struct inode *inode)
{
  lock_acquire (&inode->dir_lock);
}

void
inode_release_dir_lock (struct inode *inode)
{
  lock_release (&inode->dir_lock);
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  lock_acquire (&inode->lock);
  inode->deny_write_cnt++;
  lock_release (&inode->lock);
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
  lock_acquire (&inode->lock);
  inode->deny_write_cnt--;
  lock_release (&inode->lock);
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (struct inode *inode)
{
  lock_acquire (&inode->lock);
  size_t len = inode->length;
  lock_release (&inode->lock);
  return len;
}
