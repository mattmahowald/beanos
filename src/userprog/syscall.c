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
static bool validate_address (void * address);
static void sys_halt (void);
static void sys_exit (int status);
static tid_t sys_exec (const char *cmd_line);
static bool sys_create (const char *file, uint32_t initial_size);
static void sys_remove (void /* const char *file */);
static int sys_open (const char *file);
static void sys_filesize (void /* int fd */);
static void sys_read (void /* int fd, void *buffer */);
static size_t sys_write (void *esp /* int fd, const void *buffer, uint32_t size */);
static void sys_seek (void /* int fd, uint32_t position */);
static void sys_tell (void /* int fd */);
static void sys_close (void /* int fd */);
static tid_t sys_wait (tid_t tid);

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
}

// TODO double check this function at OH

static bool
validate_address (void * address)
{
	if (!is_user_vaddr(address))
		return false;
	uint32_t *page_dir = thread_current ()-> pagedir;
	if (pagedir_get_page(page_dir, address) == NULL)
		return false;
	return true;
}

static void
sys_halt (void)
{
	printf ("HALT\n");
}

static void
sys_exit (int status)
{
  // TODO 
  //    close all files
  //    is kernel doing this?
	printf ("EXIT\n");
  thread_current ()->ret_status = status;
  sema_up (thread_current ()->done);
  NOT_REACHED ();
}

static int
sys_exec (const char *cmd_line)
{
  printf ("EXEC\n");
  // load cmdline executable
  // return the new process's pid
  // must return -1 if cannot load
  // aka process_execute (cmd_line);
  // establish parent-child relationship in process.c
  if (!validate_address ((void *) cmd_line))
    return -1;
  tid_t tid = process_execute (cmd_line);
  if (tid == TID_ERROR) 
    return -1;
	return tid;
}

static tid_t
sys_wait (tid_t tid)
{
  return process_wait(tid);
	printf("WAIT\n");
}

// TODO what type should this be
static bool
sys_create (const char *file, uint32_t initial_size)
{
	printf ("CREATE\n");
  if (!validate_address ((void *)file))
    thread_exit (); // change to free resources and exit (decomposed)
  // add filesys lock
  return filesys_create (file, initial_size);
}

static void
sys_remove (/* const char *file */)
{
	printf ("REMOVE\n");
}

static int
sys_open (const char *file)
{
  printf ("OPEN\n");;
  if (!validate_address ((void *)file))
    thread_exit (); // change to free resources and exit (decomposed)
  printf("Opening file %s\n", file);
  // add lock for file ops
  struct file *f = filesys_open (file);
  if (f == NULL)
    return -1; 

  struct fd_to_file *user_file = malloc (sizeof (struct fd_to_file));
  user_file->f = f;
  user_file->fd = next_fd++;

  // TODO Potentially, adjust inode open_cnt

  list_push_back (&thread_current ()->files, &user_file->elem);
  printf("Setting to fd %d\n", user_file->fd);
  return user_file->fd;
}

static void
sys_filesize (/* int fd */)
{
	printf ("FILESIZE\n");
}

static void
sys_read (/* int fd, void *buffer */)
{
	printf ("READ\n");
}

static size_t
sys_write (void *esp/* int fd, const void *buffer, uint32_t size */)
{
	printf ("WRITE\n");
	int fd = ((int *)esp)[1];
	char *buffer = ((char **)esp)[2]; //((void **)esp)[2];
	size_t size = ((size_t *)esp)[3];
	if (!validate_address ((void *)buffer))
		thread_exit (); // change to free resources and exit (decomposed)
	if (fd == STDOUT_FILENO) 
		{
			printf("about to print buf of size %d\n", (int)size);
		  putbuf (buffer, size);
      return size;
		}
	else
		thread_exit ();	
}

static void
sys_seek (/* int fd, uint32_t position */)
{
	printf ("SEEK\n");
}

static void
sys_tell (/* int fd */)
{
	printf ("TELL\n");
}

static void
sys_close (/* int fd */)
{
	printf ("CLOSE\n");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  void *esp = f->esp;
  int call_number = *(int *)esp;
  switch (call_number)
  	{

  	case SYS_HALT:
  		sys_halt ();
  		break;
  	case SYS_EXIT:
  		sys_exit (((int *)esp)[1]);
  		break;
  	case SYS_EXEC:
  		f->eax = sys_exec (((char **)esp)[1]);
  		break;
  	case SYS_WAIT:
  		f->eax = sys_wait (((tid_t *)esp)[1]);
  		break;
  	case SYS_CREATE:
  		f->eax = sys_create (((char **)esp)[1], ((int *)esp)[2]);
  		break;
  	case SYS_REMOVE:
  		sys_remove ();
  		break;
  	case SYS_OPEN:
  		f->eax = sys_open (((char **)esp)[1]);
  		break;
  	case SYS_FILESIZE:
  		sys_filesize ();
  		break;
  	case SYS_READ:
  		sys_read ();
  		break;
  	case SYS_WRITE:
  		f->eax = sys_write (esp);
      printf("eax: %d\n", (int) f->eax);
  		break;
  	case SYS_SEEK:
  		sys_seek ();
  		break;
  	case SYS_TELL:
  		sys_tell ();
  		break;
  	case SYS_CLOSE:
  		sys_close ();
  		break;
  	}
  printf("Exiting thread\n");
  // thread_exit ();
}
