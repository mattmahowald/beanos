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


static void syscall_handler (struct intr_frame *);
static void validate_address (void * address);
static void sys_halt (void);
static tid_t sys_exec (const char *cmd_line);
static bool sys_create (const char *file, uint32_t initial_size);
static bool sys_remove (const char *file);
static int sys_open (const char *file);
static int sys_filesize (int fd);
static int sys_read (int fd, void *buffer, uint32_t size);
static int sys_write (int fd, void *buffer, uint32_t size);
static void sys_seek (int fd, uint32_t position);
static uint32_t sys_tell (int fd);
static void sys_close (int fd);
static tid_t sys_wait (tid_t tid);
static struct fd_to_file *get_file_struct_from_fd (int fd);


static struct lock filesys_lock; 

static int next_fd = STDOUT_FILENO + 1; 

void syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&filesys_lock);
}

// TODO validate strings just like buffers

/* Helper function that validates the passed pointer as a legal, user
   virtual space address. */
static void
validate_address (void * address)
{
  if (!is_user_vaddr(address) || !address)
    sys_exit (-1);
  uint32_t *page_dir = thread_current ()-> pagedir;
  if (pagedir_get_page(page_dir, address) == NULL)
    sys_exit (-1);
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
  if (cur->parent != NULL)
    {
      struct child_thread *self = cur->self;
      self->exit_status = status;
      self->running = true;
      sema_up (&self->done);
    }
  
  printf("%s: exit(%d)\n", cur->name, status);
  
  thread_exit ();
}

/* System call exec(cmd_line) executes the command line passed in by
   first validating the passed string then calling process execute
   and returning the return value. */
static int
sys_exec (const char *cmd_line)
{
  validate_address((void *)cmd_line);
 
  tid_t tid = process_execute (cmd_line);

  return tid == TID_ERROR ? -1 : tid;
}

/* System call wait(tid) relies on process_wait to reap child process
   identified by tid. */
static tid_t
sys_wait (tid_t tid)
{
  return process_wait(tid);
}

/* System call create(file, initial_size) opens a file with the name and
   size passed in by first validating the address of the filename string 
   and then creating a file through the filesys_create function. */
static bool
sys_create (const char *file, uint32_t initial_size)
{
  validate_address((void *)file);

  // TODO I don't feel great about this filesys lock acquiring stuff
  // Specifically I feel like we should either be calling the helper or not
  lock_acquire (&filesys_lock);
  bool success =  filesys_create (file, initial_size);
  lock_release (&filesys_lock);

  return success;
}

/* System call remove(file) removes the file with the name and passed in 
   by first validating the address of the filename string and then removing 
   the file through filesys_remove function. */
static bool
sys_remove (const char *file)
{
  validate_address((void *)file);

  lock_acquire (&filesys_lock);
  bool success = filesys_remove (file);
  lock_release (&filesys_lock);

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
  validate_address((void *)file);

  lock_acquire (&filesys_lock);
  struct file *f = filesys_open (file);
  lock_release (&filesys_lock);
  if (!f)
    return -1; 

  /* Define a fd_to_file struct for the process to lookup by fd. */
  struct fd_to_file *user_file = malloc (sizeof (struct fd_to_file));
  if (!user_file)
    return -1;

  user_file->f = f;
  user_file->fd = next_fd++;

  list_push_back (&thread_current ()->files, &user_file->elem);

  return user_file->fd;
}

/* System call filesize(fd) gets the filesize of the file corresponding
   to the passed in fd. */
static int
sys_filesize (int fd)
{
  struct file *f = get_file_struct_from_fd (fd)->f;
  if (f == NULL)
    return -1;

  return file_length (f);
}

/* System call read(fd, buffer, size) reads from the file corresponding
   to the passed in fd by first validating the buffer, then determining 
   if the fd is STDIN, in which case it reads from the keyboard, or STDOUT,
   in which case it exits with status -1. Finally, the function finds the 
   file and reads into the passed in buffer, returning the number of bytes 
   read. */
