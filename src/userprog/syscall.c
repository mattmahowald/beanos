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


static void syscall_handler (struct intr_frame *);
static char *truncate_to_page (char *addr);
static void validate_string (const char *string);
static void validate_address (void * address, size_t size);
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
static bool sys_chdir (char *dir);
static bool sys_mkdir (char *dir);
static bool sys_readdir (int fd, char *name);
static bool sys_isdir (int fd);
static int sys_inumber (int fd);

static struct fd_to_file *get_file_struct_from_fd (int fd);
static struct fd_to_dir *get_dir_struct_from_fd (int fd);
static int allocate_fd (void);

static struct lock fd_lock;

void 
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&fd_lock);
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
  unsigned *page_dir = thread_current ()-> pagedir;
  for (;;)
    {
      if (is_kernel_vaddr ((char *) cur_addr))
        sys_exit (-1);
      if (!pagedir_get_page (page_dir, (char *) cur_addr))
        sys_exit (-1);
      while (cur_addr++ % PGSIZE != 0)
        if (*(char *) cur_addr == '\0')
          return;
    }
}


/* Helper function that validates the passed pointer as a legal, user
   virtual space address. */
static void
validate_address (void *address, size_t size)
{
  char *start = address;
  char *end = start + size - 1;
  
  if (!start)
    sys_exit (-1);


  if (is_kernel_vaddr (start) || is_kernel_vaddr (end))
    sys_exit (-1);

  unsigned *page_dir = thread_current ()-> pagedir;
  char *cur_addr = truncate_to_page (start);

  while (cur_addr <= end) 
    {
      if (!pagedir_get_page (page_dir, cur_addr))
        sys_exit (-1);

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
  struct file *f = get_file_struct_from_fd (fd)->f;
  if (f == NULL)
    sys_exit (-1);
  int size = file_length (f);

  return size;
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
  validate_address (buffer, size);

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
  
  read = file_read (f, buffer, size);

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
  validate_address (buffer, size);

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

  // TODO validate name
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

#define ONE_ARG 1
#define TWO_ARG 2
#define THREE_ARG 3

/* Handles the syscall interrupt, identifying the system call to be
   made then validating the stack and calling the correct system call. */

static void
syscall_handler (struct intr_frame *f) 
{
  int *esp = f->esp;
  validate_address (esp, sizeof (void *));

  switch (*esp)
    {
    case SYS_HALT:
      sys_halt ();
      break;
    case SYS_EXIT:
      validate_address (esp, (ONE_ARG + 1) * sizeof (void *));
      sys_exit (esp[ONE_ARG]);
      break;
    case SYS_EXEC:
      validate_address (esp, (ONE_ARG + 1) * sizeof (void *));
      f->eax = sys_exec (((char **)esp)[ONE_ARG]);
      break;
    case SYS_WAIT:
      validate_address (esp, (ONE_ARG + 1) * sizeof (void *));
      f->eax = sys_wait (((tid_t *)esp)[ONE_ARG]);
      break;
    case SYS_CREATE:
      validate_address (esp, (TWO_ARG + 1) * sizeof (void *));
      f->eax = sys_create (((char **)esp)[ONE_ARG], esp[TWO_ARG]);
      break;
    case SYS_REMOVE:
      validate_address (esp, (ONE_ARG + 1) * sizeof (void *));
      f->eax = sys_remove (((char **)esp)[ONE_ARG]);
      break;
    case SYS_OPEN:
      validate_address (esp, (ONE_ARG + 1) * sizeof (void *));
      f->eax = sys_open (((char **)esp)[ONE_ARG]);
      break;
    case SYS_FILESIZE:
      validate_address (esp, (ONE_ARG + 1) * sizeof (void *));
      f->eax = sys_filesize (esp[ONE_ARG]);
      break;
    case SYS_READ:
      validate_address (esp, (THREE_ARG + 1) * sizeof (void *));
      f->eax = sys_read (esp[ONE_ARG], ((void **)esp)[TWO_ARG], 
                         esp[THREE_ARG]);
      break;
    case SYS_WRITE:
      validate_address (esp, (THREE_ARG + 1) * sizeof (void *));
      f->eax = sys_write (esp[ONE_ARG], ((void **)esp)[TWO_ARG], 
                          esp[THREE_ARG]);
      break;
    case SYS_SEEK:
      validate_address (esp, (TWO_ARG + 1) * sizeof (void *));
      sys_seek (esp[ONE_ARG], esp[TWO_ARG]);
      break;
    case SYS_TELL:
      validate_address (esp, (ONE_ARG + 1) * sizeof (void *));
      f->eax = sys_tell (esp[ONE_ARG]);
      break;
    case SYS_CLOSE:
      validate_address (esp, (ONE_ARG + 1) * sizeof (void *));
      sys_close (esp[ONE_ARG]);
      break;
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