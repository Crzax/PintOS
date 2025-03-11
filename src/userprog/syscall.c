#include "devices/shutdown.h"
#include "devices/input.h"
#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "lib/kernel/list.h"

#ifdef DEBUG
#define _DEBUG_PRINTF(...) printf(__VA_ARGS__)
#else
#define _DEBUG_PRINTF(...) /* do nothing */
#endif

static void syscall_handler (struct intr_frame *);

/** Helper Functions.*/
static void check_user (const uint8_t *uaddr);
static int32_t get_user (const uint8_t *uaddr);
static bool put_user (uint8_t *udst, uint8_t byte);
static int memread_user (void *src, void *des, size_t bytes);
static struct file_desc* find_file_desc(struct thread *, int fd);

/** Syscall Functions definition. */
void sys_halt (void);
void sys_exit (int);
pid_t sys_exec (const char *cmdline);
int sys_wait (pid_t pid);
bool sys_create (const char* filename, unsigned initial_size);
bool sys_remove (const char* filename);
int sys_open (const char* file);
void sys_close (int fd);
int sys_filesize (int fd);
void sys_seek(int fd, unsigned position);
unsigned sys_tell(int fd);
void sys_close(int fd);
int sys_read(int fd, void *buffer, unsigned size);
int sys_write(int fd, const void *buffer, unsigned size);

struct lock filesys_lock;

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/** Invalid memory access, fail and exit. */ 
static int 
fail_invalid_access(void) {
  sys_exit (-1);
  NOT_REACHED();
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  int syscall_number;
  ASSERT( sizeof(syscall_number) == 4 ); // assuming x86

  // The system call number is in the 32-bit word at the caller's stack pointer.
  memread_user(f->esp, &syscall_number, sizeof(syscall_number));

  _DEBUG_PRINTF ("[DEBUG] system call, number = %d!\n", syscall_number);

  /* Dispatch w.r.t system call number
   SYS_*** constants are defined in syscall-nr.h */
  switch (syscall_number) {
    case SYS_HALT:
      {
        sys_halt();
        NOT_REACHED();
        break;
      }
  
    case SYS_EXIT:
      {
        int exitcode;
        memread_user(f->esp + 4, &exitcode, sizeof(exitcode));

        sys_exit(exitcode);
        NOT_REACHED();
        break;
      }
  
    case SYS_EXEC:
      {
        void* cmdline;
        memread_user(f->esp + 4, &cmdline, sizeof(cmdline));

        int return_code = sys_exec((const char*) cmdline);
        f->eax = (uint32_t) return_code;
        break;
      }
    case SYS_WAIT:
      {
        pid_t pid;
        memread_user(f->esp + 4, &pid, sizeof(pid_t));

        int ret = sys_wait(pid);
        f->eax = (uint32_t) ret;
        break;
      }
    case SYS_CREATE:
      {
        const char* filename;
        unsigned initial_size;
        bool return_code;
        memread_user(f->esp + 4, &filename, sizeof(filename));
        memread_user(f->esp + 8, &initial_size, sizeof(initial_size));

        return_code = sys_create(filename, initial_size);
        f->eax = return_code;
        break;
      }
    case SYS_REMOVE:
      {
        const char* filename;
        bool return_code;
        memread_user(f->esp + 4, &filename, sizeof(filename));

        return_code = sys_remove(filename);
        f->eax = return_code;
        break;
      }
    case SYS_OPEN:
      {
        const char* filename;
        int return_code;

        memread_user(f->esp + 4, &filename, sizeof(filename));

        return_code = sys_open(filename);
        f->eax = return_code;
        break;
      }
    case SYS_FILESIZE:
      {
        int fd, return_code;
        memread_user(f->esp + 4, &fd, sizeof(fd));

        return_code = sys_filesize(fd);
        f->eax = return_code;
        break;
      }
    case SYS_READ:
      {
        int fd, return_code;
        void *buffer;
        unsigned size;

        memread_user(f->esp + 4, &fd, sizeof(fd));
        memread_user(f->esp + 8, &buffer, sizeof(buffer));
        memread_user(f->esp + 12, &size, sizeof(size));

        return_code = sys_read(fd, buffer, size);
        f->eax = (uint32_t) return_code;
        break;
      }
    case SYS_WRITE:
    {
      int fd, return_code;
      const void *buffer;
      unsigned size;

      memread_user(f->esp + 4, &fd, sizeof(fd));
      memread_user(f->esp + 8, &buffer, sizeof(buffer));
      memread_user(f->esp + 12, &size, sizeof(size));

      return_code = sys_write(fd, buffer, size);
      f->eax = (uint32_t) return_code;
      break;
    }
    case SYS_SEEK:
      {
        int fd;
        unsigned position;

        memread_user(f->esp + 4, &fd, sizeof(fd));
        memread_user(f->esp + 8, &position, sizeof(position));

        sys_seek(fd, position);
        break;
      }
    case SYS_TELL:
      {
        int fd;
        unsigned return_code;

        memread_user(f->esp + 4, &fd, sizeof(fd));

        return_code = sys_tell(fd);
        f->eax = (uint32_t) return_code;
        break;
      }
    case SYS_CLOSE:
      {
        int fd;
        memread_user(f->esp + 4, &fd, sizeof(fd));

        sys_close(fd);
        break;
      }
    
    /* unhandled case */
    default:
      printf("[ERROR] system call %d is unimplemented!\n", syscall_number);
      sys_exit(-1);
      break;
    }
}

