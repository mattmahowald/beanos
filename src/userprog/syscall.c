#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/directory.h"
#include "filesys/inode.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include <string.h>
#include "vm/frame.h"
#include "vm/page.h"


static void syscall_handler (struct intr_frame *);
static void validate_string (const char *string);
static void validate_address (void * address, size_t size, bool writable);
static void load_and_pin (void *vaddr, size_t size);
static void unpin (void *vaddr, size_t size);
static void sys_halt (void);
static tid_t sys_exec (const char *cmd_line);
static bool sys_create (const char *file, unsigned initial_size);
static bool sys_remove (const char *file);
static int sys_open (const char *file);
static int sys_filesize (int fd);
static int sys_read (int fd, void *buffer, unsigned size);
static int sys_write (int fd, void *buffer, unsigned size);
static void sys_seek (int fd, unsigned position);
static unsigned sys_tell (int fd);
static void sys_close (int fd);
static tid_t sys_wait (tid_t tid);
static mapid_t sys_mmap (int fd, void *addr);
static void unmap (struct mmapped_file *mf);
static void sys_munmap (mapid_t mapping);
static bool sys_chdir (char *dir);
static bool sys_mkdir (char *dir);
static bool sys_readdir (int fd, char *name);
static bool sys_isdir (int fd);
static int sys_inumber (int fd);
static struct fd_to_file *get_file_struct_from_fd (int fd);
static struct fd_to_dir *get_dir_struct_from_fd (int fd);
static int allocate_fd (void);
static int allocate_mapid (void);

static struct lock filesys_lock; 
static struct lock fd_lock;
static struct lock mapid_lock;

static struct lock fd_lock;

void 
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&fd_lock);
  lock_init (&mapid_lock);
}

/* Helper function that validates the entirety of a string is mapped 
   user virtual address space. */
static void
validate_string (const char *string)
{
  unsigned cur_addr = (unsigned) string;
  for (;;)
    {
      if (is_kernel_vaddr ((char *) cur_addr))
        sys_exit (-1);
      
      struct spte *page = page_get_spte ((void *)cur_addr);
      if (!page)
        sys_exit (-1);

      page_load ((void *)cur_addr, !PIN);

      while (cur_addr++ % PGSIZE != 0)
        if (*(char *) cur_addr == '\0')
          return;
    }
}


/* Helper function that validates the passed pointer as a legal, user
   virtual space address, loading appropriate pages into memory. */
static void
validate_address (void *address, size_t size, bool writable)
{
  uint8_t *start = address;
  uint8_t *end = start + size - 1;
  
  if (!start)
    sys_exit (-1);


  if (is_kernel_vaddr (start) || is_kernel_vaddr (end))
    sys_exit (-1);

  uint8_t *cur_addr = round_to_page (start);

  while (cur_addr <= end) 
    {
      struct spte *page = page_get_spte (cur_addr);
      if (!page)
        {
          if (!(writable && page_extend_stack (cur_addr, 
                                               thread_current ()->esp)))
            sys_exit (-1); 
        }
      else 
        {
          if (writable && !page->writable)
            sys_exit (-1);
        }
      cur_addr += PGSIZE;
    }
}

/* Load and pin pages corresponding to the passed VADDR and SIZE. */
static void
load_and_pin (void *vaddr, size_t size)
{
  uint8_t *cur_addr = round_to_page (vaddr);
  uint8_t *end = vaddr + size - 1;

  while (cur_addr <= end) 
    {
      page_load (cur_addr, PIN);
      cur_addr += PGSIZE;
    } 
}

/* Unpins pages corresponding to the passed VADDR and SIZE. */
static void
unpin (void *vaddr, size_t size)
{
  uint8_t *cur_addr = round_to_page (vaddr);
  uint8_t *end = vaddr + size - 1;

  while (cur_addr <= end) 
    {
      struct spte *page = page_get_spte (cur_addr);
      page->frame->pinned = false;
      cur_addr += PGSIZE;
    } 
}