static int
sys_read (int fd, void *buffer, uint32_t size)
{ 
  validate_address (buffer);
  // TODO check buffer multiple pages 
  validate_address ((char *)buffer + size);

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
    return -1;

  /* Otherwise, find the file and read from it. */
  struct fd_to_file *fd_ = get_file_struct_from_fd (fd);
  if (!fd_)
    sys_exit (-1);  
  struct file *f = fd_->f;
  
  // TODO handle this whole unsigned / int32 conundrum
  lock_acquire (&filesys_lock);
  read = file_read (f, buffer, size);
  lock_release (&filesys_lock); 

  return read;
}

/* System call write(fd, buffer, size) writes to the file corresponding
   to the passed in fd by first validating the buffer, then determining 
   if the fd is STDOUT, in which case it writes to the console, or STDIN,
   in which case it exits with status -1. Finally, the function finds 
   the file and writes to it from the passed in buffer, returning the 
   number of bytes written. */
static int
sys_write (int fd, void *buffer, uint32_t size)
{
  validate_address (buffer);
  validate_address ((char *)buffer + size);

  int written = 0;
  
  if (fd == STDOUT_FILENO) 
    {
      // TODO definitely segment this into sizes of sev hundred
      putbuf (buffer, size);
      return size;
    }
    
  if (fd == STDIN_FILENO)
    sys_exit (-1);

  struct fd_to_file *fd_ = get_file_struct_from_fd (fd);
  if (!fd_)
    sys_exit (-1);
  struct file *f = fd_->f;

  // TODO handle this whole unsigned / int32 conundrum
  lock_acquire (&filesys_lock);
  written = file_write (f, buffer, size);
  lock_release (&filesys_lock);
    
  return written;
}

/* System call seek(fd, position) seeks to the passed position in
   the file corresponding to the passed in fd. */
static void
sys_seek (int fd, uint32_t position)
{
  struct file *f = get_file_struct_from_fd (fd)->f;
  if (f == NULL)
    return;
  
  lock_acquire (&filesys_lock);
  file_seek (f, position);
  lock_release (&filesys_lock);
}

static uint32_t
sys_tell (int fd)
{
  struct file *f = get_file_struct_from_fd (fd)->f;
  if (f == NULL)
    return 0;
  
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
    return;
  
  lock_acquire (&filesys_lock);
  file_close (f->f);
  lock_release (&filesys_lock);
  
  list_remove (&f->elem);
}

/* Handles the syscall interrupt, identifying the system call to be
   made then validating the stack and calling the correct system call. */
static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  int *esp = f->esp;
  validate_address(esp);

  switch (*esp)
    {
    case SYS_HALT:
      sys_halt ();
      break;
    case SYS_EXIT:
      validate_address (esp + 1);
      sys_exit (esp[1]);
      break;
    case SYS_EXEC:
      validate_address (esp + 1);
      f->eax = sys_exec (((char **)esp)[1]);
      break;
    case SYS_WAIT:
      validate_address (esp + 1);
      f->eax = sys_wait (((tid_t *)esp)[1]);
      break;
    case SYS_CREATE:
      validate_address (esp + 2);
      f->eax = sys_create (((char **)esp)[1], esp[2]);
      break;
    case SYS_REMOVE:
      validate_address (esp + 1);
      f->eax = sys_remove (((char **)esp)[1]);
      break;
    case SYS_OPEN:
      validate_address (esp + 1);
      f->eax = sys_open (((char **)esp)[1]);
      break;
    case SYS_FILESIZE:
      validate_address (esp + 1);
      f->eax = sys_filesize (esp[1]);
      break;
    case SYS_READ:
      validate_address (esp + 3);
      f->eax = sys_read (esp[1], ((void **)esp)[2], esp[3]);
      break;
    case SYS_WRITE:
      validate_address (esp + 3);
      f->eax = sys_write (esp[1], ((void **)esp)[2], esp[3]);
      break;
    case SYS_SEEK:
      validate_address (esp + 2);
      sys_seek (esp[1], esp[2]);
      break;
    case SYS_TELL:
      validate_address (esp + 1);
      f->eax = sys_tell (esp[1]);
      break;
    case SYS_CLOSE:
      validate_address (esp + 1);
      sys_close (esp[1]);
      break;
    default:
      sys_exit (-1);
    }
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

void 
acquire_filesys_lock ()
{
  lock_acquire (&filesys_lock);
}

void 
release_filesys_lock ()
{
  lock_release (&filesys_lock);
}

