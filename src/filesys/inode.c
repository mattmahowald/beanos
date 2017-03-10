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
#define NUM_DIRECT 124
#define NUM_INDIRECT 128 
#define NUM_DOUBLY_INDIRECT NUM_INDIRECT * NUM_INDIRECT
#define MAX_SECTORS NUM_DIRECT + NUM_INDIRECT + NUM_DOUBLY_INDIRECT
#define META_BYTES 8
#define INDIRECT_OFFS BLOCK_SECTOR_SIZE - 8
#define DOUBLY_OFFS BLOCK_SECTOR_SIZE - 4
/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
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

static size_t allocate_direct_blocks (struct inode_disk *inode, size_t start, size_t end);
static size_t allocate_indirect_blocks (struct inode_disk *inode, size_t start, size_t end);
static size_t allocate_doubly_blocks (struct inode_disk *inode, size_t start, size_t end);
static bool extend_file (struct inode_disk *inode, size_t length);

static char zeros[BLOCK_SECTOR_SIZE];


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
  };

static block_sector_t sector_index_to_sector (block_sector_t sector_index, const struct inode *inode);


static block_sector_t
sector_index_to_sector (block_sector_t sector_index, const struct inode *inode)
{
  size_t offset;
  block_sector_t direct_block;
  
  if (sector_index < NUM_DIRECT)
    {
      offset = META_BYTES + sector_index * sizeof (block_sector_t);
      direct_block = inode->sector;
    }
  else if (sector_index < NUM_DIRECT + NUM_INDIRECT)
    {
      offset = sector_index - NUM_DIRECT;
      cache_read (inode->sector, &direct_block, INDIRECT_OFFS, sizeof (block_sector_t));
    }
  else
    {
      offset = (sector_index - NUM_DIRECT - NUM_INDIRECT) % NUM_INDIRECT;
      block_sector_t doubly_indirect;
      cache_read (inode->sector, &doubly_indirect, DOUBLY_OFFS, sizeof (block_sector_t));
      block_sector_t doubly_offset = (sector_index - NUM_DIRECT - NUM_INDIRECT) / NUM_INDIRECT;
      cache_read (doubly_indirect, &direct_block, doubly_offset * sizeof (block_sector_t), sizeof (block_sector_t));
    }

  block_sector_t sector;
  cache_read (direct_block, &sector, offset, sizeof (block_sector_t));
  return sector;
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
allocate_direct_blocks (struct inode_disk *inode, size_t start_sectors, size_t end_sectors)
{
  if (start_sectors > NUM_DIRECT)
    return 0;

  size_t end_direct = end_sectors > NUM_DIRECT ? NUM_DIRECT : end_sectors;
  size_t num_direct = end_direct - start_sectors;
  if (!free_map_allocate_not_consecutive (num_direct, &inode->direct[start_sectors]))
    return 0;

  size_t sector;
  for (sector = start_sectors; sector < end_direct; sector++)
    cache_write (inode->direct[sector], zeros, 0, BLOCK_SECTOR_SIZE);

  return num_direct;
}

static size_t
allocate_indirect_blocks (struct inode_disk *inode, size_t start_sectors, size_t end_sectors)
{
  if (start_sectors >= NUM_DIRECT + NUM_INDIRECT || end_sectors <= NUM_DIRECT)
    return 0;

  size_t start_indirect = start_sectors > NUM_DIRECT ? start_sectors - NUM_DIRECT : 0;
  size_t end_indirect = end_sectors > NUM_DIRECT + NUM_INDIRECT ? NUM_INDIRECT : end_sectors - NUM_DIRECT;
  size_t num_indirect = end_indirect - start_indirect;
  struct indirect_block *indirect = malloc (sizeof *indirect);
  if (!indirect)
    return 0;

  cache_read (inode->indirect, indirect, 0, BLOCK_SECTOR_SIZE);
  
  if (!free_map_allocate_not_consecutive (num_indirect, &indirect->sectors[start_indirect]))
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
allocate_doubly_blocks (struct inode_disk *inode UNUSED, size_t start_sectors UNUSED, size_t end_sectors)
{
  if (end_sectors <= NUM_DIRECT + NUM_INDIRECT)
    return 0;
  else
    ASSERT (1 == 0);

  return 0;
  // size_t start_doubly = start_sectors > NUM_DIRECT + NUM_INDIRECT ? start_sectors : NUM_DIRECT + NUM_INDIRECT;
  // size_t num_doubly = end_sectors - start_doubly;
  // size_t start_indirect = (start_doubly - 1) / NUM_INDIRECT;
  // size_t num_indirect = DIV_ROUND_UP (num_doubly, NUM_INDIRECT);

  // struct indirect_block *doubly_indirect = malloc (sizeof *doubly_indirect);
  // if (!doubly_indirect)
  //   return 0;

  // cache_read (inode->doubly_indirect, doubly_indirect, 0, BLOCK_SECTOR_SIZE);

  // struct indirect_block *temp_indirect = malloc (sizeof *temp_indirect);
  // if (!temp_indirect)
  //   {
  //     free (doubly_indirect);
  //     return 0;
  //   }

  // if (start_indirect && !free_map_allocate_not_consecutive (num_doubly, doubly_indirect->sectors[start_indirect]))
  //   {
  //     free (temp_indirect);
  //     free (doubly_indirect);
  //     return NUM_DIRECT + NUM_INDIRECT;
  //   }
  
  // size_t needed_sectors = NUM_INDIRECT;
  // size_t i;
  // for (sector = 0; sector < num_doubly; sector++)
  //   {
  //     if (sector == num_doubly - 1)
  //       needed_sectors = sectors_left % NUM_INDIRECT;
  //     if (!free_map_allocate_not_consecutive (needed_sectors, temp_indirect->sectors))
  //       {
  //         free (temp_indirect);
  //         free (doubly_indirect);
  //         return sectors - sectors_left;
  //       }

  //     for (i = 0; i < needed_sectors; i++)
  //       cache_write (temp_indirect->sectors[i], zeros, 0, BLOCK_SECTOR_SIZE);

  //     cache_write (doubly_indirect->sectors[sector], temp_indirect, 0, BLOCK_SECTOR_SIZE);
  //     sectors_left -= needed_sectors;
  //   }

  // cache_write (inode->doubly_indirect, doubly_indirect, 0, BLOCK_SECTOR_SIZE);
  // free (temp_indirect);
  // free (doubly_indirect);

  // return sectors;
}


static bool
extend_file (struct inode_disk *inode, size_t new_size)
{
  size_t num_start_sectors = DIV_ROUND_UP (inode->length, BLOCK_SECTOR_SIZE);
  size_t num_end_sectors = DIV_ROUND_UP (new_size, BLOCK_SECTOR_SIZE);
  ASSERT (num_start_sectors <= num_end_sectors);
  if (num_start_sectors == num_end_sectors)
    return true;
  
  if (num_end_sectors > NUM_DIRECT)
    PANIC ("Only doing direct at the moment.");

  size_t num_allocated = 
  allocate_direct_blocks (inode, num_start_sectors, num_end_sectors) +
  allocate_indirect_blocks (inode, num_start_sectors, num_end_sectors) +
  allocate_doubly_blocks (inode, num_start_sectors, num_end_sectors);
  
  if (num_allocated == (num_end_sectors - num_start_sectors))
    {
      inode->length = new_size;
      return true;
    }
  else
    ASSERT (4 == 5);

  return true;
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
  // printf("created\n");
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  // printf ("inode.c inode_open called\n");
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
          // printf ("inode.c inode_open done (found)\n");

          return inode; 
        }
    }
  // printf ("inode.c inode_open found the inode\n");

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

  // printf ("inode.c inode_open done (created)\n");

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
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
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
  // TODO potentially sync this
  return inode->length;
}
