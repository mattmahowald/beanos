#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
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
#include "vm/page.h"


static void syscall_handler (struct intr_frame *);
static char *truncate_to_page (char *addr);
static void validate_string (const char *string);
static void validate_address (void * address, size_t size, bool writable);
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
static void sys_munmap (mapid_t mapping);
static struct fd_to_file *get_file_struct_from_fd (int fd);
static int allocate_fd (void);
static int allocate_mapid (void);

static struct lock filesys_lock; 
static struct lock fd_lock;
static struct lock mapid_lock;

void 
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&filesys_lock);
  lock_init (&fd_lock);
  lock_init (&mapid_lock);
}

/* Truncates address to the nearest multiple of PGSIZE. */
static char *
truncate_to_page (char *addr)
{
  return (char *)((unsigned) addr & -1*PGSIZE);
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

      if (!page->loaded)
        page_load ((void *)cur_addr);

      while (cur_addr++ % PGSIZE != 0)
        if (*(char *) cur_addr == '\0')
          return;
    }
}


/* Helper function that validates the passed pointer as a legal, user
   virtual space address. */
static void
validate_address (void *address, size_t size, bool writable)
{
  char *start = address;
  char *end = start + size - 1;
  
  if (!start)
    sys_exit (-1);

  if (is_kernel_vaddr (start) || is_kernel_vaddr (end))
    sys_exit (-1);

  char *cur_addr = truncate_to_page (start);

  while (cur_addr <= end) 
    {
      struct spte *page = page_get_spte (cur_addr);
      if (!page)
        sys_exit (-1); 
      if (writable && !page->writable)
        sys_exit (-1);

      if (!page->loaded)
        page_load (cur_addr);

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

  syscall_acquire_filesys_lock ();
  bool success =  filesys_create (file, initial_size);
  syscall_release_filesys_lock ();

  return success;
}

/* System call remove(file) removes the file with the name and passed in 
   by first validating the address of the filename string and then removing 
   the file through filesys_remove function. */
static bool
sys_remove (const char *file)
{
  validate_string (file);

  syscall_acquire_filesys_lock ();
  bool success = filesys_remove (file);
  syscall_release_filesys_lock ();

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

  syscall_acquire_filesys_lock ();
  struct file *f = filesys_open (file);
  syscall_release_filesys_lock ();
  if (!f)
    return -1; 

  /* Define a fd_to_file struct for the process to lookup by fd. */
  struct fd_to_file *user_file = malloc (sizeof (struct fd_to_file));
  if (!user_file)
    return -1;

  user_file->f = f;
  user_file->fd = allocate_fd ();

  list_push_back (&thread_current ()->files, &user_file->elem);

  return user_file->fd;
}

/* System call filesize(fd) gets the filesize of the file corresponding
   to the passed in fd. */
static int
sys_filesize (int fd)
{
  struct fd_to_file *f = get_file_struct_from_fd (fd);
  if (f == NULL)
    return -1;

  return file_length (f->f);
}

/* System call read(fd, buffer, size) reads from the file corresponding
   to the passed in fd by first validating the buffer, then determining 
   if the fd is STDIN, in which case it reads from the keyboard, or STDOUT,
   in which case it exits with status -1. Finally, the function finds the 
   file and reads into the passed in buffer, returning the number of bytes 
   read. */
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

  /* Otherwise, find the file and read from it. */
  struct fd_to_file *fd_ = get_file_struct_from_fd (fd);

  if (!fd_)
    sys_exit (-1); 
  struct file *f = fd_->f;
  
  syscall_acquire_filesys_lock ();
  read = file_read (f, buffer, size);
  syscall_release_filesys_lock (); 

  return read;
}

#define MAX_TO_PUTBUF 512

/* System call write(fd, buffer, size) writes to the file corresponding
   to the passed in fd by first validating the buffer, then determining 
   if the fd is STDOUT, in which case it writes to the console, or STDIN,
   in which case it exits with status -1. Finally, the function finds 
   the file and writes to it from the passed in buffer, returning the 
   number of bytes written. */