/* System call halt() shuts the operating system down. */
static void
sys_halt (void)
{
  shutdown_power_off ();
}

/* System call exit(status) kills the current thread with return status
   `status,' then indicates to its parent that it is finished before 
   printing an exit message and exiting through thread_exit. */
void
sys_exit (int status)
{
  struct thread *cur = thread_current ();
  
  /* Unmap any still mmapped files. */
  struct list *mfiles = &cur->mmapped_files;
  while (!list_empty (mfiles))
    {
      struct list_elem *mfile_e = list_pop_front (mfiles);
      struct mmapped_file *mfile = list_entry (mfile_e, struct mmapped_file, elem);
      unmap (mfile);
    }

  /* The way we implemented wait, we need to disable interrupts here. 
     Consider the case where, without disabling intterupts, cur's parent
     (P) is in process_exit during cur's (C) sys_exit call. C checks that 
     self != NULL, which it isn't, and enters the if statement. It is then
     preempted and P takes the processor. P checks that C is still running 
     (which it is, as running has not yet been set to false), sets C's
     self field to NULL and free's the child thread object that self 
     used to point to. At this point, C will attempt to access and change
     addresses that have already been freed by the parent, and sema-up a 
     sema that no longer exists. A shared lock struct could help minimize
     the risk of this condition, but would not eliminate it (the same 
     situation could occur, where after checking self != NULL C would attempt 
     to acquire a lock that no longer exists. */

  enum intr_level old_level = intr_disable ();  
  struct child_thread *self = cur->self;
  if (self != NULL)
    {
      self->exit_status = status;
      self->running = false;
      sema_up (&self->done);
    }
  intr_set_level (old_level);

  printf("%s: exit(%d)\n", cur->name, status);
  
  thread_exit ();
}

/* System call exec(cmd_line) executes the command line passed in by
   first validating the passed string then calling process execute
   and returning the return value. */
static int
sys_exec (const char *cmd_line)
{
  validate_string (cmd_line);
  return process_execute (cmd_line);
}

/* System call wait(tid) relies on process_wait to reap child process
   identified by tid. */
static tid_t
sys_wait (tid_t tid)
{
  return process_wait (tid);
}

/* System call create(file, initial_size) opens a file with the name and
   size passed in by first validating the address of the filename string 
   and then creating a file through the filesys_create function. */
static bool
sys_create (const char *file, unsigned initial_size)
{
  validate_string (file);

  bool success =  filesys_create (file, initial_size);

  return success;
}

/* System call remove(file) removes the file with the name and passed in 
   by first validating the address of the filename string and then removing 
   the file through filesys_remove function. */
static bool
sys_remove (const char *file)
{
  validate_string (file);

  bool success = filesys_remove (file);

  return success;
}

/* System call open(file) opens a file with the passed in name by
   first validating the address of the filename string and then safely
   making a call to filesys_open to get the file struct. This function
   additionally creates a fd_to_file struct in order for the process
   to be able to convert its fds to file pointers, and return the fd. */
static int
sys_open (const char *file)
{
  validate_string (file);

  bool isdir;

  void *data = filesys_open (file, &isdir);
  if (!data)
    return -1; 

  struct fd_to_file *fd_to_data = malloc (sizeof (struct fd_to_file));
  if (!fd_to_data)
    return -1;
  fd_to_data->fd = allocate_fd ();

  if (isdir)
    {
      ((struct fd_to_dir *) fd_to_data)->dir = (struct dir *) data;
      list_push_back (&thread_current ()->directories, 
                      &fd_to_data->elem);
    } 
  else
    {
      /* Define a fd_to_file struct for the process to lookup by fd. */
      fd_to_data->f = (struct file *) data;
      list_push_back (&thread_current ()->files, &fd_to_data->elem);
    }

  return fd_to_data->fd;
}

/* System call filesize(fd) gets the filesize of the file corresponding
   to the passed in fd. */
