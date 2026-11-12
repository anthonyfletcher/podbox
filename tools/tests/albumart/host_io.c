/* Host filesystem adapter for read-only decoder tests. */
#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
int sim_open(const char *p, int flags, ...)
{
    return open(p, flags);
}
int sim_close(int fd)
{
    return close(fd);
}
ssize_t sim_read(int fd, void *p, size_t n)
{
    return read(fd, p, n);
}
off_t sim_lseek(int fd, off_t p, int mode)
{
    return lseek(fd, p, mode);
}
off_t sim_ffilesize(int fd)
{
    struct stat s;
    return fstat(fd, &s) ? -1 : s.st_size;
}
