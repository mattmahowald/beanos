#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

struct child_thread {
	struct thread *t;
	struct semaphore done;
	tid_t tid;
	int exit_status;
	bool running;
	struct list_elem elem;
};


tid_t process_execute (const char *cmdline);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

#endif /* userprog/process.h */