static int
sys_filesize (int fd)
{
  struct fd_to_file *f = get_file_struct_from_fd (fd);
  if (f == NULL)
    sys_exit (-1);
  int size = file_length (f);

  return size;
}

/* System call read(fd, buffer, size) reads from the file corresponding
   to the passed in fd by first validating the buffer, then determining 
   if the fd is STDIN, in which case it reads from the keyboard, or STDOUT,
   in which case it exits with status -1. Finally, the function finds the 
   file and reads into the passed in buffer, first loading and pinning the 
   pages associated with the buffer, returning the number of bytes read. */
static int
sys_read (int fd, void *buffer, unsigned size)
{ 
  validate_address (buffer, size, WRITABLE);
  int read = -1;

  /* Read from the keyboard if the fd refers to STDIN. */
  if (fd == STDIN_FILENO)
    {
      size_t i;
      for (i = 0; i < size; i++)  
        *((char *)buffer + i) = input_getc ();
      
      return size;
    }

  /* Return failure if the fd refers to STDOUT. */
  if (fd == STDOUT_FILENO)
    sys_exit (-1);

  /* Otherwise, find the file. */
  struct fd_to_file *fd_ = get_file_struct_from_fd (fd);
  if (!fd_)
    sys_exit (-1); 
  struct file *f = fd_->f;
  
  /* Load the buffer to read into, pinned so that it won't be evicted. */
  load_and_pin (buffer, size);

  /* Read from the file. */
  read = file_read (f, buffer, size);

  unpin (buffer, size);
  
  return read;
}

#define MAX_TO_PUTBUF 512

/* System call write(fd, buffer, size) writes to the file corresponding
   to the passed in fd by first validating the buffer, then determining 
   if the fd is STDOUT, in which case it writes to the console, or STDIN,
   in which case it exits with status -1. Finally, the function finds 
   the file and writes to it from the passed in buffer, first loading and 
   pinning the pages associated with the buffer, returning the number of 
   bytes written. */
static int
sys_write (int fd, void *buffer, unsigned size)
{
  validate_address (buffer, size, !WRITABLE);

  if (sys_isdir (fd))
    sys_exit (-1);
  int written = 0;
  
  if (fd == STDOUT_FILENO) 
    {
      char *cur = buffer;
      int remaining = size;
      while (remaining > 0)
        {
          putbuf (cur, remaining > MAX_TO_PUTBUF ? MAX_TO_PUTBUF : remaining);
          cur += MAX_TO_PUTBUF;
          remaining = size - (cur - (char *)buffer);
        }
      return size;
    }
    
  if (fd == STDIN_FILENO)
    sys_exit (-1);

  struct fd_to_file *fd_ = get_file_struct_from_fd (fd);
  if (!fd_)
    sys_exit (-1);
  struct file *f = fd_->f;


  load_and_pin (buffer, size);
  written = file_write (f, buffer, size);
  unpin (buffer, size);  
  written = file_write (f, buffer, size);
    
  return written;
}

/* System call seek(fd, position) seeks to the passed position in
   the file corresponding to the passed in fd. */
static void
sys_seek (int fd, unsigned position)
{
  struct file *f = get_file_struct_from_fd (fd)->f;
  if (f == NULL)
    sys_exit (-1);
  
  file_seek (f, position);
}

/* System call tell(fd) tells the user the position of the cursor
   in the file corresponding to the passed in fd. */
static unsigned
sys_tell (int fd)
{
  struct file *f = get_file_struct_from_fd (fd)->f;
  if (f == NULL)
    sys_exit (-1);
  
  return file_tell (f);
}  

/* System call close(fd) gets the file corresponding to the passed in 
   fd, then if the file exists, closes it and removes it from the 
   process's file list. */
