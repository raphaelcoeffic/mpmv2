#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

__attribute__((weak)) int _close(int fildes)
{
    errno = ENOSYS;
    return -1;
}
__attribute__((weak)) int _fstat(int fildes, struct stat *st)
{
    errno = ENOSYS;
    return -1;
}
__attribute__((weak)) int _getpid(void)
{
    errno = ENOSYS;
    return -1;
}
__attribute__((weak)) int _isatty(int file)
{
    errno = ENOSYS;
    return 0;
}
__attribute__((weak)) int _kill(int pid, int sig)
{
    errno = ENOSYS;
    return -1;
}
__attribute__((weak)) int _lseek(int file, int ptr, int dir)
{
    errno = ENOSYS;
    return -1;
}
__attribute__((weak)) int _read(int file, char *ptr, int len)
{
    errno = ENOSYS;
    return -1;
}
__attribute__((weak)) int _write(int file, char *ptr, int len)
{
    errno = ENOSYS;
    return -1;
}
__attribute__((weak)) void _exit(int rc)
{
    /* Convince GCC that this function never returns.  */
    while (1) {}
}

extern uint32_t _heap_start;
extern uint32_t _heap_end;

unsigned char * heap = (unsigned char *)&_heap_start;

extern caddr_t _sbrk(uint32_t nbytes)
{
  if (heap + nbytes < (unsigned char *)&_heap_end) {
    unsigned char *prev_heap = heap;
    heap += nbytes;
    return (caddr_t)prev_heap;
  } else {
    errno = ENOMEM;
    return ((void *)-1);
  }
}