/****************** System Call Implementations ********************/
void sys_halt(void) {
  shutdown_power_off();
}

void sys_exit(int status UNUSED) {
  printf("%s: exit(%d)\n", thread_current()->name, status);

  // The process exits.
  // wake up the parent process (if it was sleeping) using semaphore,
  // and pass the return code.
  struct process_control_block *pcb = thread_current()->pcb;
  if(pcb != NULL) {
    pcb->exited = true;
    pcb->exitcode = status;
  }
  else {
    // pcb == NULL probably means that previously
    //
  }
  thread_exit();
}

pid_t sys_exec(const char *cmdline) {
  _DEBUG_PRINTF ("[DEBUG] Exec : %s\n", cmdline);

  /* cmdline is an address to the character buffer, on user memory
    so a validation check is required. */
  check_user((const uint8_t*) cmdline);

  pid_t pid = process_execute(cmdline);
  return pid;
}

int sys_wait(pid_t pid) 
{
  _DEBUG_PRINTF ("[DEBUG] Wait : %d\n", pid);
  return process_wait(pid);
}

bool sys_create(const char* filename, unsigned initial_size) 
{
  bool return_code;
  // memory validation
  check_user((const uint8_t*) filename);

  return_code = filesys_create(filename, initial_size);
  return return_code;
}

bool sys_remove(const char* filename) 
{
  bool return_code;
  // memory validation
  check_user((const uint8_t*) filename);

  return_code = filesys_remove(filename);
  return return_code;
}

int sys_open(const char* file) {
  // memory validation
  check_user((const uint8_t*) file);
  
  struct file* file_opened;
  struct file_desc* fd = palloc_get_page(0);

  if (!fd) {
    return -1;
  }

  file_opened = filesys_open(file);
  if (!file_opened) {
    palloc_free_page (fd);
    return -1;
  }

  fd->file = file_opened;

  struct list* fd_list = &thread_current ()->file_descriptors;
  if (list_empty(fd_list)) {
    // 0, 1, 2 are reserved for stdin, stdout, stderr
    fd->id = 3;
  }
  else {
    fd->id = (list_entry(list_back(fd_list), struct file_desc, elem)->id) + 1;
  }
  list_push_back(fd_list, &(fd->elem));

  return fd->id;
}

int sys_filesize(int fd) {
  struct file_desc* file_d;

  file_d = find_file_desc(thread_current(), fd);

  if(file_d == NULL) {
    return -1;
  }

  return file_length(file_d->file);
}

void sys_seek(int fd, unsigned position) {
  struct file_desc* file_d = find_file_desc(thread_current(), fd);

  if(file_d && file_d->file) {
    file_seek(file_d->file, position);
  }
  else
    return; // TODO need sys_exit?
}

