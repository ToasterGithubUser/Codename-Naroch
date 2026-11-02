#include <kernel/task.h>
#include <kernel/heap.h>
#include <kernel/pmm.h>
#include <kernel/paging.h>
#include <kernel/gdt.h>
#include <kernel/idt.h>
#include <string.h>

#define PAGE_SIZE 4096U
#define USER_CODE_ADDRESS 0x40000000U
#define USER_STACK_ADDRESS 0x40001000U
#define USER_STATE_ADDRESS 0x40002000U
#define USER_STACK_TOP 0x40002000U
#define ELF_USER_STACK_ADDRESS 0xBFFFE000U
#define ELF_USER_STACK_TOP 0xBFFFF000U
#define ELF_USER_LIMIT ELF_USER_STACK_ADDRESS
#define ELF_HEAP_LIMIT 0x70000000U
#define KERNEL_STACK_SIZE 8192U
#define IDENTITY_MAP_SIZE 0x02000000U
#define ELF_PT_LOAD 1U
#define ELF_PF_X 1U
#define ELF_PF_W 2U

typedef struct __attribute__((packed)) {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint32_t entry;
    uint32_t program_header_offset;
    uint32_t section_header_offset;
    uint32_t flags;
    uint16_t header_size;
    uint16_t program_header_size;
    uint16_t program_header_count;
    uint16_t section_header_size;
    uint16_t section_header_count;
    uint16_t section_name_index;
} elf32_header_t;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t offset;
    uint32_t virtual_address;
    uint32_t physical_address;
    uint32_t file_size;
    uint32_t memory_size;
    uint32_t flags;
    uint32_t alignment;
} elf32_program_header_t;

task_t* current_task = 0;
static task_t* ready_queue_head = 0;
static task_t* ready_queue_tail = 0;
static task_t* reap_pending = 0;
static int next_pid = 3;
#define TASK_EXIT_HISTORY_SIZE 32
typedef struct {
    int pid;
    int parent_pid;
    int status;
} task_exit_record_t;
static task_exit_record_t exit_history[TASK_EXIT_HISTORY_SIZE];
static uint32_t exit_history_next;

static void push_word(uint32_t** stack, uint32_t value) {
    *--(*stack) = value;
}

static void task_exit(void) {
    if (current_task) {
        current_task->state = TASK_ZOMBIE;
    }

    task_yield();
    for (;;) {
        asm volatile("cli; hlt");
    }
}

static task_t* task_allocate(void) {
    task_t* task = (task_t*)kmalloc(sizeof(task_t));
    if (!task) {
        return 0;
    }

    uint8_t* kernel_stack = (uint8_t*)kmalloc(KERNEL_STACK_SIZE);
    if (!kernel_stack) {
        kfree(task);
        return 0;
    }

    memset(task, 0, sizeof(*task));
    task->kernel_stack_top =
        (uint32_t)(kernel_stack + KERNEL_STACK_SIZE);
    task->state = TASK_READY;
    task->page_directory = paging_current_address_space();
    return task;
}

static void task_discard_unqueued(task_t* task, uint32_t code_page,
                                  uint32_t stack_page, uint32_t state_page,
                                  uint32_t* address_space) {
    if (address_space) {
        if (paging_user_page_physical_in(address_space, USER_CODE_ADDRESS) ==
            code_page) {
            code_page = 0;
        }
        if (paging_user_page_physical_in(address_space, USER_STACK_ADDRESS) ==
            stack_page) {
            stack_page = 0;
        }
        if (paging_user_page_physical_in(address_space, USER_STATE_ADDRESS) ==
            state_page) {
            state_page = 0;
        }
    }
    if (address_space) {
        paging_destroy_address_space(address_space);
    }
    if (code_page) {
        pmm_free_block((void*)code_page);
    }
    if (stack_page) {
        pmm_free_block((void*)stack_page);
    }
    if (state_page) {
        pmm_free_block((void*)state_page);
    }
    if (task) {
        if (task->kernel_stack_top) {
            kfree((void*)(task->kernel_stack_top - KERNEL_STACK_SIZE));
        }
        kfree(task);
    }
}

static void task_enqueue(task_t* task) {
    if (!ready_queue_head) {
        ready_queue_head = task;
        ready_queue_tail = task;
        task->next = task;
        return;
    }

    task->next = ready_queue_head;
    ready_queue_tail->next = task;
    ready_queue_tail = task;
}