static void
sys_close (int fd)
{
  struct fd_to_file *f = get_file_struct_from_fd (fd);
  struct fd_to_dir *d = get_dir_struct_from_fd (fd);

  
  if (f != NULL)
    {
      file_close (f->f);
      list_remove (&f->elem);
    }
  else if (d != NULL)
    {
      dir_close (d->dir);
      list_remove (&d->elem);
    }
  else
    sys_exit (-1);
}

/* System call chdir(dir) changes the directory of the current thread
   to the absolute or relative path denoted by DIR. */
static bool
sys_chdir (char *dir)
{ 
  validate_string (dir);

  struct dir *d = dir_lookup_path (dir);

  if (!d)
    return false;

  dir_close (thread_current ()->cwd);
  thread_current ()->cwd = d;

  return true;
}

/* System call mkdir(dir) creates a directory at location DIR. DIR can be 
   a relative or absolute path, but all intermediate directories must 
   exist. */
static bool
sys_mkdir (char *dir)
{
  validate_string (dir);
  size_t len = strlen (dir);
  char path[len], name[len];

  dir_split_path (dir, path, name);
  
  struct dir *d = dir_lookup_path (path);
  
  bool success = dir_create (d, name);
  dir_close (d);
  return success;
}

/* System call readdir(fd, name) reads the name of the directory */
static bool
sys_readdir (int fd, char *name)
{

  validate_address ((void **) name, 14);
  struct fd_to_dir *fd_ = get_dir_struct_from_fd (fd);
  if (fd_ == NULL)
    return false;

  bool success = dir_readdir (fd_->dir, name);
  return success;
}

static bool
sys_isdir (int fd)
{
  return get_dir_struct_from_fd (fd) != NULL;
}

static int 
sys_inumber (int fd)
{
  struct fd_to_file *fd_file = get_file_struct_from_fd (fd);
  if (fd_file)
    {
      struct inode *inode = file_get_inode (fd_file->f);
      if (inode == NULL)
        return -1;
      return inode_get_inumber (inode);
    }
  struct fd_to_dir *fd_dir = get_dir_struct_from_fd (fd);
  if (fd_dir == NULL)
    return -1;

  return dir_get_inumber (fd_dir->dir);
}

/* Iterate over the supplied file one page worth of bytes at a time, lazily 
   adding the pages to the supplementary page table. */
static bool
mmap_file (uint8_t *start_addr, size_t file_len, struct file *file)
{
  size_t bytes_mapped, bytes_to_map, file_bytes, zero_bytes;

  bytes_to_map = file_len;
  uint8_t *next_page = start_addr;
  bytes_mapped = 0;
  while (bytes_mapped <= file_len)
    {
      file_bytes = (bytes_to_map > PGSIZE) ? PGSIZE : bytes_to_map;
      zero_bytes = PGSIZE - file_bytes;
      /* Make sure file will not overwrite existing segments. */
      if (page_get_spte (next_page))
        {
          while (next_page != start_addr)
            {
              next_page -= PGSIZE;
              page_remove_spte (next_page);              
            }
          return false;
        }

      struct spte_file file_data;
      file_data.file = file;
      file_data.ofs = (off_t) bytes_mapped; 
      file_data.read = file_bytes;
      file_data.zero = zero_bytes;
      page_add_spte (DISK, next_page, file_data, WRITABLE, LAZY);

      bytes_mapped += PGSIZE;
      bytes_to_map -= PGSIZE;
      next_page += PGSIZE;
    }
  return true; 
}

/* System call mmap(fd, addr) maps the file designated by fd into memory at 
   the passed in ADDR and adds the file to the threads mmapped files list. */
static mapid_t 
sys_mmap (int fd, void *addr)
{
  size_t file_len;

  struct fd_to_file *f = get_file_struct_from_fd (fd);
  file_len = sys_filesize (fd);
  
  if (!f || file_len <= 0 || (uintptr_t) addr % PGSIZE != 0 || !addr ||
      fd == STDIN_FILENO || fd == STDOUT_FILENO)
      return MAP_FAILED;

  struct file *file_to_map = file_reopen (f->f);

  if (!mmap_file (addr, file_len, file_to_map))
    return MAP_FAILED;

  struct mmapped_file *mf = malloc (sizeof *mf);
  if (mf == NULL)
    PANIC ("Unable to malloc mmapped file struct");

  mf->id = allocate_mapid ();
  mf->start_vaddr = addr;
  mf->end_vaddr = (uint8_t *) addr + file_len;
  mf->file = file_to_map;
  list_push_back (&thread_current ()->mmapped_files, &mf->elem);

  return mf->id;
}

