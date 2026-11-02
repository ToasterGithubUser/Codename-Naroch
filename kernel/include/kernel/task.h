#ifndef _KERNEL_TASK_H
#define _KERNEL_TASK_H

#include <stdint.h>
#include <stddef.h>
#include <kernel/ring3.h>

typedef enum {
    TASK_RUNNING,
    TASK_READY,
    TASK_BLOCKED,
    TASK_ZOMBIE
} task_state_t;

typedef enum {
    TASK_WAIT_NONE,
    TASK_WAIT_PROCESS,
    TASK_WAIT_STDIN
} task_wait_kind_t;

typedef struct task {
    int pid;
    uint32_t esp;
    int parent_pid;
    int exit_status;
    int wait_pid;
    uint32_t wait_status_address;
    uint32_t wait_buffer;
    uint32_t wait_length;
    char stdin_buffer[128];
    uint16_t stdin_head;
    uint16_t stdin_count;
    uint16_t stdin_ready;
    task_wait_kind_t wait_kind;
    uint32_t kernel_stack_top;
    uint32_t* page_directory;
    uint32_t heap_start;
    uint32_t heap_break;
    volatile ring3_test_state_t* user_test_state;
    task_state_t state;
    struct task* next;
} task_t;

extern task_t* current_task;

void tasking_install(void);
task_t* task_create_kernel(void (*entry_point)(void));
task_t* task_create_user(const void* code, size_t code_size, uint32_t pid);
int task_create_elf(const void* image, size_t image_size,
                    int argc, const char* const argv[], int parent_pid,
                    int* process_id);
void task_process_exit(int status);
int task_wait_process(int pid, uint32_t status_address);
int task_process_status(int pid, int* status);
int task_read_stdin(uint32_t user_buffer, uint32_t length);
void task_stdin_feed(int pid, char character);
void task_switch(void);
void task_start(void);
void task_yield(void);

#endif