static void task_build_initial_frame(task_t* task, uint32_t entry,
                                     uint32_t user_stack_top, int user_mode) {
    uint32_t* stack = (uint32_t*)task->kernel_stack_top;

    if (!user_mode) {
        push_word(&stack, (uint32_t)task_exit);
        push_word(&stack, 0x202);
        push_word(&stack, 0x08);
        push_word(&stack, entry);
    } else {
        push_word(&stack, 0x23);
        push_word(&stack, user_stack_top);
        push_word(&stack, 0x202);
        push_word(&stack, 0x1B);
        push_word(&stack, entry);
    }

    push_word(&stack, 0);
    push_word(&stack, 32);
    push_word(&stack, 0); /* EAX */
    push_word(&stack, 0); /* ECX */
    push_word(&stack, 0); /* EDX */
    push_word(&stack, 0); /* EBX */
    push_word(&stack, 0); /* ESP placeholder */
    push_word(&stack, 0); /* EBP */
    push_word(&stack, 0); /* ESI */
    push_word(&stack, 0); /* EDI */
    push_word(&stack, 0x10); /* Saved data-segment selector */
    task->esp = (uint32_t)stack;
}

void tasking_install(void) {
    task_t* bootstrap = (task_t*)kmalloc(sizeof(task_t));
    if (!bootstrap) {
        return;
    }

    memset(bootstrap, 0, sizeof(*bootstrap));
    bootstrap->pid = 0;
    bootstrap->state = TASK_RUNNING;
    bootstrap->page_directory = paging_current_address_space();
    bootstrap->next = bootstrap;
    current_task = bootstrap;
    ready_queue_head = bootstrap;
    ready_queue_tail = bootstrap;
}

task_t* task_create_kernel(void (*entry_point)(void)) {
    if (!ready_queue_head || !entry_point) {
        return 0;
    }

    task_t* task = task_allocate();
    if (!task) {
        return 0;
    }

    task->pid = next_pid++;
    task_build_initial_frame(task, (uint32_t)entry_point, 0, 0);
    task_enqueue(task);
    return task;
}

task_t* task_create_user(const void* code, size_t code_size, uint32_t pid) {
    if (!ready_queue_head || !code || !code_size || code_size > PAGE_SIZE) {
        return 0;
    }

    task_t* task = task_allocate();
    if (!task) {
        return 0;
    }

    uint32_t code_page = (uint32_t)pmm_alloc_block();
    uint32_t stack_page = (uint32_t)pmm_alloc_block();
    uint32_t state_page = (uint32_t)pmm_alloc_block();
    uint32_t* address_space = paging_create_address_space();
    if (!code_page || !stack_page || !state_page || !address_space ||
        code_page >= IDENTITY_MAP_SIZE || stack_page >= IDENTITY_MAP_SIZE ||
        state_page >= IDENTITY_MAP_SIZE) {
        task_discard_unqueued(task, code_page, stack_page, state_page,
                              address_space);
        return 0;
    }

    memset((void*)code_page, 0, PAGE_SIZE);
    memset((void*)stack_page, 0, PAGE_SIZE);
    memset((void*)state_page, 0, PAGE_SIZE);
    memcpy((void*)code_page, code, code_size);

    volatile ring3_test_state_t* user_state =
        (volatile ring3_test_state_t*)state_page;
    user_state->result = RING3_TEST_RUNNING;
    user_state->pid = pid;

    if (!paging_map_user_page_in(address_space, USER_CODE_ADDRESS,
                                 code_page, 0) ||
        !paging_map_user_page_in(address_space, USER_STACK_ADDRESS,
                                 stack_page, 1) ||
        !paging_map_user_page_in(address_space, USER_STATE_ADDRESS,
                                 state_page, 1)) {
        task_discard_unqueued(task, code_page, stack_page, state_page,
                              address_space);
        return 0;
    }

    task->pid = (int)pid;
    task->page_directory = address_space;
    task->user_test_state = user_state;
    task_build_initial_frame(task, USER_CODE_ADDRESS, USER_STACK_TOP, 1);
    task_enqueue(task);
    return task;
}

