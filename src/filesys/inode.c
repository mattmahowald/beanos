#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44
#define NUM_DIRECT 124
#define NUM_INDIRECT 128 
#define NUM_DOUBLY_INDIRECT NUM_INDIRECT * NUM_INDIRECT

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    // TODO remove start
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    block_sector_t direct[NUM_DIRECT];
    block_sector_t indirect;
    block_sector_t doubly_indirect;
  };

struct indirect_block
  {
    block_sector_t sectors[NUM_INDIRECT]; 
  };

static block_sector_t sector_index_to_sector (block_sector_t sector_index, struct inode_disk *inode);

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
    // TODO remove data
  };

static block_sector_t
sector_index_to_sector (block_sector_t sector_index, struct inode_disk *inode)
{
  if (sector_index < NUM_DIRECT)
    return inode->direct[sector_index];
  else if (sector_index < NUM_DIRECT + NUM_INDIRECT)
    {
      struct indirect_block indirect;
      cache_read (inode->indirect, &indirect, 0, BLOCK_SECTOR_SIZE);
      return indirect.sectors[sector_index - NUM_DIRECT];
    }
  else if (sector_index < NUM_DIRECT + NUM_INDIRECT + NUM_DOUBLY_INDIRECT)
    {
      struct indirect_block doubly_indirect;
      struct indirect_block indirect;
      cache_read (inode->doubly_indirect, &doubly_indirect, 0, BLOCK_SECTOR_SIZE);
      block_sector_t double_index = (sector_index - NUM_DIRECT - NUM_INDIRECT) / NUM_INDIRECT;
      block_sector_t double_index_index = (sector_index - NUM_DIRECT - NUM_INDIRECT) % NUM_INDIRECT;
      cache_read (doubly_indirect.sectors[double_index], &indirect, 0, BLOCK_SECTOR_SIZE);
      return indirect.sectors[double_index_index];
    }
  
  // TODO remove this.. gracefil exit maybe
  PANIC ("We're gonna wanna sys_exit here.");

  return 0;
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  struct inode_disk disk_inode;
  cache_read (inode->sector, &disk_inode, 0, sizeof disk_inode);

  block_sector_t sector_index = pos / BLOCK_SECTOR_SIZE;
  return sector_index_to_sector (sector_index, &disk_inode);
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
allocate_sectors (struct inode_disk *inode, size_t sectors)
{
  size_t sector = 0;
  static char zeros[BLOCK_SECTOR_SIZE];
  
  struct indirect_block indirect;
  struct indirect_block doubly_indirect;

  if (sectors > NUM_DIRECT)
    {
      // TODO dont just always return false
      if (!free_map_allocate (1, &inode->indirect))
        return sector;
    }
  if (sectors > NUM_DIRECT + NUM_INDIRECT)
    {
      if (!free_map_allocate (1, &inode->doubly_indirect))
        return sector;
    }

  while (sector < sectors && sector < NUM_DIRECT)
    {
      if (!free_map_allocate (1, &inode->direct[sector]))
        return sector;
      
      cache_write (inode->direct[sector], zeros, 0, BLOCK_SECTOR_SIZE);
      sector++;
    }

  sector = 0;
  
  while (sector + NUM_DIRECT < sectors && sector < NUM_INDIRECT)
    {
      if (!free_map_allocate (1, &indirect.sectors[sector]))
        return sector + NUM_DIRECT;

      cache_write (indirect.sectors[sector], zeros, 0, BLOCK_SECTOR_SIZE);
      sector++;
    }
  
  sector = 0;
  
  struct indirect_block temp_indirect;
  while (sector + NUM_DIRECT + NUM_INDIRECT < sectors && sector < NUM_DOUBLY_INDIRECT)
    {
      size_t sector_index = sector / NUM_INDIRECT;
      size_t off = sector % NUM_INDIRECT;
      if (off == 0)
        {
          if (!free_map_allocate (1, &doubly_indirect.sectors[sector_index]))
            return sector + NUM_DIRECT + NUM_INDIRECT;
        }

      if (!free_map_allocate (1, &temp_indirect.sectors[off]))
        return sector + NUM_DIRECT + NUM_INDIRECT;

      cache_write (temp_indirect.sectors[off], zeros, 0, BLOCK_SECTOR_SIZE);

      sector++;

      if (off + 1 == NUM_INDIRECT)
        cache_write (doubly_indirect.sectors[sector_index], &temp_indirect, 0, BLOCK_SECTOR_SIZE);
    }

  return sectors;

}


/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);
  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);  

  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      size_t sectors_allocated = allocate_sectors (disk_inode, sectors);
      ASSERT (sectors_allocated == sectors);
      // TODO if (sectors_allocated != sectors)
      //    for each sector from 0 to sectors allocated - 1
      //      free map release
      cache_write (sector, disk_inode, 0, BLOCK_SECTOR_SIZE);
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
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  cache_read (sector, &inode->length, 0, sizeof (size_t));

  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  // TODO sync this
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
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
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          // and cache_close
          // TODO add a free_map_release_all

            ;
          // free_map_release (inode->sector, 1);
          // free_map_release (inode->data.start,
          //                   bytes_to_sectors (inode->data.length)); 
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  // TODO sync this up
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

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

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  // TODO potentially sync this
  if (inode->deny_write_cnt)
    return 0;

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
  // TODO potentially sync this
  return inode->length;
}
