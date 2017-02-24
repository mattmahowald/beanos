#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H


#define MAP_FAILED ((mapid_t) -1)

/* Map region identifier. */
typedef int mapid_t;

struct fd_to_file 
  {
    int fd;
    struct file *f;
    struct list_elem elem;
  };

struct mmapped_file
	{
		struct list_elem elem;
		mapid_t id;
		void *start_vaddr;
		void *end_vaddr;
		struct file *file;
	};

void syscall_init (void);
void sys_exit (int status);
void syscall_acquire_filesys_lock (void);
void syscall_release_filesys_lock (void);

#endif /* userprog/syscall.h */