static int
sys_write (int fd, void *buffer, unsigned size)
{
  validate_address (buffer, size, !WRITABLE);

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

  syscall_acquire_filesys_lock ();
  written = file_write (f, buffer, size);
  syscall_release_filesys_lock ();
    
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
  
  syscall_acquire_filesys_lock ();
  file_seek (f, position);
  syscall_release_filesys_lock ();
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
  if (f == NULL)
    sys_exit (-1);
  
  syscall_acquire_filesys_lock ();
  file_close (f->f);
  syscall_release_filesys_lock ();
  
  list_remove (&f->elem);
}

static mapid_t 
sys_mmap (int fd, void *addr)
{
  // TODO maybe decompose this a little bit
  // TODO maybe turn file retreival into its own helper fn
  size_t file_len, bytes_mapped, bytes_to_map, file_bytes, zero_bytes;

  struct fd_to_file *f = get_file_struct_from_fd (fd);
  file_len = sys_filesize (fd);
  
  if (!f || file_len <= 0 || (uintptr_t) addr % PGSIZE != 0 || !addr ||
      fd == STDIN_FILENO || fd == STDOUT_FILENO)
      return MAP_FAILED;

  syscall_acquire_filesys_lock ();
  struct file *file_to_map = file_reopen (f->f);
  syscall_release_filesys_lock ();

  uint8_t *next_page = addr;
  bytes_mapped = 0;
  bytes_to_map = file_len;
  while (bytes_mapped < file_len)
    {
      file_bytes = (bytes_to_map > PGSIZE) ? PGSIZE : bytes_to_map;
      zero_bytes = PGSIZE - file_bytes;
      /* Make sure file will not overwrite existing segments. */
      if (page_get_spte (next_page))
        {
          // maybe instead return map_failed .. but then free what you already did? fuck
          return MAP_FAILED;
        }

      struct spte_file file_data;
      file_data.file = file_to_map;
      file_data.ofs = (off_t) bytes_mapped; 
      file_data.read = file_bytes;
      file_data.zero = zero_bytes;
      page_add_spte (DISK, next_page, file_data, WRITABLE, LAZY);

      bytes_mapped += PGSIZE;
      bytes_to_map -= PGSIZE;
      next_page += PGSIZE;
    }

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

void
syscall_unmap (struct mmapped_file *mf)
{
  uint8_t *next_page = mf->start_vaddr;
  while (next_page < (uint8_t *) mf->end_vaddr)
    {
      struct spte *page = page_get_spte (next_page);
      if (page->loaded)
        {
          if (pagedir_is_dirty (thread_current ()->pagedir, next_page))
            {
              syscall_acquire_filesys_lock ();
              file_seek (page->file_data.file, page->file_data.ofs);
              file_write (page->file_data.file, next_page, page->file_data.read);
              syscall_release_filesys_lock ();
            }
        }
      page_remove_spte (next_page);
      next_page += PGSIZE;
    }
}

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
          syscall_unmap (mf);
          syscall_acquire_filesys_lock ();
          file_close (mf->file);
          syscall_release_filesys_lock ();
          list_remove (mfile_e);
          free (mf);

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
  // printf("syscall received\n");

  int *esp = f->esp;
  validate_address (esp, sizeof (void *), WRITABLE);
  // printf("validated esp\n");
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
      break;
    default:
      sys_exit (-1);
    }
}

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

/* Acquires the coarse file system lock. Should be called before every filesys 
   operation. */
void 
syscall_acquire_filesys_lock ()
{
  lock_acquire (&filesys_lock);
}

/* Releases the coarse file system lock. Should be called after every filesys 
   operation. */
void 
syscall_release_filesys_lock ()
{
  lock_release (&filesys_lock);
}