unsigned sys_tell(int fd) {
  struct file_desc* file_d = find_file_desc(thread_current(), fd);

  if(file_d && file_d->file) {
    return file_tell(file_d->file);
  }
  else
    return -1; // TODO need sys_exit?
}

void sys_close(int fd) {
  struct file_desc* file_d = find_file_desc(thread_current(), fd);

  if(file_d && file_d->file) {
    file_close(file_d->file);
    list_remove(&(file_d->elem));
    palloc_free_page(file_d);
  }
}

int sys_read(int fd, void *buffer, unsigned size) {
   // memory validation : [buffer+0, buffer+size) should be all valid
   check_user((const uint8_t*) buffer);
   check_user((const uint8_t*) buffer + size - 1);

  if(fd == 0) { // stdin
    unsigned i;
    for(i = 0; i < size; ++i) {
      if(!put_user(buffer + i, input_getc()) )
        sys_exit(-1); // segfault
    }
    return size;
  }
  else {
    // read from file
    struct file_desc* file_d = find_file_desc(thread_current(), fd);

    if(file_d && file_d->file) {
      return file_read(file_d->file, buffer, size);
    }
    else // no such file or can't open
      return -1;
  }
}

int sys_write(int fd, const void *buffer, unsigned size) {
  // memory validation : [buffer+0, buffer+size) should be all valid
  check_user((const uint8_t*) buffer);
  check_user((const uint8_t*) buffer + size - 1);

  if(fd == 1) { // write to stdout
    putbuf(buffer, size);
    return size;
  }
  else {
    // write into file
    struct file_desc* file_d = find_file_desc(thread_current(), fd);

    if(file_d && file_d->file) {
      return file_write(file_d->file, buffer, size);
    }
    else // no such file or can't open
      return -1;
  }
}

/****************** Helper Functions on Memory Access ********************/
static void
check_user (const uint8_t *uaddr) {
  // check uaddr range or segfaults
  if(get_user (uaddr) == -1)
    fail_invalid_access();
}

/**
 * Reads a single 'byte' at user memory admemory at 'uaddr'.
 * 'uaddr' must be below PHYS_BASE.
 *
 * Returns the byte value if successful (extract the least significant byte),
 * or -1 in case of error (a segfault occurred or invalid uaddr)
 */
get_user (const uint8_t *uaddr) {
  /* check that a user pointer `uaddr` points below PHYS_BASE*/
  if (! ((void*)uaddr < PHYS_BASE))
    return -1; // invalid memory access

  /* as suggested in the reference manual, see (3.1.5) */
  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
      : "=&a" (result) : "m" (*uaddr));
  return result;
}


/* Writes a single byte (content is 'byte') to user address 'udst'.
 * 'udst' must be below PHYS_BASE.
 *
 * Returns true if successful, false if a segfault occurred.
 */
static bool
put_user (uint8_t *udst, uint8_t byte) {
  int error_code;

  // as suggested in the reference manual, see (3.1.5)
  asm ("movl $1f, %0; movb %b2, %1; 1:"
      : "=&a" (error_code), "=m" (*udst) : "q" (byte));
  return error_code != -1;
}

/** Reads a consecutive `bytes` bytes of user memory with the
  starting address `src` (uaddr), and writes to dst.
  Returns the number of bytes read, or -1 on page fault (invalid memory access)
 */
static int
memread_user (void *src, void *dst, size_t bytes)
{
  int32_t value;
  size_t i;
  for(i=0; i<bytes; i++) {
    value = get_user(src + i);
    if(value == -1) // segfault or invalid memory access
      fail_invalid_access();
    *(char*)(dst + i) = value & 0xff;
  }
  return (int)bytes;
}

static struct file_desc*
find_file_desc(struct thread *t, int fd)
{
  ASSERT (t != NULL);

  if (fd < 3) {
    return NULL;
  }

  struct list_elem *e;

  if (! list_empty(&t->file_descriptors)) {
    for(e = list_begin(&t->file_descriptors);
        e != list_end(&t->file_descriptors); e = list_next(e))
    {
      struct file_desc *desc = list_entry(e, struct file_desc, elem);
      if(desc->id == fd) {
        return desc;
      }
    }
  }

  return NULL; // not found
}