static int build_elf_user_stack(uint32_t stack_page, int argc,
                                const char* const argv[],
                                uint32_t* initial_stack) {
    if (argc < 0 || argc > 16 || (argc && !argv)) {
        return 0;
    }

    uint32_t cursor = ELF_USER_STACK_TOP;
    uint32_t user_argv[16];
    for (int i = argc - 1; i >= 0; i--) {
        if (!argv[i]) {
            return 0;
        }
        size_t length = 0;
        while (length < 256 && argv[i][length]) {
            length++;
        }
        if (length == 256 || cursor < ELF_USER_STACK_ADDRESS + length + 1) {
            return 0;
        }
        cursor -= (uint32_t)length + 1;
        memcpy((void*)(stack_page + cursor - ELF_USER_STACK_ADDRESS),
               argv[i], length + 1);
        user_argv[i] = cursor;
    }

    uint32_t stack_words = (uint32_t)argc + 5U;
    if (cursor < ELF_USER_STACK_ADDRESS +
                     stack_words * sizeof(uint32_t)) {
        return 0;
    }
    uint32_t aligned_stack =
        (cursor - stack_words * sizeof(uint32_t)) & ~15U;
    if (aligned_stack < ELF_USER_STACK_ADDRESS) {
        return 0;
    }
    uint32_t* stack = (uint32_t*)(stack_page + aligned_stack -
                                  ELF_USER_STACK_ADDRESS +
                                  stack_words * sizeof(uint32_t));
    push_word(&stack, 0); /* AT_NULL value */
    push_word(&stack, 0); /* AT_NULL type */
    push_word(&stack, 0); /* envp terminator */
    push_word(&stack, 0); /* argv terminator */
    for (int i = argc - 1; i >= 0; i--) {
        push_word(&stack, user_argv[i]);
    }
    push_word(&stack, (uint32_t)argc);
    *initial_stack = ELF_USER_STACK_ADDRESS +
        ((uint32_t)stack - stack_page);
    return *initial_stack == aligned_stack;
}

