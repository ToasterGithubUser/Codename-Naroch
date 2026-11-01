#ifndef _KERNEL_TASK_H
#define _KERNEL_TASK_H

#include <stdint.h>
#include <stddef.h>

// Состояние потока ядра
typedef enum {
    TASK_RUNNING,
    V_TASK_READY,
    TASK_ZOMBIE
} task_state_t;

// Структура PCB (Process Control Block)
typedef struct task {
    int pid;                    // Уникальный ID процесса
    uint32_t esp;               // Текущий указатель стека задачи (куда сохранен контекст x86)
    uint32_t stack_start;       // Начальный адрес выделенного стека (для kfree)
    task_state_t state;         // Состояние задачи
    struct task* next;          // Ссылка на следующую задачу в кольцевом списке
} task_t;

// Интерфейс планировщика
extern task_t* current_task;
void tasking_install(void);
task_t* task_create(void (*entry_point)(void));
void task_switch(void);

#endif
