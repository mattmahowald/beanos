#include "userprog/pagedir.h"
#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"


static void syscall_handler (struct intr_frame *);
static bool validate_address (void * address);
static void sys_halt (void);
static void sys_exit (void);
static void sys_exec (void /* const char *cmd_line */);
static void sys_create (void /* const *file, uint32_t initial_size */);
static void sys_remove (void /* const char *file */);
static void sys_open (void /* const char *file */);
static void sys_filesize (void /* int fd */);
static void sys_read (void /* int fd, void *buffer */);
static void sys_write (void *esp /* int fd, const void *buffer, uint32_t size */);
static void sys_seek (void /* int fd, uint32_t position */);
static void sys_tell (void /* int fd */);
static void sys_close (void /* int fd */);
static void sys_wait (void);

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
sys_exit (/* int status */)
{
	printf ("EXIT\n");
	thread_exit ();
}

static void
sys_exec (/* const char *cmd_line */)
{
	printf ("EXEC\n");
}

static void
sys_wait ()
{
	printf("WAIT\n");
}

// TODO what type should this be
static void
sys_create (/* const *file, uint32_t initial_size */)
{
	printf ("CREATE\n");
}

static void
sys_remove (/* const char *file */)
{
	printf ("REMOVE\n");
}

static void
sys_open (/* const char *file */)
{
	printf ("OPEN\n");;
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

static void
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
			printf("about to pit buf of size %d\n", (int)size);
			printf("%s\n", (char *)buffer);
			putbuf (*(char **)buffer, size);
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
  		sys_exit ();
  		break;
  	case SYS_EXEC:
  		sys_exec ();
  		break;
  	case SYS_WAIT:
  		sys_wait ();
  		break;
  	case SYS_CREATE:
  		sys_create ();
  		break;
  	case SYS_REMOVE:
  		sys_remove ();
  		break;
  	case SYS_OPEN:
  		sys_open ();
  		break;
  	case SYS_FILESIZE:
  		sys_filesize ();
  		break;
  	case SYS_READ:
  		sys_read ();
  		break;
  	case SYS_WRITE:
  		sys_write (esp);
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
  thread_exit ();
}
