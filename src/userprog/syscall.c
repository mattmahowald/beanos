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
static int next_fd = 2;

struct fd_to_file 
  {
    int fd;
    struct file *f;
    struct list_elem elem;
  };

void syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&filesys_lock);
}

// TODO double check this function at OH

static void
validate_address (void * address)
{
	if (!is_user_vaddr(address))
		sys_exit (-1);
	uint32_t *page_dir = thread_current ()-> pagedir;
	if (pagedir_get_page(page_dir, address) == NULL)
		sys_exit (-1);
}

static void
sys_halt (void)
{
	// printf ("HALT\n");
  shutdown_power_off ();
}

void
sys_exit (int status)
{
  // TODO 
  //    close all files, release all locks
  //    is kernel doing this?
	// printf ("EXIT\n");
  struct list_elem *child_e;
  struct list *children = &thread_current ()->children;
  // printf("%s\n", thread_current ()->name);
  // printf("list size is %d\n", list_size(children));
  for (child_e = list_begin (children); child_e != list_end (children);
       child_e = list_next (child_e))
    {
      struct thread *t = list_entry (child_e, struct thread, child_elem);
      // printf("Sema-ing up on thread %s\n", t->name);
      sema_up (&t->safe_to_die);
    }
  thread_current ()->ret_status = status;
  sema_up (&thread_current ()->done);
  char *name = thread_current ()->name;
  printf("%s: exit(%d)\n", name, status);
  thread_exit ();
  NOT_REACHED ();
}

static int
sys_exec (const char *cmd_line)
{
  // printf ("EXEC\n");
  // load cmdline executable
  // return the new process's pid
  // must return -1 if cannot load
  // aka process_execute (cmd_line);
  // establish parent-child relationship in process.c
  validate_address((void *)cmd_line);
  tid_t tid = process_execute (cmd_line);
  if (tid == TID_ERROR) 
    return -1;
	return tid;
}

static tid_t
sys_wait (tid_t tid)
{
  return process_wait(tid);
	// printf("WAIT\n");
}

// TODO what type should this be
static bool
sys_create (const char *file, uint32_t initial_size)
{
	// printf ("CREATE\n");
  validate_address((void *)file);
  lock_acquire (&filesys_lock);
  bool success =  filesys_create (file, initial_size);
  lock_release (&filesys_lock);
  return success;
}

static bool
sys_remove (const char *file)
{
	// printf ("REMOVE\n");
  validate_address((void *)file);
  lock_acquire (&filesys_lock);
  bool success = filesys_remove (file);
  lock_release (&filesys_lock);
  return success;
}

static int
sys_open (const char *file)
{
  validate_address((void *)file);
  // add lock for file ops
  struct file *f = filesys_open (file);
  if (f == NULL)
    return -1; 

  // TODO check malloc return val
  struct fd_to_file *user_file = malloc (sizeof (struct fd_to_file));
  user_file->f = f;
  user_file->fd = next_fd++;

  // TODO Potentially, adjust inode open_cnt

  list_push_back (&thread_current ()->files, &user_file->elem);
  // printf("Setting to fd %d\n", user_file->fd);
  return user_file->fd;
}

static int
sys_filesize (int fd)
{
	// printf ("FILESIZE\n");
  struct file *f = get_file_struct_from_fd (fd)->f;
  if (f == NULL)
    return -1;
  return file_length (f);
}

static int
sys_read (int fd, void *buffer, uint32_t size)
{ 
	// printf ("READ\n");
  int read = -1;
  validate_address (buffer);
  validate_address ((char *)buffer + size);
  if (fd == STDIN_FILENO)
    {
      size_t i;
      for (i = 0; i < size; i++)  
        *((char *)buffer + i) = input_getc ();
      read = size;
    }
  else 
    {
      if (fd == STDOUT_FILENO)
        sys_exit (-1);
      // TODO again seperate into two steps to check for null return value
      struct fd_to_file *fd_ = get_file_struct_from_fd (fd);
      if (!fd_)
        sys_exit (-1);
      struct file *f = fd_->f;
      lock_acquire (&filesys_lock);
      read = file_read (f, buffer, size);
      lock_release (&filesys_lock); 
    }
  return read;
}

static int
sys_write (int fd, void *buffer, uint32_t size)
{
	// printf ("WRITE\n");
	int written = -1;
  validate_address (buffer);
  validate_address ((char *)buffer + size);
	if (fd == STDOUT_FILENO) 
		{
      // TODO maybe segment this into sizes of sev hundred
		  putbuf (buffer, size);
      written = size;
		}
	else
    {
      // TODO potenitally make sure not writing ot STDIN
      if (fd == STDIN_FILENO)
        sys_exit (-1);
      // ALSO TODO, maybe seperate this line into multiple to check for void return val
      // (cant do at present as potentially dereferencing null pointer)
      struct fd_to_file *fd_ = get_file_struct_from_fd (fd);
      if (!fd_)
        sys_exit (-1);
      struct file *f = fd_->f;
      lock_acquire (&filesys_lock);
      written = file_write (f, buffer, size);
      lock_release (&filesys_lock);
    }
    return written;
}

static void
sys_seek (int fd, uint32_t position)
{
	// printf ("SEEK\n");
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
	// printf ("TELL\n");
  struct file *f = get_file_struct_from_fd (fd)->f;
  if (f == NULL)
    return 0;
  return file_tell (f);
}  

static void
sys_close (int fd)
{
	// printf ("CLOSE\n");
  struct fd_to_file *f = get_file_struct_from_fd (fd);
  if (f == NULL)
    return;
  lock_acquire (&filesys_lock);
  file_close (f->f);
  lock_release (&filesys_lock);
  list_remove (&f->elem);
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  int *esp = (int *)(f->esp);
  validate_address(esp);


  int call_number = *(int *)esp;
  switch (call_number)
  	{
  	case SYS_HALT:
  		sys_halt ();
  		break;
  	case SYS_EXIT:
      validate_address (esp + 1);
  		sys_exit (((int *)esp)[1]);
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
  		f->eax = sys_create (((char **)esp)[1], ((int *)esp)[2]);
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
  		f->eax = sys_filesize (((int *)esp)[1]);
  		break;
  	case SYS_READ:
      validate_address (esp + 3);
  		f->eax = sys_read (((int *)esp)[1], ((void **)esp)[2], ((int *)esp)[3]);
  		break;
  	case SYS_WRITE:
      validate_address (esp + 3);
  		f->eax = sys_write (((int *)esp)[1], ((void **)esp)[2], ((int *)esp)[3]);
  		break;
  	case SYS_SEEK:
      validate_address (esp + 2);
  		sys_seek (((int *)esp)[1], ((int *)esp)[2]);
  		break;
  	case SYS_TELL:
      validate_address (esp + 1);
  		f->eax = sys_tell (((int *)esp)[1]);
  		break;
  	case SYS_CLOSE:
      validate_address (esp + 1);
  		sys_close (((int *)esp)[1]);
  		break;
    default:
      sys_exit (-1);
  	}
}

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