/* Unmaps a specific file by removing all the individual pages and freeing
   mmap struct itself. */
static void
unmap (struct mmapped_file *mf)
{
  uint8_t *next_page = mf->start_vaddr;
  while (next_page <= (uint8_t *) mf->end_vaddr)
    {
      /* page_remove_spte will write back to disk if dirty. */      
      page_remove_spte (next_page);
      next_page += PGSIZE;
    }
  file_close (mf->file);
  list_remove (&mf->elem);
  free (mf);
}

/* System call munmap(mapping) removes from memory the mmapped file
   associated with MAPPING by iterating over the list of all the
   current threads memory mapped files. */
static void 
sys_munmap (mapid_t mapping)
{
  struct list_elem *mfile_e;
  struct list *mfiles = &thread_current ()->mmapped_files;
  for (mfile_e = list_begin (mfiles); mfile_e != list_end (mfiles); 
       mfile_e = list_next (mfile_e))
    {
      struct mmapped_file *mf = list_entry (mfile_e, struct mmapped_file, 
                                               elem);
      if (mf->id == mapping)
        {
          unmap (mf);
          return;
        }
    } 
}

#define ONE_ARG 1
#define TWO_ARG 2
#define THREE_ARG 3

/* Handles the syscall interrupt, identifying the system call to be
   made then validating the stack and calling the correct system call. */
