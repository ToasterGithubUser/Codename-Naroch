#ifndef _FCNTL_H
#define _FCNTL_H 1

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_ACCMODE 3
#define O_CREAT 0x40
#define O_TRUNC 0x200
#define O_APPEND 0x400

#ifdef __cplusplus
extern "C" {
#endif

int open(const char* path, int flags, ...);

#ifdef __cplusplus
}
#endif

#endif
