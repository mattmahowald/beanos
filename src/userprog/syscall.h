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

#endif /* userprog/syscall.h */
