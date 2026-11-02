#ifndef _UNISTD_H
#define _UNISTD_H 1

#include <stddef.h>
#include <stdint.h>

typedef int32_t ssize_t;
typedef int32_t pid_t;

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#ifdef __cplusplus
extern "C" {
#endif

ssize_t read(int file_descriptor, void* buffer, size_t count);
ssize_t write(int file_descriptor, const void* buffer, size_t count);
int close(int file_descriptor);
int32_t lseek(int file_descriptor, int32_t offset, int whence);
pid_t spawn(const char* path, char* const arguments[]);
pid_t waitpid(pid_t process_id, int* status, int options);
pid_t getpid(void);
void* sbrk(intptr_t increment);
__attribute__((__noreturn__)) void _exit(int status);

#ifdef __cplusplus
}
#endif

#endif
