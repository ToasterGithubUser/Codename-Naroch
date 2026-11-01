#ifndef _KERNEL_MUTEX_H
#define _KERNEL_MUTEX_H

#include <stdint.h>

// Структура мьютекса ядра
typedef struct {
    volatile uint32_t lock; // 0 — свободен, 1 — занят
    int holder_pid;         // PID потока, который сейчас держит блокировку
} mutex_t;

// Инициализация мьютекса
void mutex_init(mutex_t* mutex);

// Блокировка (захват ресурса). Если занято — поток крутится в ожидании (Spinlock)
void mutex_lock(mutex_t* mutex);

// Разблокировка (освобождение ресурса)
void mutex_unlock(mutex_t* mutex);

#endif
