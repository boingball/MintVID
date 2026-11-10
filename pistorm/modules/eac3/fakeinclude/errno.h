/* Minimal errno.h for the freestanding BerryCore E-AC3 build (Linux values). */
#ifndef BERRY_ERRNO_H
#define BERRY_ERRNO_H
#define EPERM 1
#define ENOENT 2
#define EINTR 4
#define EIO 5
#define ENXIO 6
#define E2BIG 7
#define EBADF 9
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENFILE 23
#define EMFILE 24
#define ENOSPC 28
#define ESPIPE 29
#define EPIPE 32
#define EDOM 33
#define ERANGE 34
#define ENAMETOOLONG 36
#define ENOSYS 38
#define EILSEQ 84
#define ENOTSUP 95
#define ETIMEDOUT 110
extern int errno;
#endif
