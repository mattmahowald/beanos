#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

struct fd_to_file 
  {
    int fd;
    struct file *f;
    struct list_elem elem;
  };

void syscall_init (void);
void sys_exit (int status);
void acquire_filesys_lock (void);
void release_filesys_lock (void);

#endif /* userprog/syscall.h */