static void
syscall_handler (struct intr_frame *f) 
{
  int *esp = f->esp;
  validate_address (esp, sizeof (void *), WRITABLE);
  thread_current ()->esp = esp;
  switch (*esp)
    {
    case SYS_HALT:
      sys_halt ();
      break;
    case SYS_EXIT:
      validate_address (esp, (ONE_ARG + 1) * sizeof (void *), WRITABLE);
      sys_exit (esp[ONE_ARG]);
      break;
    case SYS_EXEC:
      validate_address (esp, (ONE_ARG + 1) * sizeof (void *), WRITABLE);
      f->eax = sys_exec (((char **)esp)[ONE_ARG]);
      break;
    case SYS_WAIT:
      validate_address (esp, (ONE_ARG + 1) * sizeof (void *), WRITABLE);
      f->eax = sys_wait (((tid_t *)esp)[ONE_ARG]);
      break;
    case SYS_CREATE:
      validate_address (esp, (TWO_ARG + 1) * sizeof (void *), WRITABLE);
      f->eax = sys_create (((char **)esp)[ONE_ARG], esp[TWO_ARG]);
      break;
    case SYS_REMOVE:
      validate_address (esp, (ONE_ARG + 1) * sizeof (void *), WRITABLE);
      f->eax = sys_remove (((char **)esp)[ONE_ARG]);
      break;
    case SYS_OPEN:
      validate_address (esp, (ONE_ARG + 1) * sizeof (void *), WRITABLE);
      f->eax = sys_open (((char **)esp)[ONE_ARG]);
      break;
    case SYS_FILESIZE:
      validate_address (esp, (ONE_ARG + 1) * sizeof (void *), WRITABLE);
      f->eax = sys_filesize (esp[ONE_ARG]);
      break;
    case SYS_READ:
      validate_address (esp, (THREE_ARG + 1) * sizeof (void *), WRITABLE);
      f->eax = sys_read (esp[ONE_ARG], ((void **)esp)[TWO_ARG], 
                         esp[THREE_ARG]);
      break;
    case SYS_WRITE:
      validate_address (esp, (THREE_ARG + 1) * sizeof (void *), WRITABLE);
      f->eax = sys_write (esp[ONE_ARG], ((void **)esp)[TWO_ARG], 
                          esp[THREE_ARG]);
      break;
    case SYS_SEEK:
      validate_address (esp, (TWO_ARG + 1) * sizeof (void *), WRITABLE);
      sys_seek (esp[ONE_ARG], esp[TWO_ARG]);
      break;
    case SYS_TELL:
      validate_address (esp, (ONE_ARG + 1) * sizeof (void *), WRITABLE);
      f->eax = sys_tell (esp[ONE_ARG]);
      break;
    case SYS_CLOSE:
      validate_address (esp, (ONE_ARG + 1) * sizeof (void *), WRITABLE);
      sys_close (esp[ONE_ARG]);
      break;
    case SYS_MMAP:
      validate_address (esp, (TWO_ARG + 1) * sizeof (void *), WRITABLE);
      f->eax = sys_mmap (esp[ONE_ARG], ((void **)esp)[TWO_ARG]);
      break;
    case SYS_MUNMAP:
      validate_address (esp, (ONE_ARG + 1) * sizeof (void *), WRITABLE);
      sys_munmap (esp[ONE_ARG]);
    case SYS_CHDIR:
      validate_address (esp, (ONE_ARG + 1) * sizeof (void *));
      f->eax = sys_chdir (((char **)esp)[ONE_ARG]);
      break;
    case SYS_MKDIR:
      validate_address (esp, (ONE_ARG + 1) * sizeof (void *));
      f->eax = sys_mkdir (((char **)esp)[ONE_ARG]);
      break;
    case SYS_READDIR:
      validate_address (esp, (TWO_ARG + 1) * sizeof (void *));
      f->eax = sys_readdir (esp[ONE_ARG], ((char **)esp)[TWO_ARG]);
      break;
    case SYS_ISDIR:
      validate_address (esp, (ONE_ARG + 1) * sizeof (void *));
      f->eax = sys_isdir (esp[ONE_ARG]);
      break;
    case SYS_INUMBER:
      validate_address (esp, (ONE_ARG + 1) * sizeof (void *));
      f->eax = sys_inumber (esp[ONE_ARG]);
      break;
    default:
      sys_exit (-1);
    }
}

/* Returns and increments the static next_fd value. */
static int
allocate_fd (void) 
{
  static int next_fd = STDOUT_FILENO + 1;
  int fd;

  lock_acquire (&fd_lock);
  fd = next_fd++;
  lock_release (&fd_lock);

  return fd;
}

/* Returns and increments the static mapid value. */
static mapid_t
allocate_mapid (void)
{
  static int next_mapid = 0;
  int mapid;

  lock_acquire (&mapid_lock);
  mapid = next_mapid++;
  lock_release (&mapid_lock);

  return mapid;
}

/* Given an fd, finds the file pointer through the current process's open
   file list. */
static struct fd_to_file *
get_file_struct_from_fd (int fd)
{
  struct list_elem *file_e;
  struct list *files = &thread_current ()->files;
  for (file_e = list_begin (files); file_e != list_end (files); 
       file_e = list_next (file_e))
    {
      struct fd_to_file *file = list_entry (file_e, struct fd_to_file, elem);
      if (file->fd == fd)
        return file;
    }
  return NULL; 
}

/* Given an fd, finds the dir pointer through the current process's open
   dir list. */
static struct fd_to_dir *
get_dir_struct_from_fd (int fd)
{
  if (fd <= STDOUT_FILENO) return NULL;
  struct list_elem *dir_e;
  struct list *dirs = &thread_current ()->directories;
  for (dir_e = list_begin (dirs); dir_e != list_end (dirs); 
       dir_e = list_next (dir_e))
    {
      struct fd_to_dir *d = list_entry (dir_e, struct fd_to_dir, elem);
      if (d->fd == fd)
        return d;
    }
  return NULL; 
}