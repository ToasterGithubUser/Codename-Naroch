#include <kernel/task.h>
#include <kernel/heap.h>
#include <kernel/idt.h>

task_t* current_task = 0;
static task_t* ready_queue_head = 0;
static int next_pid = 1;

static void task_exit(void) {
    if (current_task) {
        current_task->state = TASK_ZOMBIE;
    }

    for (;;) {
        asm volatile("hlt");
    }
}

void tasking_install(void) {
    task_t* main_task = (task_t*)kmalloc(sizeof(task_t));
    if (!main_task) {
        return;
    }

    main_task->pid = 0;
    main_task->esp = 0;
    main_task->stack_start = 0;
    main_task->state = TASK_RUNNING;
    main_task->next = main_task;

    current_task = main_task;
    ready_queue_head = main_task;
}

task_t* task_create(void (*entry_point)(void)) {
    if (!ready_queue_head || !entry_point) {
        return 0;
    }

    task_t* new_task = (task_t*)kmalloc(sizeof(task_t));
    if (!new_task) {
        return 0;
    }

    uint32_t* stack = (uint32_t*)kmalloc(4096);
    if (!stack) {
        kfree(new_task);
        return 0;
    }

    new_task->pid = next_pid++;
    new_task->state = V_TASK_READY;

    new_task->stack_start = (uint32_t)stack;
    uint32_t* esp = (uint32_t*)((uint32_t)stack + 4096);

    *(--esp) = (uint32_t)task_exit;
    *(--esp) = 0x0202;
    *(--esp) = 0x08;
    *(--esp) = (uint32_t)entry_point;
    *(--esp) = 0; /* Error code */
    *(--esp) = 0; /* Interrupt number */
    *(--esp) = 0; /* EAX */
    *(--esp) = 0; /* ECX */
    *(--esp) = 0; /* EDX */
    *(--esp) = 0; /* EBX */
    *(--esp) = 0; /* ESP placeholder */
    *(--esp) = 0; /* EBP */
    *(--esp) = 0; /* ESI */
    *(--esp) = 0; /* EDI */
    *(--esp) = 0x10; /* Saved data-segment selector */

    new_task->esp = (uint32_t)esp;
    new_task->next = ready_queue_head->next;
    ready_queue_head->next = new_task;

    return new_task;
}

void task_switch(void) {
    if (!current_task || !current_task->next) {
        return;
    }

    if (current_task->state == TASK_RUNNING) {
        current_task->state = V_TASK_READY;
    }

    task_t* next_task = current_task->next;
    while (next_task != current_task && next_task->state == TASK_ZOMBIE) {
        next_task = next_task->next;
    }

    if (next_task->state != TASK_ZOMBIE) {
        current_task = next_task;
        current_task->state = TASK_RUNNING;
    } else {
        current_task->state = TASK_RUNNING;
    }
}
