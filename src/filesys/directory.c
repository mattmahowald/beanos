#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/thread.h"

/* A directory. */
struct dir 
  {
    struct inode *inode;                /* Backing store. */
    off_t pos;                          /* Current position. */
  };

/* A single directory entry. */
struct dir_entry 
  {
    block_sector_t inode_sector;        /* Sector number of header. */
    char name[NAME_MAX + 1];            /* Null terminated file name. */
    bool in_use;                        /* In use or free? */
    bool dir;                           /* Is this a directory? */
  }; 

/* Creates the root directory. Returns true if successful, false on failure. */
bool
dir_create_root (size_t entry_cnt)
{
  struct dir root_dir;
  if (!inode_create (ROOT_DIR_SECTOR, entry_cnt * sizeof (struct dir_entry), ISDIR))
    return false;

  root_dir.inode = inode_open (ROOT_DIR_SECTOR);

  if (!root_dir.inode)
    return false;

  if (!dir_add (&root_dir, "..", ROOT_DIR_SECTOR)
      || !dir_add (&root_dir, ".", ROOT_DIR_SECTOR))
    // TODO inode delete?
    return false;

  inode_close (root_dir.inode);
  return true;
}


bool
dir_create (struct dir *parent, char *name)
{
  // TODO cleanup freemap and inode
  block_sector_t sector;
  if (!free_map_allocate (1, &sector))
    return false;

  struct dir new_dir;
  if (!inode_create (sector, sizeof (struct dir_entry), ISDIR))
    return false;
  new_dir.inode = inode_open (sector);
  if (!new_dir.inode)
    return false;
  if (!new_dir.inode)
    {
      free_map_release (sector, 1);
      return false;
    }

  if (!dir_add (parent, name, sector)
      || !dir_add (&new_dir, "..", inode_get_inumber (parent->inode))
      || !dir_add (&new_dir, ".", sector))
    // freemap release and inode delete
    return false;
  return true;
} 

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) 
{
  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && dir != NULL)
    {
      dir->inode = inode;
      dir->pos = 0;
      return dir;
    }
  else
    {
      inode_close (inode);
      free (dir);
      return NULL; 
    }
}



/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root (void)
{
  return dir_open (inode_open (ROOT_DIR_SECTOR));
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) 
{
  return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
  if (dir != NULL)
    {
      inode_close (dir->inode);
      free (dir);
    }
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) 
{
  return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp) 
{
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (e.in_use && !strcmp (name, e.name)) 
      {
        if (ep != NULL)
          *ep = e;
        if (ofsp != NULL)
          *ofsp = ofs;
        return true;
      }
  return false;
}


/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
            struct inode **inode) 
{
  struct dir_entry e;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  if (lookup (dir, name, &e, NULL))
    *inode = inode_open (e.inode_sector);
  else
    *inode = NULL;

  return *inode != NULL;
}

#define ROOT_SYMBOL '/'
#define DELIMIT_SYMBOL "/"

void 
dir_split_path (const char *path, char *dirpath, char *name)
{
  char *end = strrchr (path, ROOT_SYMBOL);
  if (!end)
    {
      *dirpath = '\0';
      strlcpy (name, path, strlen (path) + 1);
      return;
    }
  if (end == path)
    *dirpath = '/';
  else
    strlcpy (dirpath, path, end - path);

  *(dirpath + (end - path + 1)) = '\0';
  strlcpy (name, end + 1, strlen (end));
}


/* Returns an open dir. */
struct dir *
dir_lookup_path (char *pathname)
{
  struct dir *cur_dir;
  
  if(pathname[0] == ROOT_SYMBOL) 
    {
      cur_dir = dir_open_root ();
      pathname++;
    }
  else
    {
      cur_dir = dir_reopen (thread_current ()->cwd);
      if (pathname[0] == '\0')
        return cur_dir;
    }
  

  size_t len = strlen (pathname) + 1;
  char dirname[len];
  strlcpy (dirname, pathname, len);
  
  char *save_ptr, *subdir;
  struct inode *inode = NULL;
  for(subdir = strtok_r (dirname, DELIMIT_SYMBOL, &save_ptr); subdir != NULL; 
                      subdir = strtok_r (NULL, DELIMIT_SYMBOL, &save_ptr)) 
    {
      if (!dir_lookup (cur_dir, subdir, &inode))
        {
          dir_close (cur_dir);
          return NULL;
        }
      struct dir *next = dir_open (inode);
      dir_close (cur_dir);
      if (!next)
        return NULL;
      cur_dir = next;       
    }
  return cur_dir;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add (struct dir *dir, const char *name, block_sector_t inode_sector)
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Check NAME for validity. */
  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;

  /* Check that NAME is not in use. */
  if (lookup (dir, name, NULL, NULL))
    goto done;

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.
     
     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

 done:
  return success;
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) 
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Find directory entry. */
  if (!lookup (dir, name, &e, &ofs))
    goto done;

  /* Open inode. */
  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;

  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e) 
    goto done;

  /* Remove inode. */
  inode_remove (inode);
  success = true;

 done:
  inode_close (inode);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  struct dir_entry e;

  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) 
    {
      dir->pos += sizeof e;
      if (e.in_use)
        {
          strlcpy (name, e.name, NAME_MAX + 1);
          return true;
        } 
    }
  return false;
}
