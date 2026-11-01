#include <kernel/mutex.h>
#include <kernel/task.h>

// Импортируем указатель на текущую задачу, чтобы знать, кто захватил мьютекс
extern task_t* current_task;

void mutex_init(mutex_t* mutex) {
    mutex->lock = 0;
    mutex->holder_pid = -1;
}

void mutex_lock(mutex_t* mutex) {
    uint32_t value = 1;

    // Цикл Spinlock: атомарно меняем значение памяти на 1.
    // Если на выходе value оказалась равна 0, значит до этого момент мьютекс был свободен,
    // и мы его успешно захватили! Если 1 — продолжаем крутиться в цикле.
    while (value == 1) {
        asm volatile(
            "xchgl %0, %1"
            : "=r"(value), "+m"(mutex->lock)
            : "0"(value)
            : "memory"
        );
        
        if (value == 1) {
            // Чтобы не перегревать процессор в пустом Spinlock-цикле,
            // даем ему инструкцию паузы конвейера или усыпляем до следующего тика PIT
            asm volatile("pause"); 
        }
    }

    // Мьютекс наш! Запоминаем, кто им владеет
    if (current_task) {
        mutex->holder_pid = current_task->pid;
    }
}

void mutex_unlock(mutex_t* mutex) {
    mutex->holder_pid = -1;
    
    // Атомарно сбрасываем флаг блокировки обратно в 0
    uint32_t value = 0;
    asm volatile(
        "xchgl %0, %1"
        : "=r"(value), "+m"(mutex->lock)
        : "0"(value)
        : "memory"
    );
}
