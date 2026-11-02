#include <stddef.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#define USER_SYSCALL_WRITE 1
#define USER_SYSCALL_EXIT 2
#define USER_SYSCALL_READ 3
#define USER_SYSCALL_OPEN 4
#define USER_SYSCALL_CLOSE 5
#define USER_SYSCALL_BRK 6
#define USER_SYSCALL_LSEEK 7
#define USER_SYSCALL_SPAWN 8
#define USER_SYSCALL_WAITPID 9
#define USER_SYSCALL_GETPID 10
#define USER_SYSCALL_STDOUT 1
#define USER_SYSCALL_MAX_IO 256
#define USER_SYSCALL_MAX_ARGS 16

static int invoke_syscall(uint32_t number, uint32_t first,
                          uint32_t second, uint32_t third) {
    int result;
    asm volatile("int $0x80"
                 : "=a"(result)
                 : "0"(number), "b"(first), "c"(second), "d"(third)
                 : "memory", "cc");
    return result;
}

ssize_t read(int file_descriptor, void* buffer, size_t count) {
    if (!count) {
        return 0;
    }
    if (!buffer) {
        return -1;
    }

    unsigned char* bytes = (unsigned char*)buffer;
    size_t written = 0;
    while (written < count) {
        size_t chunk = count - written;
        if (chunk > USER_SYSCALL_MAX_IO) {
            chunk = USER_SYSCALL_MAX_IO;
        }
        int result = invoke_syscall(USER_SYSCALL_READ,
                                    (uint32_t)file_descriptor,
                                    (uint32_t)(bytes + written),
                                    (uint32_t)chunk);
        if (result <= 0) {
            return written ? (ssize_t)written : result;
        }
        written += (size_t)result;
        if ((size_t)result < chunk) {
            break;
        }
    }
    return (ssize_t)written;
}

ssize_t write(int file_descriptor, const void* buffer, size_t count) {
    if (!count) {
        return 0;
    }
    if (!buffer) {
        return -1;
    }

    const unsigned char* bytes = (const unsigned char*)buffer;
    size_t written = 0;
    while (written < count) {
        size_t chunk = count - written;
        if (chunk > USER_SYSCALL_MAX_IO) {
            chunk = USER_SYSCALL_MAX_IO;
        }
        int result = invoke_syscall(USER_SYSCALL_WRITE,
                                   (uint32_t)file_descriptor,
                                   (uint32_t)(bytes + written),
                                   (uint32_t)chunk);
        if (result <= 0) {
            return written ? (ssize_t)written : result;
        }
        written += (size_t)result;
        if ((size_t)result < chunk) {
            break;
        }
    }
    return (ssize_t)written;
}

int open(const char* path, int flags, ...) {
    if (!path) {
        return -1;
    }
    return invoke_syscall(USER_SYSCALL_OPEN, (uint32_t)path,
                          (uint32_t)flags, 0);
}

int close(int file_descriptor) {
    return invoke_syscall(USER_SYSCALL_CLOSE,
                          (uint32_t)file_descriptor, 0, 0);
}

int32_t lseek(int file_descriptor, int32_t offset, int whence) {
    return invoke_syscall(USER_SYSCALL_LSEEK,
                          (uint32_t)file_descriptor,
                          (uint32_t)offset, (uint32_t)whence);
}

pid_t spawn(const char* path, char* const arguments[]) {
    if (!path || !arguments) {
        return -1;
    }
    uint32_t count = 0;
    while (count <= USER_SYSCALL_MAX_ARGS && arguments[count]) {
        count++;
    }
    if (!count || count > USER_SYSCALL_MAX_ARGS) {
        return -1;
    }
    return invoke_syscall(USER_SYSCALL_SPAWN, (uint32_t)path,
                          (uint32_t)arguments, count);
}

pid_t waitpid(pid_t process_id, int* status, int options) {
    return invoke_syscall(USER_SYSCALL_WAITPID, (uint32_t)process_id,
                          (uint32_t)status, (uint32_t)options);
}

pid_t getpid(void) {
    return invoke_syscall(USER_SYSCALL_GETPID, 0, 0, 0);
}

void* sbrk(intptr_t increment) {
    int current = invoke_syscall(USER_SYSCALL_BRK, 0, 0, 0);
    if (current < 0) {
        return (void*)-1;
    }
    if (!increment) {
        return (void*)(uintptr_t)current;
    }

    uintptr_t next = (uintptr_t)current + (uintptr_t)increment;
    if ((increment > 0 && next < (uintptr_t)current) ||
        (increment < 0 && next > (uintptr_t)current)) {
        return (void*)-1;
    }
    int result = invoke_syscall(USER_SYSCALL_BRK, (uint32_t)next, 0, 0);
    return result < 0 ? (void*)-1 : (void*)(uintptr_t)current;
}

__attribute__((__noreturn__))
void _exit(int status) {
    (void)invoke_syscall(USER_SYSCALL_EXIT, (uint32_t)status, 0, 0);
    for (;;) {
        asm volatile("hlt");
    }
}

__attribute__((__noreturn__))
void exit(int status) {
    extern void __libc_fini_array(void);
    extern void __cxa_finalize(void*);
    __libc_fini_array();
    __cxa_finalize(0);
    _exit(status);
}