int task_create_elf(const void* image, size_t image_size, int argc,
                    const char* const argv[], int parent_pid,
                    int* process_id) {
    if (!ready_queue_head || !image || image_size < sizeof(elf32_header_t)) {
        return 0;
    }
    if (argc < 0 || argc > 16 || (argc && !argv)) {
        return 0;
    }

    elf32_header_t header;
    memcpy(&header, image, sizeof(header));
    if (header.ident[0] != 0x7F || header.ident[1] != 'E' ||
        header.ident[2] != 'L' || header.ident[3] != 'F' ||
        header.ident[4] != 1 || header.ident[5] != 1 ||
        header.ident[6] != 1 || header.type != 2 || header.machine != 3 ||
        header.version != 1 || header.header_size != sizeof(header) ||
        header.program_header_size != sizeof(elf32_program_header_t) ||
        !header.program_header_count ||
        (uint64_t)header.program_header_offset +
            (uint64_t)header.program_header_count *
                sizeof(elf32_program_header_t) > image_size) {
        return 0;
    }

    task_t* task = task_allocate();
    if (!task) {
        return 0;
    }
    uint32_t* address_space = paging_create_address_space();
    if (!address_space) {
        task_discard_unqueued(task, 0, 0, 0, 0);
        return 0;
    }

    int has_loadable_segment = 0;
    int entry_is_executable = 0;
    uint32_t image_end = USER_CODE_ADDRESS;
    for (uint32_t i = 0; i < header.program_header_count; i++) {
        elf32_program_header_t program;
        size_t offset = header.program_header_offset +
            i * sizeof(program);
        memcpy(&program, (const uint8_t*)image + offset, sizeof(program));
        if (program.type != ELF_PT_LOAD) {
            continue;
        }

        uint64_t file_end = (uint64_t)program.offset + program.file_size;
        uint64_t memory_end =
            (uint64_t)program.virtual_address + program.memory_size;
        int alignment_valid = program.alignment <= 1 ||
            (!(program.alignment & (program.alignment - 1)) &&
             program.offset % program.alignment ==
                 program.virtual_address % program.alignment);
        if (program.file_size > program.memory_size ||
            file_end > image_size || memory_end > ELF_USER_LIMIT ||
            !alignment_valid ||
            (program.memory_size &&
             program.virtual_address < USER_CODE_ADDRESS)) {
            task_discard_unqueued(task, 0, 0, 0, address_space);
            return 0;
        }
        if (!program.memory_size) {
            continue;
        }
        has_loadable_segment = 1;
        if (memory_end > image_end) {
            image_end = (uint32_t)memory_end;
        }
        if ((program.flags & ELF_PF_X) &&
            header.entry >= program.virtual_address &&
            header.entry < memory_end) {
            entry_is_executable = 1;
        }

        uint32_t page = program.virtual_address & ~(PAGE_SIZE - 1);
        uint32_t last_page = ((uint32_t)memory_end - 1) & ~(PAGE_SIZE - 1);
        for (;;) {
            uint32_t physical =
                paging_user_page_physical_in(address_space, page);
            if (!physical) {
                physical = (uint32_t)pmm_alloc_block();
                if (!physical || physical >= IDENTITY_MAP_SIZE) {
                    if (physical) {
                        pmm_free_block((void*)physical);
                    }
                    task_discard_unqueued(task, 0, 0, 0, address_space);
                    return 0;
                }
                memset((void*)physical, 0, PAGE_SIZE);
                if (!paging_map_user_page_in(
                        address_space, page, physical,
                        (program.flags & ELF_PF_W) != 0)) {
                    pmm_free_block((void*)physical);
                    task_discard_unqueued(task, 0, 0, 0, address_space);
                    return 0;
                }
            } else if ((program.flags & ELF_PF_W) &&
                       !paging_map_user_page_in(address_space, page,
                                                physical, 1)) {
                task_discard_unqueued(task, 0, 0, 0, address_space);
                return 0;
            }

            if (page == last_page) {
                break;
            }
            page += PAGE_SIZE;
        }
    }

    if (!has_loadable_segment || !entry_is_executable) {
        task_discard_unqueued(task, 0, 0, 0, address_space);
        return 0;
    }

    for (uint32_t i = 0; i < header.program_header_count; i++) {
        elf32_program_header_t program;
        size_t offset = header.program_header_offset +
            i * sizeof(program);
        memcpy(&program, (const uint8_t*)image + offset, sizeof(program));
        if (program.type != ELF_PT_LOAD || !program.file_size) {
            continue;
        }

        uint32_t destination = program.virtual_address;
        const uint8_t* source = (const uint8_t*)image + program.offset;
        uint32_t remaining = program.file_size;
        while (remaining) {
            uint32_t physical =
                paging_user_page_physical_in(address_space, destination);
            uint32_t page_offset = destination & (PAGE_SIZE - 1);
            uint32_t chunk = PAGE_SIZE - page_offset;
            if (chunk > remaining) {
                chunk = remaining;
            }
            if (!physical) {
                task_discard_unqueued(task, 0, 0, 0, address_space);
                return 0;
            }
            memcpy((void*)(physical + page_offset), source, chunk);
            destination += chunk;
            source += chunk;
            remaining -= chunk;
        }
    }

    uint32_t stack_page = (uint32_t)pmm_alloc_block();
    if (!stack_page || stack_page >= IDENTITY_MAP_SIZE) {
        if (stack_page) {
            pmm_free_block((void*)stack_page);
        }
        task_discard_unqueued(task, 0, 0, 0, address_space);
        return 0;
    }
    memset((void*)stack_page, 0, PAGE_SIZE);
    if (!paging_map_user_page_in(address_space, ELF_USER_STACK_ADDRESS,
                                 stack_page, 1)) {
        pmm_free_block((void*)stack_page);
        task_discard_unqueued(task, 0, 0, 0, address_space);
        return 0;
    }

    uint32_t initial_stack;
    if (!build_elf_user_stack(stack_page, argc, argv, &initial_stack)) {
        task_discard_unqueued(task, 0, stack_page, 0, address_space);
        return 0;
    }

    task->pid = next_pid++;
    task->parent_pid = parent_pid;
    task->page_directory = address_space;
    task->heap_start = (image_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    task->heap_break = task->heap_start;
    if (task->heap_start >= ELF_HEAP_LIMIT) {
        task_discard_unqueued(task, 0, stack_page, 0, address_space);
        return 0;
    }
    if (process_id) {
        *process_id = task->pid;
    }
    task_build_initial_frame(task, header.entry, initial_stack, 1);
    task_enqueue(task);
    return 1;
}

static task_t* task_find(int pid) {
    if (!ready_queue_head) {
        return 0;
    }
    task_t* task = ready_queue_head;
    do {
        if (task->pid == pid) {
            return task;
        }
        task = task->next;
    } while (task != ready_queue_head);
    return 0;
}

static int task_exit_history_find(int pid, int parent_pid, int* status) {
    for (uint32_t i = 0; i < TASK_EXIT_HISTORY_SIZE; i++) {
        if (exit_history[i].pid == pid &&
            exit_history[i].parent_pid == parent_pid) {
            if (status) {
                *status = exit_history[i].status;
            }
            return 1;
        }
    }
    return 0;
}

void task_process_exit(int status) {
    if (!current_task || current_task->pid == 0) {
        return;
    }

    current_task->exit_status = status;
    current_task->state = TASK_ZOMBIE;
    task_exit_record_t* record =
        &exit_history[exit_history_next++ % TASK_EXIT_HISTORY_SIZE];
    record->pid = current_task->pid;
    record->parent_pid = current_task->parent_pid;
    record->status = status;

    if (!ready_queue_head) {
        return;
    }
    task_t* waiter = ready_queue_head;
    do {
        if (waiter->state == TASK_BLOCKED &&
            waiter->wait_kind == TASK_WAIT_PROCESS &&
            waiter->wait_pid == current_task->pid) {
            int success = waiter->pid == current_task->parent_pid &&
                (!waiter->wait_status_address ||
                 paging_copy_to_user_in(waiter->page_directory,
                                        waiter->wait_status_address,
                                        &status, sizeof(status)));
            regs_t* registers = (regs_t*)waiter->esp;
            if (registers) {
                registers->eax = success ? (uint32_t)current_task->pid
                                          : (uint32_t)-1;
            }
            waiter->wait_kind = TASK_WAIT_NONE;
            waiter->state = TASK_READY;
        }
        waiter = waiter->next;
    } while (waiter != ready_queue_head);
}

int task_wait_process(int pid, uint32_t status_address) {
    if (!current_task || pid <= 0 ||
        (status_address &&
         !paging_user_range_writable(status_address, sizeof(int)))) {
        return -1;
    }

    task_t* target = task_find(pid);
    if (target) {
        if (target->parent_pid != current_task->pid ||
            target->pid == current_task->pid) {
            return -1;
        }
        if (target->state == TASK_ZOMBIE) {
            if (status_address &&
                !paging_copy_to_user_in(current_task->page_directory,
                                        status_address,
                                        &target->exit_status,
                                        sizeof(target->exit_status))) {
                return -1;
            }
            return pid;
        }
        current_task->wait_pid = pid;
        current_task->wait_status_address = status_address;
        current_task->wait_kind = TASK_WAIT_PROCESS;
        current_task->state = TASK_BLOCKED;
        return -2;
    }

    int status;
    if (task_exit_history_find(pid, current_task->pid, &status)) {
        if (status_address &&
            !paging_copy_to_user_in(current_task->page_directory,
                                    status_address, &status, sizeof(status))) {
            return -1;
        }
        return pid;
    }
    return -1;
}

int task_process_status(int pid, int* status) {
    task_t* task = task_find(pid);
    if (task) {
        if (task->state != TASK_ZOMBIE) {
            return 0;
        }
        if (status) {
            *status = task->exit_status;
        }
        return 1;
    }
    for (uint32_t i = 0; i < TASK_EXIT_HISTORY_SIZE; i++) {
        if (exit_history[i].pid == pid) {
            if (status) {
                *status = exit_history[i].status;
            }
            return 1;
        }
    }
    return -1;
}

static void task_deliver_stdin(task_t* task) {
    if (!task || task->state != TASK_BLOCKED ||
        task->wait_kind != TASK_WAIT_STDIN || !task->stdin_ready) {
        return;
    }

    uint32_t amount = task->stdin_ready;
    if (amount > task->wait_length) {
        amount = task->wait_length;
    }
    char buffer[128];
    for (uint32_t i = 0; i < amount; i++) {
        buffer[i] = task->stdin_buffer[task->stdin_head];
        task->stdin_head = (task->stdin_head + 1) % sizeof(task->stdin_buffer);
    }
    task->stdin_count -= amount;
    task->stdin_ready -= amount;
    regs_t* registers = (regs_t*)task->esp;
    if (!paging_copy_to_user_in(task->page_directory, task->wait_buffer,
                                buffer, amount)) {
        if (registers) {
            registers->eax = (uint32_t)-1;
        }
    } else if (registers) {
        registers->eax = amount;
    }
    task->wait_kind = TASK_WAIT_NONE;
    task->state = TASK_READY;
}

int task_read_stdin(uint32_t user_buffer, uint32_t length) {
    if (!current_task || !length) {
        return length ? -1 : 0;
    }
    if (current_task->stdin_ready) {
        uint32_t amount = current_task->stdin_ready;
        if (amount > length) {
            amount = length;
        }
        char buffer[128];
        for (uint32_t i = 0; i < amount; i++) {
            buffer[i] = current_task->stdin_buffer[current_task->stdin_head];
            current_task->stdin_head =
                (current_task->stdin_head + 1) %
                sizeof(current_task->stdin_buffer);
        }
        current_task->stdin_count -= amount;
        current_task->stdin_ready -= amount;
        return paging_copy_to_user_in(current_task->page_directory,
                                      user_buffer, buffer, amount)
            ? (int)amount
            : -1;
    }

    current_task->wait_buffer = user_buffer;
    current_task->wait_length = length;
    current_task->wait_kind = TASK_WAIT_STDIN;
    current_task->state = TASK_BLOCKED;
    return -2;
}

void task_stdin_feed(int pid, char character) {
    task_t* task = task_find(pid);
    if (!task || task->state == TASK_ZOMBIE) {
        return;
    }
    if (character == '\b') {
        if (task->stdin_count > task->stdin_ready) {
            task->stdin_count--;
        }
        return;
    }
    if (task->stdin_count == sizeof(task->stdin_buffer)) {
        return;
    }

    uint32_t tail = (task->stdin_head + task->stdin_count) %
        sizeof(task->stdin_buffer);
    task->stdin_buffer[tail] = character;
    task->stdin_count++;
    if (character == '\n') {
        task->stdin_ready = task->stdin_count;
        task_deliver_stdin(task);
    }
}

static void reap_elf_task(task_t* task) {
    if (!task || task->pid < 4 || task == current_task ||
        !ready_queue_head) {
        return;
    }

    task_t* previous = ready_queue_head;
    do {
        if (previous->next == task) {
            previous->next = task->next;
            if (ready_queue_head == task) {
                ready_queue_head = task->next;
            }
            if (ready_queue_tail == task) {
                ready_queue_tail = previous;
            }
            if (task->kernel_stack_top) {
                kfree((void*)(task->kernel_stack_top - KERNEL_STACK_SIZE));
            }
            kfree(task);
            return;
        }
        previous = previous->next;
    } while (previous != ready_queue_head);
}

void task_switch(void) {
    if (!current_task || !current_task->next) {
        return;
    }

    task_t* previous = current_task;
    if (reap_pending && reap_pending != previous) {
        task_t* task = reap_pending;
        reap_pending = 0;
        reap_elf_task(task);
    }
    if (previous->state == TASK_RUNNING) {
        previous->state = TASK_READY;
    }

    task_t* next_task = previous->next;
    while (next_task != previous &&
           (next_task->state == TASK_ZOMBIE ||
            next_task->state == TASK_BLOCKED)) {
        next_task = next_task->next;
    }

    if (next_task == previous &&
        (next_task->state == TASK_ZOMBIE ||
         next_task->state == TASK_BLOCKED)) {
        return;
    }

    current_task = next_task;
    current_task->state = TASK_RUNNING;
    paging_activate_address_space(current_task->page_directory);
    gdt_set_kernel_stack(current_task->kernel_stack_top);
    if (previous != current_task && previous->state == TASK_ZOMBIE &&
        previous->pid != 0 && previous->page_directory &&
        previous->page_directory != current_task->page_directory) {
        paging_destroy_address_space(previous->page_directory);
        previous->page_directory = 0;
        previous->user_test_state = 0;
        if (previous->pid >= 4) {
            reap_pending = previous;
        }
    }
}

extern void task_start_asm(void);

void task_start(void) {
    if (!current_task) {
        return;
    }

    current_task->state = TASK_ZOMBIE;
    task_start_asm();
    for (;;) {
        asm volatile("cli; hlt");
    }
}

void task_yield(void) {
    asm volatile("int $0x81" : : : "memory");
}
