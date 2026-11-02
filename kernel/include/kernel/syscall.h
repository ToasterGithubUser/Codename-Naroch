#ifndef _KERNEL_SYSCALL_H
#define _KERNEL_SYSCALL_H

enum {
    SYSCALL_WRITE = 1,
    SYSCALL_EXIT = 2,
    SYSCALL_READ = 3,
    SYSCALL_OPEN = 4,
    SYSCALL_CLOSE = 5,
    SYSCALL_BRK = 6,
    SYSCALL_LSEEK = 7,
    SYSCALL_SPAWN = 8,
    SYSCALL_WAITPID = 9,
    SYSCALL_GETPID = 10,
    SYSCALL_STDOUT = 1,
    SYSCALL_STDERR = 2,
    SYSCALL_MAX_IO = 256
};

enum {
    SYSCALL_O_RDONLY = 0,
    SYSCALL_O_WRONLY = 1,
    SYSCALL_O_RDWR = 2,
    SYSCALL_O_ACCMODE = 3,
    SYSCALL_O_CREAT = 0x40,
    SYSCALL_O_TRUNC = 0x200,
    SYSCALL_O_APPEND = 0x400
};

enum {
    SYSCALL_SEEK_SET = 0,
    SYSCALL_SEEK_CUR = 1,
    SYSCALL_SEEK_END = 2,
    SYSCALL_SPAWN_MAX_ARGS = 16,
    SYSCALL_SPAWN_MAX_ARG_LENGTH = 256
};

int syscall_console_input(char character);
void syscall_set_foreground_pid(int pid);

#endif
