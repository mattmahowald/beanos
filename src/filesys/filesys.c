#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/cache.h"
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/thread.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");
  cache_init ();
  
  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  thread_current ()->cwd = dir_open_root ();

  free_map_open ();

}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
  cache_cleanup ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) 
{
  block_sector_t inode_sector = 0;
  size_t len = strlen (name);
  char path[len], end[len];
  dir_split_path (name, path, end);

  struct dir *dir = dir_lookup_path (path);
  if (!dir)
    return false;

  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, !ISDIR)
                  && dir_add (dir, end, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
void *
filesys_open (const char *name, bool *isdir)
{
  size_t len = strlen (name);
  if (len == 0)
    return NULL;

  char path[len + 1], end[len + 1];

  if (strcmp(name, "/") == 0)
    {
      struct dir *root = dir_open_root();
      struct inode *inode = dir_get_inode (root);
      dir_close (root);
      if (isdir != NULL)
        *isdir = true;
      else
        return NULL;
      return (void *) dir_open (inode);
    }

  dir_split_path (name, path, end);
  
  struct dir *d = dir_lookup_path (path);

  struct inode *inode = NULL;

  if (d != NULL)
    dir_lookup (d, end, &inode);
  dir_close (d);

  if (isdir != NULL)
    *isdir = inode_isdir (inode);

  if (isdir != NULL && *isdir)
    return (void *) dir_open (inode);
  return (void *) file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  size_t len = strlen (name);
  char path[len], end[len];
  dir_split_path (name, path, end);

  struct dir *dir = dir_lookup_path (path);
  bool success = dir != NULL && dir_remove (dir, end);
  dir_close (dir); 

  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create_root (16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
